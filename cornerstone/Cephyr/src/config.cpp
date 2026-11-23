#include <cephyr/config.hpp>

#include <cephyr/diagnostics.hpp>

#include <toml.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <string_view>
#include <system_error>
#include <utility>

namespace cephyr {
namespace {

constexpr std::size_t max_config_size = 1024 * 1024;

unsigned coordinate(std::size_t value) {
    constexpr auto maximum = std::numeric_limits<unsigned>::max();
    return value > maximum ? maximum : static_cast<unsigned>(value);
}

struct TomlDeleter {
    void operator()(toml_table_t *table) const { toml_free(table); }
};

using TomlPtr = std::unique_ptr<toml_table_t, TomlDeleter>;

struct SourceMap {
    std::filesystem::path file;
    std::string_view source;

    Location location(std::string_view section = {},
                      std::string_view key = {}) const {
        std::string current_section;
        std::size_t line_number = 1;
        std::size_t offset = 0;

        while (offset <= source.size()) {
            const std::size_t end = source.find('\n', offset);
            const std::size_t length =
                end == std::string_view::npos ? source.size() - offset : end - offset;
            std::string_view line = source.substr(offset, length);
            std::size_t first = line.find_first_not_of(" \t\r");
            if (first != std::string_view::npos && line[first] == '[') {
                const bool array_table =
                    first + 1 < line.size() && line[first + 1] == '[';
                const std::size_t begin = first + (array_table ? 2 : 1);
                const std::size_t close =
                    line.find(array_table ? "]]" : "]", begin);
                if (close != std::string_view::npos) {
                    std::string_view name = line.substr(begin, close - begin);
                    const std::size_t left = name.find_first_not_of(" \t");
                    const std::size_t right = name.find_last_not_of(" \t");
                    const std::string normalized =
                        left == std::string_view::npos
                            ? std::string{}
                            : std::string(name.substr(left, right - left + 1));
                    const std::string nested =
                        section.empty()
                            ? std::string(key)
                            : std::string(section) + "." + std::string(key);
                    if (!key.empty() && normalized == nested)
                        return Location{file.string(), coordinate(line_number),
                                        coordinate(first + 1)};
                    current_section = normalized;
                }
            } else if (first != std::string_view::npos && line[first] != '#') {
                const std::size_t equal = line.find('=', first);
                if (equal != std::string_view::npos) {
                    std::string_view name = line.substr(first, equal - first);
                    const std::size_t right = name.find_last_not_of(" \t");
                    if (right != std::string_view::npos)
                        name = name.substr(0, right + 1);

                    bool match = current_section == section && name == key;
                    if (!match && current_section.empty() && !section.empty()) {
                        const std::string dotted =
                            std::string(section) + "." + std::string(key);
                        match = name == dotted;
                    }
                    if (match)
                        return Location{file.string(), coordinate(line_number),
                                        coordinate(first + 1)};
                }
            }

            if (end == std::string_view::npos)
                break;
            offset = end + 1;
            ++line_number;
        }
        return Location{file.string(), 1, 1};
    }

    Location line_location(std::size_t line) const {
        return Location{file.string(), coordinate(line == 0 ? 1 : line), 1};
    }
};

void error(Diagnostics &diagnostics, const SourceMap &source,
           std::string_view code, std::string message,
           std::string_view section = {}, std::string_view key = {}) {
    diagnostics.error(code, message, source.location(section, key));
}

bool read_file(const std::filesystem::path &path, std::string &contents,
               Diagnostics &diagnostics) {
    const SourceMap source{path, {}};
    std::error_code status_error;
    const auto status = std::filesystem::status(path, status_error);
    if (status_error || !std::filesystem::is_regular_file(status)) {
        diagnostics.error(
            "cephyr.config.io",
            "cannot read configuration file '" + path.string() + "': " +
                (status_error ? status_error.message() : "not a regular file"),
            source.location());
        return false;
    }

    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        diagnostics.error(
            "cephyr.config.io",
            "cannot open configuration file '" + path.string() + "': " +
                std::strerror(errno),
            source.location());
        return false;
    }

    stream.seekg(0, std::ios::end);
    const std::streampos end = stream.tellg();
    if (end < 0) {
        diagnostics.error("cephyr.config.io",
                          "cannot determine the size of configuration file '" +
                              path.string() + "'",
                          source.location());
        return false;
    }
    if (static_cast<std::uintmax_t>(end) > max_config_size) {
        diagnostics.error(
            "cephyr.config.too-large",
            "configuration file is larger than the 1 MiB safety limit",
            source.location());
        return false;
    }

    contents.resize(static_cast<std::size_t>(end));
    stream.seekg(0, std::ios::beg);
    if (!contents.empty())
        stream.read(contents.data(), static_cast<std::streamsize>(contents.size()));
    if (!stream && !contents.empty()) {
        diagnostics.error("cephyr.config.io",
                          "cannot read configuration file '" + path.string() + "'",
                          source.location());
        return false;
    }
    if (contents.find('\0') != std::string::npos) {
        diagnostics.error("cephyr.config.syntax",
                          "configuration file contains a NUL byte",
                          source.location());
        return false;
    }
    return true;
}

std::size_t parse_error_line(std::string_view message) {
    constexpr std::string_view prefix = "line ";
    const std::size_t begin = message.find(prefix);
    if (begin == std::string_view::npos)
        return 1;
    std::size_t value = 0;
    std::size_t i = begin + prefix.size();
    bool found = false;
    while (i < message.size() &&
           std::isdigit(static_cast<unsigned char>(message[i]))) {
        found = true;
        const unsigned digit = static_cast<unsigned>(message[i] - '0');
        if (value > (std::numeric_limits<std::size_t>::max() - digit) / 10)
            return 1;
        value = value * 10 + digit;
        ++i;
    }
    return found ? value : 1;
}

bool known_keys(const toml_table_t *table,
                std::initializer_list<std::string_view> allowed,
                std::string_view section, const SourceMap &source,
                Diagnostics &diagnostics) {
    bool valid = true;
    for (int index = 0;; ++index) {
        const char *raw_key = toml_key_in(table, index);
        if (!raw_key)
            break;
        const std::string_view key(raw_key);
        if (std::find(allowed.begin(), allowed.end(), key) != allowed.end())
            continue;

        std::string qualified;
        if (!section.empty())
            qualified = std::string(section) + ".";
        qualified += key;
        error(diagnostics, source, "cephyr.config.unknown-key",
              "unknown configuration key '" + qualified + "'", section, key);
        valid = false;
    }
    return valid;
}

bool has(const toml_table_t *table, std::string_view key) {
    return toml_key_exists(table, std::string(key).c_str()) != 0;
}

// tomlc99 exposes decoded keys and strings as NUL-terminated C strings.
// Reject a decoded NUL before parsing, including in quoted keys that would
// otherwise silently compare equal to a known key. Leave literal strings
// and comments alone; neither interprets Unicode escapes.
std::optional<std::size_t> nul_escape_line(std::string_view source) {
    unsigned quote_count = 0;
    char quote = '\0';
    std::size_t line = 1;
    for (std::size_t i = 0; i < source.size(); ++i) {
        const char c = source[i];
        if (c == '\n')
            ++line;
        if (!quote) {
            if (c == '#') {
                const auto end = source.find('\n', i);
                if (end == std::string_view::npos)
                    break;
                i = end - 1;
            } else if (c == '"' || c == '\'') {
                quote = c;
                quote_count =
                    i + 2 < source.size() && source[i + 1] == c &&
                            source[i + 2] == c
                        ? 3 : 1;
                i += quote_count - 1;
            }
        } else if (quote == '"' && c == '\\' && i + 1 < source.size()) {
            const auto escape = source.substr(i + 1);
            if (escape.substr(0, 5) == "u0000" ||
                escape.substr(0, 9) == "U00000000")
                return line;
            if (source[++i] == '\n')
                ++line;
        } else if (c == quote) {
            if (quote_count == 3) {
                const auto start = i;
                while (i + 1 < source.size() && source[i + 1] == quote)
                    ++i;
                if (i - start < 2)
                    continue;
            }
            quote = '\0';
        }
    }
    return std::nullopt;
}

std::optional<std::string>
string_value(const toml_table_t *table, std::string_view key,
             std::string_view section, const SourceMap &source,
             Diagnostics &diagnostics, bool &valid) {
    if (!has(table, key))
        return std::nullopt;
    toml_datum_t datum = toml_string_in(table, std::string(key).c_str());
    if (!datum.ok) {
        error(diagnostics, source, "cephyr.config.type",
              "configuration key '" +
                  (section.empty() ? std::string{} : std::string(section) + ".") +
                  std::string(key) + "' must be a string",
              section, key);
        valid = false;
        return std::nullopt;
    }
    std::string result(datum.u.s);
    std::free(datum.u.s);
    return result;
}

std::optional<bool>
bool_value(const toml_table_t *table, std::string_view key,
           std::string_view section, const SourceMap &source,
           Diagnostics &diagnostics, bool &valid) {
    if (!has(table, key))
        return std::nullopt;
    toml_datum_t datum = toml_bool_in(table, std::string(key).c_str());
    if (!datum.ok) {
        error(diagnostics, source, "cephyr.config.type",
              "configuration key '" +
                  (section.empty() ? std::string{} : std::string(section) + ".") +
                  std::string(key) + "' must be a boolean",
              section, key);
        valid = false;
        return std::nullopt;
    }
    return datum.u.b != 0;
}

std::optional<std::vector<std::string>>
string_array(const toml_table_t *table, std::string_view key,
             std::string_view section, const SourceMap &source,
             Diagnostics &diagnostics, bool &valid) {
    if (!has(table, key))
        return std::nullopt;
    toml_array_t *array = toml_array_in(table, std::string(key).c_str());
    if (!array || (toml_array_kind(array) != 'v' &&
                   toml_array_nelem(array) != 0)) {
        error(diagnostics, source, "cephyr.config.type",
              "configuration key '" + std::string(section) + "." +
                  std::string(key) + "' must be an array of strings",
              section, key);
        valid = false;
        return std::nullopt;
    }

    std::vector<std::string> result;
    const int count = toml_array_nelem(array);
    result.reserve(static_cast<std::size_t>(count));
    for (int index = 0; index < count; ++index) {
        toml_datum_t datum = toml_string_at(array, index);
        if (!datum.ok) {
            error(diagnostics, source, "cephyr.config.type",
                  "configuration key '" + std::string(section) + "." +
                      std::string(key) + "' must contain only strings",
                  section, key);
            valid = false;
            return std::nullopt;
        }
        result.emplace_back(datum.u.s);
        std::free(datum.u.s);
    }
    return result;
}

std::optional<std::vector<std::string>>
tool_value(const toml_table_t *table, std::string_view key,
           const SourceMap &source, Diagnostics &diagnostics, bool &valid) {
    if (!has(table, key))
        return std::nullopt;

    if (toml_raw_in(table, std::string(key).c_str())) {
        auto value =
            string_value(table, key, "tools", source, diagnostics, valid);
        if (!value)
            return std::nullopt;
        return std::vector<std::string>{std::move(*value)};
    }
    return string_array(table, key, "tools", source, diagnostics, valid);
}

toml_table_t *table_value(const toml_table_t *root, std::string_view key,
                          const SourceMap &source, Diagnostics &diagnostics,
                          bool &valid) {
    if (!has(root, key))
        return nullptr;
    toml_table_t *table = toml_table_in(root, std::string(key).c_str());
    if (!table) {
        error(diagnostics, source, "cephyr.config.type",
              "configuration key '" + std::string(key) + "' must be a table",
              {}, key);
        valid = false;
    }
    return table;
}

bool contains_control(std::string_view value) {
    return std::any_of(value.begin(), value.end(), [](unsigned char character) {
        return character < 0x20 || character == 0x7f;
    });
}

bool valid_identifier(std::string_view value) {
    if (value.empty() ||
        !(std::isalpha(static_cast<unsigned char>(value.front())) ||
          value.front() == '_'))
        return false;
    return std::all_of(value.begin() + 1, value.end(), [](unsigned char character) {
        return std::isalnum(character) || character == '_';
    });
}

std::filesystem::path resolve_path(const std::filesystem::path &base,
                                   std::string_view value) {
    if (value.empty())
        return {};
    std::filesystem::path path(value);
    if (path.is_relative())
        path = base / path;
    return path.lexically_normal();
}

bool validate_text(std::string_view value, std::string_view qualified,
                   std::string_view section, std::string_view key,
                   const SourceMap &source, Diagnostics &diagnostics,
                   bool allow_empty = false) {
    if ((!allow_empty && value.empty()) || contains_control(value)) {
        error(diagnostics, source, "cephyr.config.value",
              "configuration key '" + std::string(qualified) +
                  (value.empty() ? "' must not be empty"
                                 : "' must not contain control characters"),
              section, key);
        return false;
    }
    return true;
}

bool validate_argv(const std::vector<std::string> &argv,
                   std::string_view key, const SourceMap &source,
                   Diagnostics &diagnostics) {
    if (argv.empty()) {
        error(diagnostics, source, "cephyr.config.value",
              "tool 'tools." + std::string(key) +
                  "' must contain an executable",
              "tools", key);
        return false;
    }
    bool valid = true;
    for (const std::string &argument : argv) {
        if (argument.empty() || contains_control(argument)) {
            error(diagnostics, source, "cephyr.config.value",
                  "tool 'tools." + std::string(key) +
                      "' has an empty argument or an argument containing a "
                      "control character",
                  "tools", key);
            valid = false;
            break;
        }
    }
    return valid;
}

bool read_paths(Config &config, const toml_table_t *table,
                const std::filesystem::path &base, const SourceMap &source,
                Diagnostics &diagnostics) {
    bool valid = known_keys(table,
                            {"triple_dir", "triple_script", "archon_root",
                             "stdlib_root", "cache_path", "sysroot"},
                            "paths", source, diagnostics);

    const auto assign = [&](std::string_view key,
                            std::filesystem::path &destination) {
        if (auto value =
                string_value(table, key, "paths", source, diagnostics, valid)) {
            if (validate_text(*value, "paths." + std::string(key), "paths", key,
                              source, diagnostics, true))
                destination = resolve_path(base, *value);
            else
                valid = false;
        }
    };

    assign("triple_dir", config.triple_dir);
    assign("triple_script", config.triple_script);
    assign("archon_root", config.archon_root);
    assign("stdlib_root", config.stdlib_root);
    assign("cache_path", config.cache_path);
    assign("sysroot", config.sysroot);
    return valid;
}

bool read_tools(Config &config, const toml_table_t *table,
                const std::filesystem::path &base, const SourceMap &source,
                Diagnostics &diagnostics) {
    bool valid = known_keys(table,
                            {"assembler", "linker"},
                            "tools", source, diagnostics);
    const auto assign = [&](std::string_view key,
                            std::vector<std::string> &destination) {
        if (auto value = tool_value(table, key, source, diagnostics, valid)) {
            if (value->empty()) {
                error(diagnostics, source, "cephyr.config.value",
                      "configured tool 'tools." + std::string(key) +
                          "' must contain an executable",
                      "tools", key);
                valid = false;
            } else if (value->front().find('/') != std::string::npos) {
                // Only argv[0] is a tool path. Fixed arguments are opaque.
                value->front() = resolve_path(base, value->front()).string();
            }
            destination = std::move(*value);
        }
    };
    assign("assembler", config.assembler);
    assign("linker", config.linker);
    return valid;
}

bool read_flags(Config &config, const toml_table_t *table,
                const SourceMap &source, Diagnostics &diagnostics) {
    bool valid = known_keys(table, {"assemble", "link"},
                            "flags", source, diagnostics);
    const auto assign = [&](std::string_view key,
                            std::vector<std::string> &destination) {
        if (auto value =
                string_array(table, key, "flags", source, diagnostics, valid))
            destination = std::move(*value);
    };
    assign("assemble", config.assemble_flags);
    assign("link", config.link_flags);
    return valid;
}

bool read_preprocess(Config &config, const toml_table_t *table,
                     const std::filesystem::path &base,
                     const SourceMap &source, Diagnostics &diagnostics) {
    bool valid = known_keys(
        table,
        {"include_dirs", "system_include_dirs", "forced_includes", "defines",
         "undefines", "trigraphs", "keep_comments", "line_markers", "hosted"},
        "preprocess", source, diagnostics);

    const auto paths = [&](std::string_view key,
                           std::vector<std::filesystem::path> &destination) {
        if (auto values =
                string_array(table, key, "preprocess", source, diagnostics, valid)) {
            std::vector<std::filesystem::path> resolved;
            resolved.reserve(values->size());
            for (const std::string &value : *values) {
                if (!validate_text(value, "preprocess." + std::string(key),
                                   "preprocess", key, source, diagnostics)) {
                    valid = false;
                    continue;
                }
                resolved.push_back(resolve_path(base, value));
            }
            destination = std::move(resolved);
        }
    };
    paths("include_dirs", config.preprocess.include_dirs);
    paths("system_include_dirs", config.preprocess.system_include_dirs);
    paths("forced_includes", config.preprocess.forced_includes);

    std::vector<std::pair<bool, std::string>> macros = config.preprocess.macros;
    if (auto definitions = string_array(table, "defines", "preprocess", source,
                                        diagnostics, valid)) {
        macros.erase(std::remove_if(macros.begin(), macros.end(),
                                    [](const auto &macro) { return macro.first; }),
                     macros.end());
        for (std::string &definition : *definitions) {
            const std::size_t equal = definition.find('=');
            const std::string_view name(
                definition.data(),
                equal == std::string::npos ? definition.size() : equal);
            if (!validate_text(definition, "preprocess.defines", "preprocess",
                               "defines", source, diagnostics) ||
                !valid_identifier(name)) {
                if (!valid_identifier(name))
                    error(diagnostics, source, "cephyr.config.value",
                          "macro definition must begin with a valid C identifier",
                          "preprocess", "defines");
                valid = false;
                continue;
            }
            macros.emplace_back(true, std::move(definition));
        }
    }
    if (auto undefinitions = string_array(table, "undefines", "preprocess",
                                          source, diagnostics, valid)) {
        macros.erase(std::remove_if(macros.begin(), macros.end(),
                                    [](const auto &macro) { return !macro.first; }),
                     macros.end());
        for (std::string &name : *undefinitions) {
            if (!valid_identifier(name)) {
                error(diagnostics, source, "cephyr.config.value",
                      "macro undefinition must be a valid C identifier",
                      "preprocess", "undefines");
                valid = false;
                continue;
            }
            macros.emplace_back(false, std::move(name));
        }
    }
    config.preprocess.macros = std::move(macros);

    if (auto value = bool_value(table, "trigraphs", "preprocess", source,
                                diagnostics, valid))
        config.preprocess.trigraphs = *value;
    if (auto value = bool_value(table, "keep_comments", "preprocess", source,
                                diagnostics, valid))
        config.preprocess.keep_comments = *value;
    if (auto value = bool_value(table, "line_markers", "preprocess", source,
                                diagnostics, valid))
        config.preprocess.line_markers = *value;
    if (auto value = bool_value(table, "hosted", "preprocess", source,
                                diagnostics, valid))
        config.preprocess.hosted = *value;
    return valid;
}

bool read_qml(Config &config, const toml_table_t *table,
              const SourceMap &source, Diagnostics &diagnostics) {
    bool valid = known_keys(table, {"profile"}, "qml", source, diagnostics);
    if (auto name = string_value(table, "profile", "qml", source, diagnostics, valid)) {
        if (validate_text(*name, "qml.profile", "qml", "profile", source, diagnostics))
            config.qml_profile = std::move(*name);
        else
            valid = false;
    }
    return valid;
}

std::optional<unsigned> qml_fuel(const toml_table_t *table, std::string_view section,
                                 const SourceMap &source, Diagnostics &diagnostics, bool &valid) {
    if (!has(table, "fuel"))
        return std::nullopt;
    const auto fuel = toml_int_in(table, "fuel");
    if (!fuel.ok || fuel.u.i <= 0 ||
        static_cast<std::uintmax_t>(fuel.u.i) > std::numeric_limits<unsigned>::max()) {
        error(diagnostics, source, "cephyr.config.value",
              "configuration key '" + std::string(section) + ".fuel' must be a positive unsigned integer",
              section, "fuel");
        valid = false;
        return std::nullopt;
    }
    return static_cast<unsigned>(fuel.u.i);
}

bool read_qml_parameters(QmlProfile &profile, const toml_table_t *entry,
                         const std::string &section, const SourceMap &source,
                         Diagnostics &diagnostics) {
    if (!has(entry, "parameters"))
        return true;
    const auto *table = toml_table_in(entry, "parameters");
    if (!table) {
        error(diagnostics, source, "cephyr.config.type",
              "configuration key '" + section + ".parameters' must be a table", section, "parameters");
        return false;
    }
    bool valid = true;
    for (int index = 0; const char *name = toml_key_in(table, index); ++index) {
        const std::string phase = section + ".parameters." + name;
        if (!validate_text(name, phase, section, "parameters", source, diagnostics)) {
            valid = false;
            continue;
        }
        const auto *parameter = toml_table_in(table, name);
        if (!parameter) {
            error(diagnostics, source, "cephyr.config.type",
                  "configuration key '" + phase + "' must be a table", section, "parameters");
            valid = false;
            continue;
        }
        valid = known_keys(parameter, {"fuel"}, phase, source, diagnostics) && valid;
        if (auto fuel = qml_fuel(parameter, phase, source, diagnostics, valid))
            profile.phase_fuel.insert_or_assign(name, *fuel);
    }
    return valid;
}

bool read_profiles(Config &config, const toml_table_t *table,
                   const std::filesystem::path &base, const SourceMap &source,
                   Diagnostics &diagnostics) {
    bool valid = true;
    for (int index = 0; const char *name = toml_key_in(table, index); ++index) {
        const std::string section = "profiles." + std::string(name);
        if (!validate_text(name, section, "profiles", name, source, diagnostics)) {
            valid = false;
            continue;
        }
        const auto *entry = toml_table_in(table, name);
        if (!entry) {
            error(diagnostics, source, "cephyr.config.type",
                  "configuration key '" + section + "' must be a table", "profiles", name);
            valid = false;
            continue;
        }
        valid = known_keys(entry, {"passes", "files", "fuel", "parameters"}, section, source, diagnostics) && valid;
        QmlProfile profile;
        if (auto passes = string_array(entry, "passes", section, source, diagnostics, valid)) {
            for (const auto &pass : *passes)
                valid = validate_text(pass, section + ".passes", section, "passes", source, diagnostics) && valid;
            profile.passes = std::move(*passes);
        }
        if (auto files = string_array(entry, "files", section, source, diagnostics, valid)) {
            for (const auto &file : *files) {
                if (validate_text(file, section + ".files", section, "files", source, diagnostics))
                    profile.files.push_back(resolve_path(base, file));
                else
                    valid = false;
            }
        }
        profile.fuel = qml_fuel(entry, section, source, diagnostics, valid);
        valid = read_qml_parameters(profile, entry, section, source, diagnostics) && valid;
        config.qml_profiles.insert_or_assign(name, std::move(profile));
    }
    return valid;
}

bool validate_config(const Config &config, const SourceMap &source,
                     Diagnostics &diagnostics) {
    bool valid = true;
    const bool valid_target = !config.target ||
        (!config.target->empty() && *config.target != "." && *config.target != ".." &&
        std::all_of(config.target->begin(), config.target->end(),
                    [](unsigned char character) {
                        return std::isalnum(character) || character == '-' ||
                               character == '_' || character == '.' ||
                               character == '+';
                    }));
    if (!valid_target) {
        error(diagnostics, source, "cephyr.config.value",
              "target must be a nonempty target-bank name containing only "
              "letters, digits, '.', '_', '+', and '-'",
              {}, "target");
        valid = false;
    }

    static const std::set<std::string, std::less<>> representations{
        "gated",          "memory-ssa", "sea-of-nodes", "anf",
        "cps",            "typed-tac",  "untyped-tac",  "supercombinators"};
    if (!representations.count(config.representation)) {
        error(diagnostics, source, "cephyr.config.value",
              "unknown IR representation '" + config.representation + "'",
              {}, "representation");
        valid = false;
    }

    if (config.assembler_style != "driver" && config.assembler_style != "as") {
        error(diagnostics, source, "cephyr.config.value",
              "assembler_style must be either 'driver' or 'as'", {},
              "assembler_style");
        valid = false;
    }
    if (config.stdlib != "cephyr" && config.stdlib != "system" &&
        config.stdlib != "none") {
        error(diagnostics, source, "cephyr.config.value",
              "stdlib must be one of 'cephyr', 'system', or 'none'", {},
              "stdlib");
        valid = false;
    }
    if (!config.assembler.empty())
        valid = validate_argv(config.assembler, "assembler", source, diagnostics) &&
                valid;
    if (!config.linker.empty())
        valid = validate_argv(config.linker, "linker", source, diagnostics) && valid;
    const auto validate_flags = [&](const std::vector<std::string> &flags,
                                    std::string_view key) {
        for (const std::string &flag : flags) {
            if (flag.empty() || contains_control(flag)) {
                error(diagnostics, source, "cephyr.config.value",
                      "flags." + std::string(key) +
                          " must not contain empty strings or control characters",
                      "flags", key);
                valid = false;
                break;
            }
        }
    };
    validate_flags(config.assemble_flags, "assemble");
    validate_flags(config.link_flags, "link");
    return valid;
}

} // namespace

bool is_raw_linker_command(const std::vector<std::string> &argv) {
    if (argv.empty())
        return false;
    const std::string name = std::filesystem::path(argv.front()).filename().string();
    std::string_view stem(name);
    if (stem.size() > 4 && stem.substr(stem.size() - 4) == ".exe")
        stem.remove_suffix(4);
    const auto version_dash = stem.rfind('-');
    if (version_dash != std::string_view::npos && version_dash + 1 < stem.size()) {
        const auto suffix = stem.substr(version_dash + 1);
        if (std::isdigit(static_cast<unsigned char>(suffix.front())) &&
            std::all_of(suffix.begin(), suffix.end(), [](unsigned char c) {
                return std::isdigit(c) || c == '.';
            }))
            stem = stem.substr(0, version_dash);
    }
    const auto is_or_has_suffix = [&](std::string_view suffix) {
        return stem == suffix ||
               (stem.size() > suffix.size() &&
                stem.substr(stem.size() - suffix.size()) == suffix &&
                stem[stem.size() - suffix.size() - 1] == '-');
    };
    return stem == "qobjld" || is_or_has_suffix("ld") || is_or_has_suffix("ld.bfd") ||
           is_or_has_suffix("ld.gold") || is_or_has_suffix("ld.lld") ||
           is_or_has_suffix("lld") || is_or_has_suffix("mold");
}

Config default_config(const std::filesystem::path &resources,
                      const std::filesystem::path &archon_root) {
    Config config;
    config.triple_dir = (resources / "triples").lexically_normal();
    config.triple_script.clear();
    config.archon_root = archon_root.lexically_normal();
    config.stdlib_root = (resources / "stdlib").lexically_normal();

    if (const char *xdg = std::getenv("XDG_CACHE_HOME");
        xdg && *xdg && std::filesystem::path(xdg).is_absolute()) {
        config.cache_path =
            (std::filesystem::path(xdg) / "cephyr").lexically_normal();
    } else if (const char *home = std::getenv("HOME");
               home && *home && std::filesystem::path(home).is_absolute()) {
        config.cache_path =
            (std::filesystem::path(home) / ".cache" / "cephyr").lexically_normal();
    }

    return config;
}

bool load_config(Config &config, const std::filesystem::path &path,
                 Diagnostics &diagnostics) {
    std::string contents;
    if (!read_file(path, contents, diagnostics))
        return false;
    SourceMap source{path, contents};

    if (const auto line = nul_escape_line(contents)) {
        diagnostics.error("cephyr.config.value",
                          "configuration strings must not contain NUL characters",
                          source.line_location(*line));
        return false;
    }

    std::vector<char> input(contents.begin(), contents.end());
    input.push_back('\0');
    char parse_error[256] = {};
    TomlPtr root(toml_parse(input.data(), parse_error, sizeof(parse_error)));
    if (!root) {
        const std::string message =
            parse_error[0] ? parse_error : "invalid TOML configuration";
        diagnostics.error("cephyr.config.syntax", message,
                          source.line_location(parse_error_line(message)));
        return false;
    }

    bool valid = known_keys(
        root.get(),
        {"target", "representation", "assembler_style", "stdlib",
         "cache_enabled", "static_link", "paths", "tools", "flags",
         "preprocess", "qml", "profiles", "factir", "blackstone", "sandslab"},
        {}, source, diagnostics);
    Config candidate = config;

    if (auto value =
            string_value(root.get(), "target", {}, source, diagnostics, valid))
        candidate.target = std::move(*value);
    if (auto value = string_value(root.get(), "representation", {}, source,
                                  diagnostics, valid))
        candidate.representation = std::move(*value);
    if (auto value = string_value(root.get(), "assembler_style", {}, source,
                                  diagnostics, valid))
        candidate.assembler_style = std::move(*value);
    if (auto value =
            string_value(root.get(), "stdlib", {}, source, diagnostics, valid))
        candidate.stdlib = std::move(*value);
    if (auto value = bool_value(root.get(), "cache_enabled", {}, source,
                                diagnostics, valid))
        candidate.cache_enabled = *value;
    if (auto value = bool_value(root.get(), "static_link", {}, source,
                                diagnostics, valid))
        candidate.static_link = *value;

    std::error_code path_error;
    std::filesystem::path absolute = std::filesystem::absolute(path, path_error);
    const std::filesystem::path base =
        (path_error ? path : absolute).parent_path();

    if (toml_table_t *table =
            table_value(root.get(), "paths", source, diagnostics, valid))
        valid = read_paths(candidate, table, base, source, diagnostics) && valid;

    if (toml_table_t *table =
            table_value(root.get(), "tools", source, diagnostics, valid))
        valid = read_tools(candidate, table, base, source, diagnostics) && valid;

    if (toml_table_t *table =
            table_value(root.get(), "flags", source, diagnostics, valid))
        valid = read_flags(candidate, table, source, diagnostics) && valid;

    if (toml_table_t *table =
            table_value(root.get(), "preprocess", source, diagnostics, valid))
        valid =
            read_preprocess(candidate, table, base, source, diagnostics) && valid;

    if (auto *table = table_value(root.get(), "qml", source, diagnostics, valid))
        valid = read_qml(candidate, table, source, diagnostics) && valid;
    if (auto *table = table_value(root.get(), "sandslab", source, diagnostics, valid)) {
        valid = known_keys(table, {"enabled"}, "sandslab", source, diagnostics) && valid;
        if (auto value = bool_value(table, "enabled", "sandslab", source, diagnostics, valid))
            candidate.sandslab_enabled = *value;
    }
    if (auto *table = table_value(root.get(), "blackstone", source, diagnostics, valid)) {
        valid = known_keys(table, {"enabled"}, "blackstone", source, diagnostics) && valid;
        if (auto value = bool_value(table, "enabled", "blackstone", source, diagnostics, valid))
            candidate.blackstone_enabled = *value;
    }
    if (auto *table = table_value(root.get(), "factir", source, diagnostics, valid)) {
        valid = known_keys(table, {"enabled"}, "factir", source, diagnostics) && valid;
        if (auto value = bool_value(table, "enabled", "factir", source, diagnostics, valid))
            candidate.factir_enabled = *value;
    }
    if (auto *table = table_value(root.get(), "profiles", source, diagnostics, valid))
        valid = read_profiles(candidate, table, base, source, diagnostics) && valid;

    valid = validate_config(candidate, source, diagnostics) && valid;
    if (!valid)
        return false;

    config = std::move(candidate);
    return true;
}

} // namespace cephyr
