#include "qwasm/qwasm.hpp"

#include <array>
#include <limits>
#include <utility>

namespace qwasm {
namespace {

constexpr std::array sections{
    std::string_view{"custom"}, std::string_view{"type"}, std::string_view{"import"},
    std::string_view{"function"}, std::string_view{"table"}, std::string_view{"memory"},
    std::string_view{"global"}, std::string_view{"export"}, std::string_view{"start"},
    std::string_view{"element"}, std::string_view{"code"}, std::string_view{"data"},
    std::string_view{"data-count"}, std::string_view{"tag"}
};
// Indexed by ID: tag follows memory, data-count precedes code.
constexpr std::array section_rank{0, 1, 2, 3, 4, 5, 7, 8, 9, 10, 12, 13, 11, 6};
static_assert(sections.size() == section_rank.size());

[[noreturn]] void fail(ErrorKind kind, std::size_t offset, std::string_view message) {
    throw Error{kind, offset, message};
}

class Reader {
public:
    explicit Reader(std::span<const std::uint8_t> bytes, std::size_t base = 0)
        : bytes_(bytes), base_(base) {}

    std::size_t offset() const { return base_ + pos_; }
    std::size_t remaining() const { return bytes_.size() - pos_; }

    std::span<const std::uint8_t> take(std::size_t count, std::string_view truncated) {
        if (count > remaining())
            fail(ErrorKind::malformed, base_ + bytes_.size(), truncated);
        const auto view = bytes_.subspan(pos_, count);
        pos_ += count;
        return view;
    }

    std::uint8_t byte(std::string_view truncated) { return take(1, truncated).front(); }

    template <typename UInt>
    UInt leb(std::string_view truncated) {
        constexpr unsigned bits = std::numeric_limits<UInt>::digits;
        constexpr unsigned last_shift = ((bits - 1) / 7) * 7;
        UInt value = 0;
        for (unsigned shift = 0; ; shift += 7) {
            const auto at = offset();
            const auto b = byte(truncated);
            if (shift == last_shift) {
                if (b & 0x80)
                    fail(ErrorKind::malformed, at, "unsigned LEB128 is too long");
                if (b >> (bits - last_shift))
                    fail(ErrorKind::malformed, at, "unsigned LEB128 overflows its field");
            }
            value |= static_cast<UInt>(b & 0x7f) << shift;
            if (!(b & 0x80))
                return value;
        }
    }

    void require_end(std::string_view message) const {
        if (remaining() != 0)
            fail(ErrorKind::malformed, offset(), message);
    }

private:
    std::span<const std::uint8_t> bytes_;
    std::size_t base_;
    std::size_t pos_ = 0;
};

void check_utf8(std::span<const std::uint8_t> bytes, std::size_t base) {
    for (std::size_t i = 0; i < bytes.size();) {
        const auto b = bytes[i++];
        if (b <= 0x7f)
            continue;
        if (b < 0xc2 || b > 0xf4)
            fail(ErrorKind::malformed, base + i - 1, "invalid UTF-8 leading byte in name");
        const unsigned extra = b < 0xe0 ? 1 : (b < 0xf0 ? 2 : 3);
        for (unsigned j = 0; j < extra; ++j, ++i) {
            if (i == bytes.size())
                fail(ErrorKind::malformed, base + i, "incomplete UTF-8 sequence in name");
            // Exclude overlong forms, surrogate halves, and values above U+10FFFF.
            const auto lo = j == 0 && b == 0xe0 ? 0xa0 : (j == 0 && b == 0xf0 ? 0x90 : 0x80);
            const auto hi = j == 0 && b == 0xed ? 0x9f : (j == 0 && b == 0xf4 ? 0x8f : 0xbf);
            if (bytes[i] < lo || bytes[i] > hi)
                fail(ErrorKind::malformed, base + i, "invalid UTF-8 continuation byte in name");
        }
    }
}

std::string read_name(Reader& reader, std::size_t limit) {
    const auto length_at = reader.offset();
    const auto length = reader.leb<std::uint32_t>("truncated name length");
    const auto at = reader.offset();
    const auto bytes = reader.take(length, "name extends past section end");
    if (length > limit)
        fail(ErrorKind::limit_exceeded, length_at, "name exceeds byte limit");
    check_utf8(bytes, at);
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

ExternalKind read_kind(Reader& reader) {
    const auto at = reader.offset();
    const auto b = reader.byte("missing external kind");
    if (b > static_cast<std::uint8_t>(ExternalKind::tag))
        fail(ErrorKind::malformed, at, "invalid external kind");
    return static_cast<ExternalKind>(b);
}

ValueType read_type(Reader& reader, bool reference_only) {
    const auto at = reader.offset();
    const auto b = reader.byte(reference_only ? "missing table reference type" :
                                                "missing global value type");
    if (b == 0x70 || b == 0x6f || (!reference_only && b >= 0x7b && b <= 0x7f))
        return static_cast<ValueType>(b);
    if (b == 0x63 || b == 0x64 || (b >= 0x69 && b <= 0x74))
        fail(ErrorKind::unsupported, at, "import reference type is not supported");
    fail(ErrorKind::malformed, at,
         reference_only ? "invalid table reference type" : "invalid global value type");
}

struct ParsedLimits {
    TypeLimits values;
    bool shared;
};

ParsedLimits read_limits(Reader& reader, bool memory) {
    const auto at = reader.offset();
    const auto flags = reader.byte("missing limits flags");
    const bool has_max = (flags & 1) != 0;
    const bool shared = (flags & 2) != 0;
    const bool address64 = (flags & 4) != 0;
    if (flags > (memory ? 15 : 7))
        fail(ErrorKind::malformed, at, "invalid limits flags");
    if (!memory && shared)
        fail(ErrorKind::unsupported, at, "shared table imports are not supported");
    if (flags & 8)
        fail(ErrorKind::unsupported, at, "custom memory page sizes are not supported");
    if (shared && !has_max)
        fail(ErrorKind::malformed, at, "shared memory requires a maximum");
    // The current core limits grammar uses u64 for both address types. This
    // reader does not check the semantic bounds associated with either type.
    TypeLimits result{reader.leb<std::uint64_t>("truncated minimum limit"), {}, address64};
    if (has_max)
        result.maximum = reader.leb<std::uint64_t>("truncated maximum limit");
    return {result, shared};
}

ImportDescriptor read_descriptor(Reader& reader) {
    switch (read_kind(reader)) {
    case ExternalKind::function:
        return FunctionImport{reader.leb<std::uint32_t>("truncated function type index")};
    case ExternalKind::table: {
        const auto type = static_cast<ReferenceType>(read_type(reader, true));
        return TableImport{type, read_limits(reader, false).values};
    }
    case ExternalKind::memory: {
        const auto limits = read_limits(reader, true);
        return MemoryImport{limits.values, limits.shared};
    }
    case ExternalKind::global: {
        const auto type = read_type(reader, false);
        const auto at = reader.offset();
        const auto mut = reader.byte("missing global mutability");
        if (mut > 1)
            fail(ErrorKind::malformed, at, "invalid global mutability");
        return GlobalImport{type, mut != 0};
    }
    case ExternalKind::tag: {
        const auto at = reader.offset();
        if (reader.byte("missing tag attribute") != 0)
            fail(ErrorKind::malformed, at, "invalid tag attribute");
        return TagImport{reader.leb<std::uint32_t>("truncated tag type index")};
    }
    }
    // read_kind rejects unknown values.
    fail(ErrorKind::malformed, reader.offset(), "invalid import descriptor");
}

std::uint32_t read_count(Reader& reader, std::size_t minimum_size, std::size_t limit,
                         std::string_view truncated, std::string_view impossible,
                         std::string_view over_limit) {
    const auto at = reader.offset();
    const auto count = reader.leb<std::uint32_t>(truncated);
    if (count > reader.remaining() / minimum_size)
        fail(ErrorKind::malformed, at, impossible);
    if (count > limit)
        fail(ErrorKind::limit_exceeded, at, over_limit);
    return count;
}

void read_imports(Reader& reader, Module& module, const InspectionLimits& limits) {
    // Each entry needs at least two name lengths, a kind and one descriptor byte.
    const auto count = read_count(reader, 4, limits.max_imports, "truncated import count",
                                  "import count cannot fit in section", "too many imports");
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto at = reader.offset();
        auto from = read_name(reader, limits.max_name_bytes);
        auto name = read_name(reader, limits.max_name_bytes);
        auto descriptor = read_descriptor(reader);
        module.imports.push_back({at, std::move(from), std::move(name), descriptor});
    }
    reader.require_end("trailing bytes in import section");
}

void read_exports(Reader& reader, Module& module, const InspectionLimits& limits) {
    // Each entry needs at least a name length, a kind and an index.
    const auto count = read_count(reader, 3, limits.max_exports, "truncated export count",
                                  "export count cannot fit in section", "too many exports");
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto at = reader.offset();
        auto name = read_name(reader, limits.max_name_bytes);
        const auto kind = read_kind(reader);
        const auto index = reader.leb<std::uint32_t>("truncated export index");
        module.exports.push_back({at, std::move(name), kind, index});
    }
    reader.require_end("trailing bytes in export section");
}

Module inspect_module(std::span<const std::uint8_t> bytes, const InspectionLimits& limits) {
    if (bytes.size() > limits.max_module_bytes)
        fail(ErrorKind::limit_exceeded, limits.max_module_bytes, "module exceeds byte limit");

    Reader reader(bytes);
    for (const auto b : {0x00, 0x61, 0x73, 0x6d}) {
        const auto at = reader.offset();
        if (reader.byte("truncated WebAssembly magic") != b)
            fail(ErrorKind::malformed, at, "invalid WebAssembly magic");
    }
    const auto version = reader.take(4, "truncated WebAssembly version");
    if (version[0] != 1 || version[1] != 0 || version[2] != 0 || version[3] != 0)
        fail(ErrorKind::unsupported, 4, "not a core WebAssembly binary version 1");
    Module module{};
    module.version = 1;
    int previous_rank = 0;
    std::array<bool, sections.size()> seen{};
    while (reader.remaining() != 0) {
        const auto at = reader.offset();
        if (module.sections.size() == limits.max_sections)
            fail(ErrorKind::limit_exceeded, at, "too many sections");
        const auto raw_id = reader.byte("missing section ID");
        const auto size = reader.leb<std::uint32_t>("truncated section size");
        const auto payload_at = reader.offset();
        Reader payload(reader.take(size, "section extends past module end"), payload_at);
        if (raw_id >= sections.size())
            fail(ErrorKind::unsupported, at, "unknown core section ID");
        if (raw_id != 0) {
            if (seen[raw_id])
                fail(ErrorKind::malformed, at, "duplicate standard section");
            if (section_rank[raw_id] < previous_rank)
                fail(ErrorKind::malformed, at, "standard section out of order");
            seen[raw_id] = true;
            previous_rank = section_rank[raw_id];
        }

        const auto id = static_cast<SectionId>(raw_id);
        Section section{id, at, {payload_at, size}, {}};
        switch (id) {
        case SectionId::custom: {
            auto name = read_name(payload, limits.max_name_bytes);
            section.custom = CustomSection{std::move(name), {payload.offset(), payload.remaining()}};
            break;
        }
        case SectionId::import:
            read_imports(payload, module, limits);
            break;
        case SectionId::export_:
            read_exports(payload, module, limits);
            break;
        case SectionId::start:
            module.start_function = payload.leb<std::uint32_t>("truncated start function index");
            payload.require_end("trailing bytes in start section");
            break;
        case SectionId::data_count:
            module.data_count = payload.leb<std::uint32_t>("truncated data count");
            payload.require_end("trailing bytes in data-count section");
            break;
        default:
            if (size == 0)
                fail(ErrorKind::malformed, payload_at, "missing standard section payload");
            break; // Known section; intentionally opaque.
        }
        module.sections.push_back(std::move(section));
    }
    return module;
}

} // namespace

Inspection inspect(std::span<const std::uint8_t> bytes, const InspectionLimits& limits) {
    try {
        return inspect_module(bytes, limits);
    } catch (const Error& err) {
        return err;
    }
}

std::string_view to_string(SectionId id) noexcept {
    const auto i = static_cast<std::uint8_t>(id);
    return i < sections.size() ? sections[i] : "unknown";
}

std::string_view to_string(ExternalKind kind) noexcept {
    switch (kind) {
    case ExternalKind::function: return "function";
    case ExternalKind::table: return "table";
    case ExternalKind::memory: return "memory";
    case ExternalKind::global: return "global";
    case ExternalKind::tag: return "tag";
    }
    return "unknown";
}

std::string_view to_string(ReferenceType type) noexcept {
    return to_string(static_cast<ValueType>(type));
}

std::string_view to_string(ValueType type) noexcept {
    switch (type) {
    case ValueType::i32: return "i32";
    case ValueType::i64: return "i64";
    case ValueType::f32: return "f32";
    case ValueType::f64: return "f64";
    case ValueType::v128: return "v128";
    case ValueType::funcref: return "funcref";
    case ValueType::externref: return "externref";
    }
    return "unknown";
}

std::string_view to_string(ErrorKind kind) noexcept {
    switch (kind) {
    case ErrorKind::malformed: return "malformed";
    case ErrorKind::unsupported: return "unsupported";
    case ErrorKind::limit_exceeded: return "limit exceeded";
    }
    return "unknown";
}

} // namespace qwasm
