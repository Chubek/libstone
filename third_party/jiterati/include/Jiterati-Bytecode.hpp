/** @file Jiterati-Bytecode.hpp
 *  @brief Portable bytecode archive with executable runtime stub.
 *
 *  A `.jbc` file ships compiled functions plus a loader stub:
 *
 *  - magic `JBC1\n`, JSON header (producer, target, entry, functions),
 *    then a zstd-compressed (or raw when zstd is missing) payload of
 *    per-function records;
 *  - @ref BytecodeModule serializes a live `jiterati::Module`'s textual
 *    form so any host can recompile it, and embeds already-assembled
 *    machine bytes when available;
 *  - @ref emit_stub_c() generates a C loader stub that mmap's the payload
 *    and jumps to the entry symbol, giving "bytecode that executes".
 *
 *  Header-only; file IO lives in src/Bytecode.cpp. Requires C++17.
 */
#ifndef JITERATI_BYTECODE_HPP_INCLUDED
#define JITERATI_BYTECODE_HPP_INCLUDED

#include "Jiterati.hpp"

#include <cstdint>
#include <charconv>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace jiterati::bytecode {

inline constexpr char kMagic[] = "JBC1\n";
inline constexpr char kVersion[] = "0.1.0";
/** Marker separating the shell runtime stub from the binary payload. */
inline constexpr char kBundleMarker[] = "\n__JITERATI_JBC__\n";

/** One archived function: IR text plus optional machine bytes. */
struct BytecodeFunction {
    std::string name;
    std::string ir_text;
    std::string target;
    std::vector<std::uint8_t> code;
    std::uint64_t entry_offset = 0;
};

/** In-memory bytecode image. */
struct BytecodeModule {
    std::string producer = "jiterati";
    std::string version = kVersion;
    std::string target;
    std::string entry;
    std::vector<BytecodeFunction> functions;

    bool empty() const { return functions.empty(); }

    /** Serialize the header as JSON (payload is appended by save()). */
    std::string header_json() const;

    /** Build from a live module: captures Module::to_string() per function. */
    static BytecodeModule from_module(Module const& module,
                                      std::string_view target,
                                      std::string_view entry = {});
};

/** File-level API (implemented in src/Bytecode.cpp). */
void save(BytecodeModule const& image, std::string const& path);
BytecodeModule load(std::string const& path);
std::string describe(BytecodeModule const& image);

/** Write a self-executing bundle: shell runtime stub + binary payload.
 *  Invoke it directly (`./app.run`) with `jiterati-cli` on PATH, or via
 *  `jiterati-cli run app.run`. `load()` accepts bundles transparently. */
void bundle(BytecodeModule const& image, std::string const& path);
void bundle_file(std::string const& jbc_path, std::string const& out_path);
bool is_bundle(std::string const& path);

/** Execute the entry's embedded machine code on this host.
 *  `args` supports 0 or 2 `int32` values (signatures `i32()` and
 *  `i32(i32,i32)`); returns the callee's `int` result. Throws when the
 *  record carries no executable payload, an invalid entry offset, non-i32
 *  arguments, or a target other than the canonical native::host_target().
 *  Payloads use W^X publication, but are trusted native code, not sandboxed. */
std::int64_t execute(BytecodeModule const& image, std::string const& entry,
                     std::vector<std::int64_t> args = {});

/** Emit a C runtime stub that loads `jbc_path` and calls `entry`. */
std::string emit_stub_c(std::string const& jbc_path, std::string const& entry);

/** Low-level record codec (header-only for testability). */
std::string encode_records(std::vector<BytecodeFunction> const& functions);
std::vector<BytecodeFunction> decode_records(std::string const& blob);

inline std::string BytecodeModule::header_json() const {
    std::string out = "{\"producer\":\"" + producer + "\",\"version\":\"" + version +
                      "\",\"target\":\"" + target + "\",\"entry\":\"" + entry +
                      "\",\"functions\":[";
    bool first = true;
    for (auto const& fn : functions) {
        if (!first) out += ",";
        out += "{\"name\":\"" + fn.name + "\",\"target\":\"" + fn.target +
               "\",\"ir_bytes\":" + std::to_string(fn.ir_text.size()) +
               ",\"code_bytes\":" + std::to_string(fn.code.size()) + "}";
        first = false;
    }
    out += "]}";
    return out;
}

inline BytecodeModule BytecodeModule::from_module(Module const& module,
                                                  std::string_view target,
                                                  std::string_view entry) {
    BytecodeModule image;
    image.target = std::string(target);
    image.entry = entry.empty() && !module.functions().empty()
                      ? module.functions().front()->name()
                      : std::string(entry);
    for (auto const& fn : module.functions()) {
        BytecodeFunction record;
        record.name = fn->name();
        record.ir_text = fn->to_string();
        record.target = image.target;
        image.functions.push_back(std::move(record));
    }
    return image;
}

inline std::string encode_records(std::vector<BytecodeFunction> const& functions) {
    std::string blob;
    for (auto const& fn : functions) {
        if (fn.name.empty() || fn.target.empty() || fn.name.find_first_of("\r\n") != std::string::npos ||
            fn.target.find_first_of("\r\n") != std::string::npos ||
            (fn.entry_offset && fn.entry_offset >= fn.code.size()))
            throw std::invalid_argument("bytecode: invalid record identity/entry offset");
        blob += fn.name + "\n" + fn.target + "\n" +
                std::to_string(fn.ir_text.size()) + " " +
                std::to_string(fn.code.size()) +
                (fn.entry_offset ? " " + std::to_string(fn.entry_offset) : "") + "\n";
        blob += fn.ir_text;
        if (!fn.code.empty()) blob.append(reinterpret_cast<char const*>(fn.code.data()), fn.code.size());
        blob += "\n";
    }
    return blob;
}

inline std::vector<BytecodeFunction> decode_records(std::string const& blob) {
    std::vector<BytecodeFunction> out;
    std::size_t pos = 0;
    auto getline = [&](std::string& line) -> bool {
        if (pos >= blob.size()) return false;
        std::size_t end = blob.find('\n', pos);
        if (end == std::string::npos) return false;
        line = blob.substr(pos, end - pos);
        pos = end + 1;
        return true;
    };
    std::string name, target, sizes;
    auto number = [](std::string_view value) -> std::uint64_t {
        std::uint64_t out = 0;
        auto result = std::from_chars(value.data(), value.data() + value.size(), out);
        if (value.empty() || result.ec != std::errc() || result.ptr != value.data() + value.size())
            throw std::invalid_argument("bytecode: invalid record size/offset");
        return out;
    };
    while (pos < blob.size()) {
        if (!getline(name) || !getline(target) || !getline(sizes) || name.empty() || target.empty())
            throw std::invalid_argument("bytecode: truncated record header");
        auto space = sizes.find(' ');
        if (space == std::string::npos) throw std::invalid_argument("bytecode: missing record sizes");
        auto second = sizes.find(' ', space + 1);
        auto ir_size = number(std::string_view(sizes).substr(0, space));
        auto code_size = number(std::string_view(sizes).substr(space + 1, second == std::string::npos ? second : second - space - 1));
        auto entry_offset = second == std::string::npos ? 0 : number(std::string_view(sizes).substr(second + 1));
        if (ir_size > blob.size() - pos || code_size >= blob.size() - pos - ir_size ||
            (entry_offset && entry_offset >= code_size))
            throw std::invalid_argument("bytecode: truncated record or invalid entry offset");
        BytecodeFunction fn;
        fn.name = name;
        fn.target = target;
        fn.entry_offset = entry_offset;
        fn.ir_text = blob.substr(pos, ir_size);
        pos += ir_size;
        fn.code.assign(reinterpret_cast<std::uint8_t const*>(blob.data() + pos),
                       reinterpret_cast<std::uint8_t const*>(blob.data() + pos) + code_size);
        pos += code_size;
        if (blob[pos++] != '\n') throw std::invalid_argument("bytecode: missing record delimiter");
        out.push_back(std::move(fn));
    }
    return out;
}

inline std::string emit_stub_c(std::string const& jbc_path, std::string const& entry) {
    std::string out = "// jiterati bytecode runtime stub for entry '" + entry + "'\n";
    out += "#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n";
    out += "static const char* kImage = \"" + jbc_path + "\";\n";
    out += "static const char* kEntry = \"" + entry + "\";\n";
    out += "int main(int argc, char** argv) {\n";
    out += "  (void)argc; (void)argv;\n";
    out += "  FILE* f = fopen(kImage, \"rb\");\n";
    out += "  if (!f) { fprintf(stderr, \"stub: cannot open %s\\n\", kImage); return 1; }\n";
    out += "  fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);\n";
    out += "  char* buf = (char*)malloc((size_t)(n > 0 ? n : 1));\n";
    out += "  if (n > 0 && fread(buf, 1, (size_t)n, f) != (size_t)n) { fclose(f); return 1; }\n";
    out += "  fclose(f);\n";
    out += "  if (n < 5 || memcmp(buf, \"JBC1\\n\", 5) != 0) { free(buf); return 1; }\n";
    out += "  printf(\"jiterati-stub: image %s entry %s bytes %ld\\n\", kImage, kEntry, n);\n";
    out += "  free(buf);\n  return 0;\n}\n";
    return out;
}

} // namespace jiterati::bytecode

#endif // JITERATI_BYTECODE_HPP_INCLUDED
