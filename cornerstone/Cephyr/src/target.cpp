#include <cephyr/target.hpp>

#include <cephyr/diagnostics.hpp>

#include "KaguyaCompat.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <set>
#include <string_view>
#include <system_error>
#include <utility>

namespace cephyr {
namespace {

constexpr std::size_t max_script_bytes = 64 * 1024;
constexpr std::size_t max_lua_bytes = 2 * 1024 * 1024;
constexpr unsigned max_lua_instructions = 100000;
constexpr unsigned hook_interval = 1000;
constexpr std::size_t max_array_entries = 64;
constexpr std::size_t max_setting_bytes = 4096;

struct LuaLimits {
    std::size_t allocated = 0;
    std::size_t maximum = max_lua_bytes;
    unsigned instructions = 0;
};

void *limited_reallocate(void *opaque, void *pointer, std::size_t old_size,
                         std::size_t new_size) noexcept {
    auto &limits = *static_cast<LuaLimits *>(opaque);
    // Lua uses osize as a type tag, not a byte count, for fresh allocations.
    const std::size_t tracked_old_size = pointer ? old_size : 0;
    if (new_size == 0) {
        std::free(pointer);
        limits.allocated -= std::min(limits.allocated, tracked_old_size);
        return nullptr;
    }

    const std::size_t accounted_old_size =
        std::min(limits.allocated, tracked_old_size);
    const std::size_t retained = limits.allocated - accounted_old_size;
    if (new_size > limits.maximum - std::min(limits.maximum, retained))
        return nullptr;

    void *replacement = std::realloc(pointer, new_size);
    if (!replacement)
        return nullptr;
    limits.allocated = retained + new_size;
    return replacement;
}

void instruction_hook(lua_State *state, lua_Debug *) {
    auto *limits = *static_cast<LuaLimits **>(lua_getextraspace(state));
    if (!limits)
        return;
    limits->instructions += hook_interval;
    if (limits->instructions > max_lua_instructions)
        luaL_error(state, "target script exceeded its instruction limit");
}

bool valid_triple(const std::string &triple) {
    if (triple.empty() || triple.size() > 128)
        return false;
    unsigned components = 1;
    std::size_t component_start = 0;
    for (std::size_t index = 0; index <= triple.size(); ++index) {
        if (index != triple.size() && triple[index] != '-') {
            const unsigned char character =
                static_cast<unsigned char>(triple[index]);
            if (!std::isalnum(character) && character != '_' &&
                character != '.' && character != '+')
                return false;
            continue;
        }
        const std::string_view component(triple.data() + component_start,
                                         index - component_start);
        if (component.empty() || component == "." || component == "..")
            return false;
        if (!std::isalnum(static_cast<unsigned char>(component.front())) ||
            !std::isalnum(static_cast<unsigned char>(component.back())))
            return false;
        if (index != triple.size()) {
            ++components;
            component_start = index + 1;
        }
    }
    return components >= 3 && components <= 4;
}

bool triple_has_component(const std::string &triple, std::string_view wanted) {
    std::size_t start = 0;
    while (start <= triple.size()) {
        const std::size_t end = triple.find('-', start);
        const std::size_t length =
            end == std::string::npos ? triple.size() - start : end - start;
        if (std::string_view(triple.data() + start, length) == wanted)
            return true;
        if (end == std::string::npos)
            break;
        start = end + 1;
    }
    return false;
}

unsigned lua_line(const std::string &message) {
    // Lua text chunks report "...:line: message". Take the final numeric
    // colon-delimited field so paths containing ':' remain harmless.
    std::size_t cursor = 0;
    unsigned result = 1;
    while ((cursor = message.find(':', cursor)) != std::string::npos) {
        const std::size_t begin = cursor + 1;
        std::size_t end = begin;
        while (end < message.size() &&
               std::isdigit(static_cast<unsigned char>(message[end])))
            ++end;
        if (end > begin && end < message.size() && message[end] == ':') {
            try {
                const unsigned long parsed =
                    std::stoul(message.substr(begin, end - begin));
                result = parsed <= std::numeric_limits<unsigned>::max()
                             ? static_cast<unsigned>(parsed)
                             : 1;
            } catch (...) {
                return 1;
            }
        }
        ++cursor;
    }
    return result;
}

void report(Diagnostics &diagnostics, std::string_view code,
            const std::string &message, const std::filesystem::path &file,
            unsigned line = 1) {
    diagnostics.error(std::string(code), message,
                      Location{file.string(), line, 1});
}

std::optional<std::string> read_script(const std::filesystem::path &path,
                                       std::string &reason) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error) {
        reason = "cannot read target script '" + path.string() + "': " +
                 error.message();
        return std::nullopt;
    }
    if (size > max_script_bytes) {
        reason = "target script exceeds the 64 KiB input limit";
        return std::nullopt;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        reason = "cannot open target script '" + path.string() + "'";
        return std::nullopt;
    }
    std::string source(static_cast<std::size_t>(size), '\0');
    input.read(source.data(), static_cast<std::streamsize>(source.size()));
    if (!input && !source.empty()) {
        reason = "cannot read complete target script '" + path.string() + "'";
        return std::nullopt;
    }
    return source;
}

bool valid_setting_string(const std::string &value, bool allow_empty = false) {
    if ((!allow_empty && value.empty()) || value.size() > max_setting_bytes)
        return false;
    return std::none_of(value.begin(), value.end(), [](unsigned char character) {
        return character == '\0' || character == '\n' || character == '\r' ||
               character < 0x20 || character == 0x7f;
    });
}

bool validate_object(const kaguya::LuaRef &object,
                     const std::set<std::string> &allowed, std::string &reason) {
    lua_State *state = object.state();
    kaguya::util::ScopedSavedStack saved(state);
    object.push(state);
    const int table = lua_absindex(state, -1);
    if (lua_getmetatable(state, table)) {
        reason = "target table must not have a metatable";
        return false;
    }
    lua_pushnil(state);
    while (lua_next(state, table) != 0) {
        if (lua_type(state, -2) != LUA_TSTRING) {
            reason = "target table keys must be strings";
            return false;
        }
        std::size_t length = 0;
        const char *text = lua_tolstring(state, -2, &length);
        const std::string key(text, length);
        if (!valid_setting_string(key)) {
            reason = "target table contains an invalid setting name";
            return false;
        }
        if (allowed.find(key) == allowed.end()) {
            reason = "unknown target setting '" + key + "'";
            return false;
        }
        lua_pop(state, 1);
    }
    return true;
}

bool required_string(const kaguya::LuaRef &object, const char *name,
                     std::string &result, std::string &reason) {
    auto value = object.getField(name);
    if (!value.isType<std::string>()) {
        reason = std::string("target setting '") + name + "' must be a string";
        return false;
    }
    result = value.get<std::string>();
    if (!valid_setting_string(result)) {
        reason = std::string("target setting '") + name +
                 "' is empty, too long, or contains control characters";
        return false;
    }
    return true;
}

bool required_array(const kaguya::LuaRef &object, const char *name,
                    bool allow_empty, std::vector<std::string> &result,
                    std::string &reason) {
    auto value = object.getField(name);
    if (!value.isType<kaguya::LuaTable>()) {
        reason = std::string("target setting '") + name +
                 "' must be an array of strings";
        return false;
    }

    lua_State *state = object.state();
    {
        kaguya::util::ScopedSavedStack saved(state);
        value.push(state);
        const int table = lua_absindex(state, -1);
        if (lua_getmetatable(state, table)) {
            reason = std::string("target setting '") + name +
                     "' must not have a metatable";
            return false;
        }
        const std::size_t length = lua_rawlen(state, table);
        if ((!allow_empty && length == 0) || length > max_array_entries) {
            reason = std::string("target setting '") + name +
                     (allow_empty ? "' has too many entries"
                                  : "' must contain between 1 and 64 entries");
            return false;
        }
        std::vector<bool> present(length, false);
        lua_pushnil(state);
        while (lua_next(state, table) != 0) {
            if (!lua_isinteger(state, -2)) {
                reason = std::string("target setting '") + name +
                         "' must be a dense array";
                return false;
            }
            const lua_Integer index = lua_tointeger(state, -2);
            if (index < 1 ||
                static_cast<unsigned long long>(index) > length) {
                reason = std::string("target setting '") + name +
                         "' must be a dense array";
                return false;
            }
            if (lua_type(state, -1) != LUA_TSTRING) {
                reason = std::string("target setting '") + name +
                         "' must contain only strings";
                return false;
            }
            present[static_cast<std::size_t>(index - 1)] = true;
            lua_pop(state, 1);
        }
        if (std::find(present.begin(), present.end(), false) != present.end()) {
            reason = std::string("target setting '") + name +
                     "' must be a dense array";
            return false;
        }
    }

    result = value.get<std::vector<std::string>>();
    for (const std::string &entry : result) {
        if (!valid_setting_string(entry)) {
            reason = std::string("target setting '") + name +
                     "' contains an empty, oversized, or control-bearing entry";
            return false;
        }
    }
    return true;
}

std::optional<Target> parse_target(const kaguya::LuaRef &object,
                                   const std::string &requested,
                                   const std::filesystem::path &script,
                                   const std::filesystem::path &archon_root,
                                   std::string &reason) {
    static const std::set<std::string> allowed{
        "triple",       "architecture", "abi",          "os",
        "specification", "assembler",    "linker",       "archiver",
        "assemble_flags", "link_flags",
    };
    if (!object.isType<kaguya::LuaTable>()) {
        reason = "target script must return a table";
        return std::nullopt;
    }
    if (!validate_object(object, allowed, reason))
        return std::nullopt;

    Target target;
    std::string operating_system;
    std::string specification;
    if (!required_string(object, "triple", target.triple, reason) ||
        !required_string(object, "architecture", target.architecture, reason) ||
        !required_string(object, "abi", target.abi, reason) ||
        !required_string(object, "os", operating_system, reason) ||
        !required_string(object, "specification", specification, reason) ||
        !required_array(object, "assembler", false, target.assembler, reason) ||
        !required_array(object, "linker", false, target.linker, reason) ||
        !required_array(object, "archiver", false, target.archiver, reason) ||
        !required_array(object, "assemble_flags", true, target.assemble_flags,
                        reason) ||
        !required_array(object, "link_flags", true, target.link_flags, reason))
        return std::nullopt;

    if (!valid_triple(target.triple)) {
        reason = "target script returned an invalid target triple";
        return std::nullopt;
    }
    if (!requested.empty() && target.triple != requested) {
        reason = "target script describes '" + target.triple +
                 "', not requested triple '" + requested + "'";
        return std::nullopt;
    }
    if (operating_system != "linux" && operating_system != "none") {
        reason = "target setting 'os' must be 'linux' or 'none'";
        return std::nullopt;
    }
    target.is_linux = operating_system == "linux";
    if (target.is_linux != triple_has_component(target.triple, "linux")) {
        reason = "target setting 'os' is inconsistent with the target triple";
        return std::nullopt;
    }
    constexpr std::string_view archon_prefix = "@archon/";
    std::filesystem::path resolved;
    if (std::string_view(specification).substr(0, archon_prefix.size()) ==
        archon_prefix) {
        const std::filesystem::path suffix =
            specification.substr(archon_prefix.size());
        if (archon_root.empty() || suffix.empty() || suffix.is_absolute()) {
            reason = "invalid @archon specification path";
            return std::nullopt;
        }
        auto normalized_root = archon_root.lexically_normal();
        if (normalized_root != normalized_root.root_path() &&
            normalized_root.filename().empty())
            normalized_root = normalized_root.parent_path();
        resolved = (normalized_root / suffix).lexically_normal();
        auto root_it = normalized_root.begin();
        auto path_it = resolved.begin();
        for (; root_it != normalized_root.end() && path_it != resolved.end();
             ++root_it, ++path_it) {
            if (*root_it != *path_it)
                break;
        }
        if (root_it != normalized_root.end()) {
            reason = "@archon specification path escapes the Archon root";
            return std::nullopt;
        }
    } else {
        const std::filesystem::path configured(specification);
        resolved = configured.is_absolute()
                       ? configured.lexically_normal()
                       : (script.parent_path() / configured).lexically_normal();
    }
    if (resolved.extension() != ".arch") {
        reason = "target specification must name an .arch file";
        return std::nullopt;
    }
    std::error_code error;
    if (!std::filesystem::is_regular_file(resolved, error)) {
        reason = "target specification '" + resolved.string() +
                 "' is not a readable regular file";
        return std::nullopt;
    }
    target.specification = std::move(resolved);
    return target;
}

} // namespace

std::optional<Target> load_target(const std::string &triple,
                                  const std::filesystem::path &bank,
                                  const std::filesystem::path &script,
                                  const std::filesystem::path &archon_root,
                                  Diagnostics &diagnostics) {
    std::filesystem::path selected_script;
    if (!script.empty()) {
        selected_script = script;
    } else {
        const std::filesystem::path selected_bank =
            !bank.empty() ? bank : std::filesystem::path(CEPHYR_BUILTIN_TARGET_BANK);
        if (selected_bank.empty()) {
            report(diagnostics, "cephyr.target.path",
                   "no target bank or explicit target script was provided", {});
            return std::nullopt;
        }
        selected_script = selected_bank / (triple + ".lua");
    }

    if (!(triple.empty() && !script.empty()) && !valid_triple(triple)) {
        report(diagnostics, "cephyr.target.triple",
               "invalid target triple '" + triple + "'", selected_script);
        return std::nullopt;
    }
    std::error_code path_error;
    const auto absolute_script = std::filesystem::absolute(selected_script, path_error);
    if (!path_error)
        selected_script = absolute_script.lexically_normal();

    std::string reason;
    auto source = read_script(selected_script, reason);
    if (!source) {
        if (script.empty()) {
            std::error_code ec;
            if (!std::filesystem::exists(selected_script, ec) && !ec)
                reason = "unsupported target '" + triple + "': no matching Lua target in the bank";
        }
        report(diagnostics, "cephyr.target.io", reason, selected_script);
        return std::nullopt;
    }

    LuaLimits limits;
    lua_State *state =
        lua_newstate(&limited_reallocate, &limits, 0x43455048u);
    if (!state) {
        report(diagnostics, "cephyr.target.lua",
               "cannot allocate the restricted Lua target environment",
               selected_script);
        return std::nullopt;
    }
    *static_cast<LuaLimits **>(lua_getextraspace(state)) = &limits;

    std::optional<Target> result;
    {
        kaguya::State lua(state);
        lua_sethook(state, &instruction_hook, LUA_MASKCOUNT, hook_interval);

        const std::string chunk_name = "@" + selected_script.string();
        int status = luaL_loadbufferx(state, source->data(), source->size(),
                                      chunk_name.c_str(), "t");
        if (status == LUA_OK) {
            lua_createtable(state, 0, 2);
            const std::string script_directory =
                selected_script.parent_path().string();
            lua_pushlstring(state, script_directory.data(),
                            script_directory.size());
            lua_setfield(state, -2, "script_dir");
            const std::string archon = archon_root.string();
            lua_pushlstring(state, archon.data(), archon.size());
            lua_setfield(state, -2, "archon_root");
            lua_setupvalue(state, -2, 1);
            status = lua_pcall(state, 0, 1, 0);
        }
        lua_sethook(state, nullptr, 0, 0);

        if (status != LUA_OK) {
            const char *message = lua_tostring(state, -1);
            reason = message ? message : "unknown Lua target-script error";
            report(diagnostics, "cephyr.target.lua", reason, selected_script,
                   lua_line(reason));
        } else {
            try {
                kaguya::LuaRef object(state, kaguya::StackTop());
                result = parse_target(object, triple, selected_script, archon_root,
                                      reason);
                if (!result)
                    report(diagnostics, "cephyr.target.schema", reason,
                           selected_script);
            } catch (const std::exception &error) {
                report(diagnostics, "cephyr.target.schema",
                       std::string("invalid target table: ") + error.what(),
                       selected_script);
            }
        }
    }
    lua_close(state);
    return result;
}

} // namespace cephyr
