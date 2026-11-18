#include "qwasm/qwasm.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <source_location>
#include <utility>

namespace {

using Bytes = std::vector<std::uint8_t>;
using qwasm::ErrorKind;
using qwasm::ExternalKind;
using qwasm::SectionId;

const Bytes header{0, 'a', 's', 'm', 1, 0, 0, 0};

void check(bool ok, std::string_view what,
           const std::source_location at = std::source_location::current()) {
    if (!ok) {
        std::cerr << at.file_name() << ':' << at.line() << ": " << what << '\n';
        std::exit(1);
    }
}

void add(Bytes& out, std::uint8_t id, const Bytes& data) {
    check(data.size() < 128, "test helper supports one-byte section sizes");
    out.push_back(id);
    out.push_back(static_cast<std::uint8_t>(data.size()));
    out.insert(out.end(), data.begin(), data.end());
}

Bytes one(std::uint8_t id, const Bytes& data) {
    auto out = header;
    add(out, id, data);
    return out;
}

qwasm::Module accept(std::span<const std::uint8_t> bytes,
                     const qwasm::InspectionLimits& limits = {},
                     const std::source_location at = std::source_location::current()) {
    auto result = qwasm::inspect(bytes, limits);
    if (const auto* error = std::get_if<qwasm::Error>(&result)) {
        std::cerr << qwasm::to_string(error->kind) << " at " << error->offset << ": "
                  << error->message << '\n';
        check(false, "expected a summary", at);
    }
    return std::get<qwasm::Module>(std::move(result));
}

void reject(std::span<const std::uint8_t> bytes, ErrorKind kind, std::size_t offset,
            std::string_view message, const qwasm::InspectionLimits& limits = {},
            const std::source_location at = std::source_location::current()) {
    const auto result = qwasm::inspect(bytes, limits);
    const auto* error = std::get_if<qwasm::Error>(&result);
    if (error && error->kind == kind && error->offset == offset &&
        error->message.find(message) != std::string_view::npos)
        return;
    if (error)
        std::cerr << "got " << qwasm::to_string(error->kind) << " at " << error->offset
                  << ": " << error->message << '\n';
    check(false, "wrong diagnostic", at);
}

void header_and_framing() {
    for (std::size_t i = 0; i < header.size(); ++i)
        reject(std::span{header}.first(i), ErrorKind::malformed, i, "truncated");
    auto bytes = header;
    bytes[2] = 0;
    reject(bytes, ErrorKind::malformed, 2, "magic");
    bytes = header;
    bytes[4] = 2;
    reject(bytes, ErrorKind::unsupported, 4, "version");
    bytes[4] = 0x0d;
    bytes[6] = 1; // Component binary preamble, not core v1.
    reject(bytes, ErrorKind::unsupported, 4, "version");
    const auto empty = accept(header);
    check(empty.version == 1 && empty.sections.empty() && empty.imports.empty() &&
          empty.exports.empty() && !empty.start_function && !empty.data_count, "empty module");

    reject(one(14, {}), ErrorKind::unsupported, 8, "unknown core section");
    reject(one(1, {}), ErrorKind::malformed, 10, "missing standard section payload");
    bytes = header;
    bytes.insert(bytes.end(), {1});
    reject(bytes, ErrorKind::malformed, 9, "truncated section size");
    bytes.push_back(1);
    reject(bytes, ErrorKind::malformed, 10, "section extends past module");

    // A section id is a byte; a section size is a u32 LEB with optional padding.
    bytes = header;
    bytes.insert(bytes.end(), {0, 0x81, 0x80, 0x80, 0x80, 0, 0});
    const auto padded = accept(bytes);
    check(padded.sections[0].payload.offset == 14 && padded.sections[0].payload.size == 1 &&
          padded.sections[0].custom->data.offset == 15, "padded length offsets");
    bytes[13] = 0x10;
    reject(bytes, ErrorKind::malformed, 13, "overflows");
    bytes[13] = 0x80;
    reject(bytes, ErrorKind::malformed, 13, "too long");

    bytes = one(1, {0});
    add(bytes, 0, {0});
    add(bytes, 1, {0});
    reject(bytes, ErrorKind::malformed, 14, "duplicate");

    bytes = one(6, {0});
    add(bytes, 13, {0});
    reject(bytes, ErrorKind::malformed, 11, "out of order");
    bytes = one(10, {0});
    add(bytes, 12, {0});
    reject(bytes, ErrorKind::malformed, 11, "out of order");

    bytes = one(5, {0});
    add(bytes, 13, {0});
    add(bytes, 6, {0});
    add(bytes, 12, {0});
    add(bytes, 10, {0});
    add(bytes, 11, {0});
    check(accept(bytes).sections.size() == 6, "tag and data-count section ordering");
}

void names() {
    auto bytes = one(0, {11, 0, 0xc2, 0x80, 0xe0, 0xa0, 0x80, 0xf4, 0x8f, 0xbf, 0xbf, 'z',
                         0xff, 0xff});
    add(bytes, 0, {0});
    add(bytes, 0, {0});
    const auto module = accept(bytes);
    const auto& first = module.sections[0];
    check(first.id == SectionId::custom && first.offset == 8 && first.payload.offset == 10 &&
          first.payload.size == 14 && first.custom->data.offset == 22 &&
          first.custom->data.size == 2, "custom section byte ranges");
    for (auto& byte : bytes)
        byte = 0;
    const std::string expected{"\0\xc2\x80\xe0\xa0\x80\xf4\x8f\xbf\xbfz", 11};
    check(first.custom->name == expected && module.sections[1].custom->name.empty() &&
          module.sections[2].custom->name.empty(), "owned names and repeated custom sections");

    reject(one(0, {}), ErrorKind::malformed, 10, "truncated name length");
    reject(one(0, {4, 'a'}), ErrorKind::malformed, 12, "name extends");
    reject(one(0, {2, 0xc0, 0xaf}), ErrorKind::malformed, 11, "leading byte");
    reject(one(0, {1, 0x80}), ErrorKind::malformed, 11, "leading byte");
    reject(one(0, {2, 0xe2, 0x82, 0xac}), ErrorKind::malformed, 13, "incomplete UTF-8");
    reject(one(0, {3, 0xed, 0xa0, 0x80}), ErrorKind::malformed, 12, "continuation byte");
    reject(one(0, {4, 0xf4, 0x90, 0x80, 0x80}), ErrorKind::malformed, 12, "continuation byte");
    reject(one(7, {1, 1, 0xff, 0, 0}), ErrorKind::malformed, 12, "leading byte");
    reject(one(2, {1, 1, 0xff, 0, 0, 0}), ErrorKind::malformed, 12, "leading byte");
}

void interface_metadata() {
    auto bytes = one(1, {1, 0x60, 0, 0}); // One () -> () type.
    add(bytes, 2, {5, 3, 'e', 'n', 'v', 1, 'f', 0, 0,
                      3, 'e', 'n', 'v', 1, 't', 1, 0x70, 1, 0, 3,
                      3, 'e', 'n', 'v', 1, 'm', 2, 7, 1, 4,
                      3, 'e', 'n', 'v', 1, 'g', 3, 0x7f, 1,
                      3, 'e', 'n', 'v', 1, 'e', 4, 0, 0});
    add(bytes, 3, {1, 0});
    add(bytes, 7, {5, 1, 'f', 0, 1, 1, 't', 1, 0, 1, 'm', 2, 0, 1, 'g', 3, 0, 1, 'e', 4, 0});
    add(bytes, 8, {0x81, 0});
    add(bytes, 12, {0});
    add(bytes, 10, {1, 2, 0, 0x0b}); // One empty body.
    add(bytes, 11, {0});

    const auto module = accept(bytes);
    const auto& imports = module.imports;
    check(imports.size() == 5 && module.exports.size() == 5 && module.sections.size() == 8,
          "all parsed records present");
    check(module.start_function == 1 && module.data_count == 0, "scalar metadata");
    check(imports[0].module == "env" && imports[0].name == "f" && imports[0].offset == 17 &&
          std::get<qwasm::FunctionImport>(imports[0].descriptor).type_index == 0, "function import");
    const auto& table = std::get<qwasm::TableImport>(imports[1].descriptor);
    check(table.element_type == qwasm::ReferenceType::funcref && table.limits.minimum == 0 &&
          table.limits.maximum == 3 && !table.limits.address64, "table import");
    const auto& memory = std::get<qwasm::MemoryImport>(imports[2].descriptor);
    check(memory.limits.minimum == 1 && memory.limits.maximum == 4 && memory.shared &&
          memory.limits.address64, "memory import");
    const auto& global = std::get<qwasm::GlobalImport>(imports[3].descriptor);
    check(global.value_type == qwasm::ValueType::i32 && global.is_mutable, "global import");
    check(std::get<qwasm::TagImport>(imports[4].descriptor).type_index == 0, "tag import");
    const auto& exports = module.exports;
    check(exports[0].name == "f" && exports[0].offset == module.sections[3].payload.offset + 1,
          "first export name and location");
    check(exports[0].kind == ExternalKind::function && exports[0].index == 1 &&
          exports[1].kind == ExternalKind::table && exports[1].index == 0 &&
          exports[2].kind == ExternalKind::memory && exports[3].kind == ExternalKind::global &&
          exports[4].kind == ExternalKind::tag, "export kinds and indices");

    for (const auto& section : module.sections) {
        // Truncation strictly inside each section, including its ID/size prefix.
        const auto end = section.payload.offset + section.payload.size;
        for (auto cut = section.offset + 1; cut < end; ++cut)
            reject(std::span{bytes}.first(cut), ErrorKind::malformed, cut, "");
    }
}

void diagnostics_and_limits() {
    reject(one(2, {0xff, 0xff, 0xff, 0xff, 0x0f}), ErrorKind::malformed, 10, "count cannot fit");
    reject(one(7, {1, 0x80}), ErrorKind::malformed, 10, "count cannot fit");
    reject(one(7, {1, 0, 5, 0}), ErrorKind::malformed, 12, "external kind");
    reject(one(2, {1, 0, 0, 5, 0}), ErrorKind::malformed, 13, "external kind");
    reject(one(2, {1, 0, 0, 1, 0x63, 0, 0, 0}), ErrorKind::unsupported, 14, "reference type");
    reject(one(2, {1, 0, 0, 1, 0x7f, 0, 0}), ErrorKind::malformed, 14, "table reference type");
    reject(one(2, {1, 0, 0, 1, 0x70, 3, 0, 0}), ErrorKind::unsupported, 15, "shared table");
    reject(one(2, {1, 0, 0, 2, 0x10}), ErrorKind::malformed, 14, "flags");
    reject(one(2, {1, 0, 0, 2, 8, 0, 0}), ErrorKind::unsupported, 14, "page sizes");
    reject(one(2, {1, 0, 0, 2, 2, 0}), ErrorKind::malformed, 14, "shared memory");
    reject(one(2, {1, 0, 0, 2, 1, 0}), ErrorKind::malformed, 16, "truncated maximum");
    reject(one(2, {1, 0, 0, 3, 0x7f, 2}), ErrorKind::malformed, 15, "mutability");
    reject(one(2, {1, 0, 0, 4, 1, 0}), ErrorKind::malformed, 14, "tag attribute");
    reject(one(2, {1, 0, 0, 0, 0x80}), ErrorKind::malformed, 15, "truncated function type index");
    reject(one(2, {0, 0}), ErrorKind::malformed, 11, "trailing");
    reject(one(7, {0, 0}), ErrorKind::malformed, 11, "trailing");
    reject(one(8, {}), ErrorKind::malformed, 10, "truncated start function");
    reject(one(12, {0, 0}), ErrorKind::malformed, 11, "trailing");

    const auto bytes = one(7, {1, 0, 0, 0xff, 0xff, 0xff, 0xff, 0x0f});
    auto with_more = bytes;
    with_more[17] = 0x80;
    reject(with_more, ErrorKind::malformed, 17, "too long");
    with_more[17] = 0x10;
    reject(with_more, ErrorKind::malformed, 17, "overflows");
    check(accept(bytes).exports[0].index == std::numeric_limits<std::uint32_t>::max(),
          "max u32 and unresolved export index");
    // Even with a following section, a truncated field must not read into it.
    with_more = one(7, {1, 0, 0, 0x80});
    add(with_more, 0, {0});
    reject(with_more, ErrorKind::malformed, 14, "truncated export index");

    Bytes u64{1, 0, 0, 2, 4, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 1};
    const auto wide = accept(one(2, u64));
    const auto& mem = std::get<qwasm::MemoryImport>(wide.imports[0].descriptor);
    check(mem.limits.minimum == std::numeric_limits<std::uint64_t>::max() &&
          !mem.limits.maximum && !mem.shared, "max u64 LEB and no maximum");
    u64.back() = 2;
    reject(one(2, u64), ErrorKind::malformed, 24, "overflows");
    u64.back() = 0x81;
    reject(one(2, u64), ErrorKind::malformed, 24, "too long");

    const qwasm::InspectionLimits small{.max_module_bytes = bytes.size(), .max_sections = 1,
                                       .max_imports = 0, .max_exports = 1, .max_name_bytes = 0};
    check(accept(bytes, small).exports.size() == 1, "inclusive limits");
    with_more = bytes;
    add(with_more, 0, {0});
    reject(with_more, ErrorKind::limit_exceeded, bytes.size(), "module exceeds", small);
    auto limits = small;
    limits.max_module_bytes = with_more.size();
    reject(with_more, ErrorKind::limit_exceeded, bytes.size(), "too many sections", limits);
    reject(one(2, {1, 0, 0, 0, 0}), ErrorKind::limit_exceeded, 10, "too many imports", limits);
    limits.max_exports = 0;
    reject(bytes, ErrorKind::limit_exceeded, 10, "too many exports", limits);
    reject(one(0, {1, 'a'}), ErrorKind::limit_exceeded, 10, "name exceeds", limits);

    // A successful inspection is not a verdict on opaque payloads.
    check(accept(one(10, {0xff})).sections[0].id == SectionId::code, "opaque code contents");
}

void write_fixture(const std::filesystem::path& path, const Bytes& bytes) {
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    file.close();
    check(static_cast<bool>(file), "write CLI fixture");
}

void cli_fixtures(const std::filesystem::path& dir) {
    auto bytes = one(0, {6, 'x', 0, 0x1b, '\n', '\\', '"', 0xff});
    add(bytes, 1, {1, 0x60, 0, 0});
    add(bytes, 2, {1, 1, 'm', 1, 'f', 0, 0});
    add(bytes, 7, {1, 1, 'f', 0, 0});
    write_fixture(dir / "good input.wasm", bytes);
    write_fixture(dir / "bad.wasm", one(7, {1, 0, 0, 0x80}));
    bytes = header;
    bytes[4] = 2;
    write_fixture(dir / "unsupported.wasm", bytes);
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 3 && std::string_view(argv[1]) == "--write-cli-fixtures") {
        cli_fixtures(argv[2]);
        return 0;
    }
    header_and_framing();
    names();
    interface_metadata();
    diagnostics_and_limits();
}
