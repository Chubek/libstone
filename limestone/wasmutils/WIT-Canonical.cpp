#include "WIT-Canonical.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <limits>
#include <sstream>
#include <utility>

namespace limestone::wasmutils::canonical {
namespace {

using CT = core::Type;
constexpr std::uint32_t max_byte_length = (1u << 28) - 1;
constexpr std::uint64_t memory32_size = std::uint64_t{1} << 32;

std::uint64_t align_to(std::uint64_t size, std::uint32_t alignment) {
    return (size + alignment - 1) & ~(std::uint64_t{alignment} - 1);
}

CT join(CT a, CT b) {
    if (a == b)
        return a;
    if ((a == CT::i32 && b == CT::f32) || (a == CT::f32 && b == CT::i32))
        return CT::i32;
    return CT::i64;
}

unsigned scalar_size(WitScalar value) {
    switch (value) {
    case WitScalar::Bool: case WitScalar::U8: case WitScalar::S8: return 1;
    case WitScalar::U16: case WitScalar::S16: return 2;
    case WitScalar::U32: case WitScalar::S32: case WitScalar::Char: case WitScalar::F32: return 4;
    case WitScalar::U64: case WitScalar::S64: case WitScalar::F64: return 8;
    }
    throw Error("invalid Canonical ABI scalar");
}

std::uint32_t checked_size(std::uint64_t size) {
    if (size > max_byte_length)
        throw Error("Canonical ABI value layout exceeds 2^28 - 1 bytes");
    return static_cast<std::uint32_t>(size);
}

} // namespace

struct Type::Data {
    Kind kind = Kind::Scalar;
    WitScalar scalar = WitScalar::Bool;
    std::uint32_t size = 0;
    std::uint32_t alignment = 1;
    unsigned depth = 1;
    std::vector<Field> fields;
    std::vector<std::uint32_t> offsets;
    std::uint32_t discriminant = 0;
    std::uint32_t payload = 0;
    std::size_t flat_count = 0;
    std::vector<CT> flat;

    void add_child(const Type& child) {
        depth = std::max(depth, child.get().depth + 1);
        if (depth > 512)
            throw Error("Canonical ABI type nesting exceeds implementation limit");
    }
};

Type::Type(std::shared_ptr<const Data> data) : data_(std::move(data)) {}

const Type::Data& Type::get() const {
    if (!data_)
        throw Error("absent Canonical ABI value type");
    return *data_;
}

Kind Type::kind() const { return get().kind; }
WitScalar Type::scalar() const {
    if (kind() != Kind::Scalar)
        throw Error("expected a Canonical ABI scalar type");
    return get().scalar;
}
std::uint32_t Type::size() const { return get().size; }
std::uint32_t Type::alignment() const { return get().alignment; }
std::span<const Field> Type::fields() const { return get().fields; }
std::span<const std::uint32_t> Type::field_offsets() const { return get().offsets; }
std::uint32_t Type::discriminant_size() const { return get().discriminant; }
std::uint32_t Type::payload_offset() const { return get().payload; }
std::size_t Type::flat_count() const { return get().flat_count; }
std::span<const CT> Type::flat_prefix() const { return get().flat; }

Type Type::scalar(WitScalar value) {
    auto data = std::make_shared<Data>();
    data->scalar = value;
    data->size = data->alignment = scalar_size(value);
    data->flat_count = 1;
    data->flat = {value == WitScalar::F32 ? CT::f32 : value == WitScalar::F64 ? CT::f64 :
                  data->size == 8 ? CT::i64 : CT::i32};
    return Type(std::move(data));
}

Type Type::string() {
    auto data = std::make_shared<Data>();
    data->kind = Kind::String;
    data->size = 8;
    data->alignment = 4;
    data->flat_count = 2;
    data->flat = {CT::i32, CT::i32};
    return Type(std::move(data));
}

Type Type::list(Type element) {
    auto data = std::make_shared<Data>();
    data->kind = Kind::List;
    data->size = 8;
    data->alignment = 4;
    data->flat_count = 2;
    data->flat = {CT::i32, CT::i32};
    data->add_child(element);
    data->fields.push_back({{}, std::move(element)});
    return Type(std::move(data));
}

Type Type::sequence(Kind kind, std::vector<Field> fields) {
    if (kind != Kind::Tuple && kind != Kind::Record)
        throw Error("expected a Canonical ABI tuple or record type");
    if (fields.empty())
        throw Error("empty records and tuples are not Canonical ABI value types");
    auto data = std::make_shared<Data>();
    data->kind = kind;
    data->fields = std::move(fields);
    for (const auto& field : data->fields) {
        const auto& child = field.type;
        data->add_child(child);
        data->alignment = std::max(data->alignment, child.alignment());
        const auto offset = align_to(data->size, child.alignment());
        data->size = checked_size(offset + child.size());
        data->offsets.push_back(static_cast<std::uint32_t>(offset));
        const auto prefix = child.flat_prefix().first(
            std::min(child.flat_prefix().size(), max_flat_params - data->flat.size()));
        data->flat.insert(data->flat.end(), prefix.begin(), prefix.end());
        data->flat_count += child.flat_count();
    }
    data->size = checked_size(align_to(data->size, data->alignment));
    return Type(std::move(data));
}

Type Type::choice(Kind kind, std::vector<Field> cases) {
    if (kind != Kind::Variant && kind != Kind::Enum && kind != Kind::Option && kind != Kind::Result)
        throw Error("expected a Canonical ABI variant, enum, option or result type");
    if (cases.empty() || cases.size() > std::numeric_limits<std::uint32_t>::max())
        throw Error("invalid Canonical ABI case count");
    if ((kind == Kind::Option && (cases.size() != 2 || cases[0].type || !cases[1].type)) ||
        (kind == Kind::Result && cases.size() != 2))
        throw Error("invalid Canonical ABI option/result cases");
    auto data = std::make_shared<Data>();
    data->kind = kind;
    data->discriminant = cases.size() <= 256 ? 1 : cases.size() <= 65'536 ? 2 : 4;
    data->fields = std::move(cases);
    data->flat_count = 1;
    data->flat = {CT::i32};
    std::uint32_t payload_alignment = 1;
    std::uint32_t payload_size = 0;
    for (const auto& arm : data->fields) {
        const auto& child = arm.type;
        if (!child)
            continue;
        if (kind == Kind::Enum)
            throw Error("Canonical ABI enum case cannot have a payload");
        data->add_child(child);
        payload_alignment = std::max(payload_alignment, child.alignment());
        payload_size = std::max(payload_size, child.size());
        data->flat_count = std::max(data->flat_count, 1 + child.flat_count());
        const auto prefix = child.flat_prefix().first(std::min(child.flat_prefix().size(), max_flat_params - 1));
        for (std::size_t i = 0; i < prefix.size(); ++i) {
            if (i + 1 < data->flat.size())
                data->flat[i + 1] = join(data->flat[i + 1], prefix[i]);
            else
                data->flat.push_back(prefix[i]);
        }
    }
    data->payload = static_cast<std::uint32_t>(align_to(data->discriminant, payload_alignment));
    data->alignment = std::max(data->discriminant, payload_alignment);
    data->size = checked_size(align_to(std::uint64_t{data->payload} + payload_size, data->alignment));
    return Type(std::move(data));
}

Type Type::flags(std::vector<Field> labels) {
    if (labels.empty() || labels.size() > 32)
        throw Error("Canonical ABI flags require 1 to 32 labels");
    if (std::any_of(labels.begin(), labels.end(), [](const Field& field) { return bool(field.type); }))
        throw Error("Canonical ABI flag label cannot have a payload");
    auto data = std::make_shared<Data>();
    data->kind = Kind::Flags;
    data->size = data->alignment = labels.size() <= 8 ? 1 : labels.size() <= 16 ? 2 : 4;
    data->fields = std::move(labels);
    data->flat_count = 1;
    data->flat = {CT::i32};
    return Type(std::move(data));
}

Signature signature(const Function& function, Direction direction) {
    if (direction != Direction::Lift && direction != Direction::Lower)
        throw Error("invalid Canonical ABI adapter direction");
    if (function.parameters && function.parameters.kind() != Kind::Tuple)
        throw Error("Canonical ABI function parameters must be a tuple");
    Signature result{{}, {}, function.parameters && function.parameters.flat_count() > max_flat_params,
                             function.result && function.result.flat_count() > max_flat_results};
    if (result.indirect_parameters)
        result.parameters.push_back(CT::i32);
    else if (function.parameters)
        result.parameters.assign(function.parameters.flat_prefix().begin(), function.parameters.flat_prefix().end());
    if (result.indirect_result)
        (direction == Direction::Lift ? result.results : result.parameters).push_back(CT::i32);
    else if (function.result)
        result.results.assign(function.result.flat_prefix().begin(), function.result.flat_prefix().end());
    return result;
}

namespace {

template<class T>
const T& as(const Value& value) {
    const auto* result = std::get_if<T>(&value.data);
    if (!result)
        throw Error("wrong component value type for Canonical ABI");
    return *result;
}

// All float operations in the adapter operate on bits; there is no numeric
// conversion of a float slot when it is joined with an integer slot.
static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559);
static_assert(sizeof(double) == 8 && std::numeric_limits<double>::is_iec559);
std::uint64_t nan_bits(std::uint64_t bits, bool wide) {
    const std::uint64_t infinity = wide ? 0x7FF0000000000000ULL : 0x7F800000ULL;
    const std::uint64_t sign = wide ? (1ULL << 63) : (1ULL << 31);
    if ((bits & ~sign) > infinity)
        return wide ? 0x7FF8000000000000ULL : 0x7FC00000ULL;
    return bits;
}

bool is_scalar(std::uint64_t code) {
    return code < 0x110000 && !(code >= 0xD800 && code <= 0xDFFF);
}

std::uint64_t mask_for(unsigned width) {
    return width == 64 ? ~std::uint64_t{0} : (std::uint64_t{1} << width) - 1;
}

std::uint64_t scalar_bits(WitScalar type, const Value& value, bool flat) {
    const auto width = 8 * scalar_size(type);
    switch (type) {
    case WitScalar::Bool: return as<bool>(value) ? 1 : 0;
    case WitScalar::Char: {
        const auto code = as<char32_t>(value);
        if (!is_scalar(code))
            throw Error("invalid Unicode scalar for Canonical ABI char");
        return code;
    }
    case WitScalar::F32: return nan_bits(std::bit_cast<std::uint32_t>(as<float>(value)), false);
    case WitScalar::F64: return nan_bits(std::bit_cast<std::uint64_t>(as<double>(value)), true);
    case WitScalar::S8: case WitScalar::S16: case WitScalar::S32: case WitScalar::S64: {
        const auto number = as<std::int64_t>(value);
        if (width != 64) {
            const auto sign = std::int64_t{1} << (width - 1);
            if (number < -sign || number >= sign)
                throw Error("signed component value out of range for Canonical ABI");
        }
        return static_cast<std::uint64_t>(number) & mask_for(flat ? std::max(32u, width) : width);
    }
    default: {
        const auto number = as<std::uint64_t>(value);
        if (number > mask_for(width))
            throw Error("unsigned component value out of range for Canonical ABI");
        return number;
    }
    }
}

Value scalar_value(WitScalar type, std::uint64_t bits) {
    const auto width = 8 * scalar_size(type);
    switch (type) {
    case WitScalar::Bool: return {bits != 0};
    case WitScalar::Char:
        if (!is_scalar(bits))
            throw Error("invalid Unicode scalar for Canonical ABI char");
        return {static_cast<char32_t>(bits)};
    case WitScalar::F32: return {std::bit_cast<float>(static_cast<std::uint32_t>(nan_bits(bits, false)))};
    case WitScalar::F64: return {std::bit_cast<double>(nan_bits(bits, true))};
    case WitScalar::S8: case WitScalar::S16: case WitScalar::S32: case WitScalar::S64: {
        bits &= mask_for(width);
        const auto sign = std::uint64_t{1} << (width - 1);
        // Sign-extend without an implementation-defined unsigned-to-signed cast.
        if ((bits & sign) != 0)
            return {-1 - static_cast<std::int64_t>(~bits & mask_for(width))};
        return {static_cast<std::int64_t>(bits)};
    }
    default: return {bits & mask_for(width)};
    }
}

bool valid_utf8(std::span<const std::uint8_t> bytes) {
    for (std::size_t i = 0; i < bytes.size();) {
        const auto first = bytes[i++];
        if (first < 0x80)
            continue;
        unsigned count;
        std::uint32_t code;
        std::uint32_t minimum;
        if (first >= 0xC2 && first <= 0xDF) {
            count = 1; code = first & 0x1F; minimum = 0x80;
        } else if (first >= 0xE0 && first <= 0xEF) {
            count = 2; code = first & 0x0F; minimum = 0x800;
        } else if (first >= 0xF0 && first <= 0xF4) {
            count = 3; code = first & 7; minimum = 0x10000;
        } else {
            return false;
        }
        if (count > bytes.size() - i)
            return false;
        for (; count != 0; --count) {
            const auto next = bytes[i++];
            if ((next & 0xC0) != 0x80)
                return false;
            code = (code << 6) | (next & 0x3F);
        }
        if (code < minimum || !is_scalar(code))
            return false;
    }
    return true;
}

bool wide(CT type) { return type == CT::i64 || type == CT::f64; }

void check_core_values(std::span<const core::Value> values, std::span<const CT> types) {
    if (values.size() != types.size())
        throw Error("wrong core value count for Canonical ABI signature");
    for (std::size_t i = 0; i < types.size(); ++i)
        if (values[i].type != types[i] || values[i].high != 0 ||
            (!wide(types[i]) && (values[i].bits >> 32) != 0))
            throw Error("invalid core value at Canonical ABI slot " + std::to_string(i));
}

bool sequence(Kind kind) { return kind == Kind::Record || kind == Kind::Tuple; }

std::uint32_t flag_mask(const Type& type) {
    return static_cast<std::uint32_t>(mask_for(static_cast<unsigned>(type.fields().size())));
}

std::uint32_t flag_bits(const Type& type, const Value& value) {
    const auto bits = as<Value::Flags>(value).bits;
    if (bits & ~flag_mask(type))
        throw Error("component flags value contains undeclared flags");
    return bits;
}

const Type& arm_type(const Type& type, std::uint64_t tag) {
    if (tag >= type.fields().size())
        throw Error("Canonical ABI discriminant out of range");
    return type.fields()[static_cast<std::size_t>(tag)].type;
}

const Type& check_arm(const Type& type, const Value::Case& arm) {
    const auto& payload = arm_type(type, arm.tag);
    if (arm.payload.size() != static_cast<std::size_t>(bool(payload)))
        throw Error("wrong payload count for Canonical ABI case");
    return payload;
}

class Marshal {
public:
    explicit Marshal(const Memory& memory) : memory_(memory), limits_(memory.limits) {}

    std::span<std::uint8_t> range(std::uint64_t pointer, std::uint64_t size, std::uint32_t alignment) const {
        if (!memory_.bytes)
            throw Error("Canonical ABI memory is required");
        if (pointer % alignment != 0)
            throw Error("misaligned Canonical ABI memory pointer");
        const auto view = memory_.bytes();
        const auto end = std::min(memory32_size, static_cast<std::uint64_t>(view.size()));
        if (pointer > end || size > end - pointer)
            throw Error("out-of-bounds Canonical ABI memory range");
        return view.subspan(static_cast<std::size_t>(pointer), static_cast<std::size_t>(size));
    }

    std::uint32_t allocate(std::uint32_t alignment, std::uint32_t size) const {
        if (!memory_.bytes || !memory_.realloc)
            throw Error("Canonical ABI memory and realloc are required for allocation");
        const auto pointer = memory_.realloc(0, 0, alignment, size);
        range(pointer, size, alignment);
        return pointer;
    }

    Value load(const Type& type, std::uint64_t pointer) {
        consume(limits_.values, 1);
        range(pointer, type.size(), type.alignment());
        const auto kind = type.kind();
        if (kind == Kind::Scalar)
            return scalar_value(type.scalar(), read(pointer, type.size()));
        if (kind == Kind::Flags)
            return {Value::Flags{static_cast<std::uint32_t>(read(pointer, type.size())) & flag_mask(type)}};
        if (kind == Kind::List || kind == Kind::String)
            return load_buffer(type, read(pointer, 4), read(pointer + 4, 4));
        if (sequence(kind)) {
            if (type.fields().size() > limits_.values)
                throw Error("Canonical ABI host value budget exceeded");
            Value::Sequence values;
            for (std::size_t i = 0; i < type.fields().size(); ++i)
                values.push_back(load(type.fields()[i].type, pointer + type.field_offsets()[i]));
            return {std::move(values)};
        }
        const auto tag = static_cast<std::uint32_t>(read(pointer, type.discriminant_size()));
        const auto& payload = arm_type(type, tag);
        Value::Case arm{tag, {}};
        if (payload)
            arm.payload.push_back(load(payload, pointer + type.payload_offset()));
        return {std::move(arm)};
    }

    void store(const Type& type, const Value& value, std::uint64_t pointer) {
        consume(limits_.values, 1);
        range(pointer, type.size(), type.alignment());
        const auto kind = type.kind();
        if (kind == Kind::Scalar)
            write(pointer, type.size(), scalar_bits(type.scalar(), value, false));
        else if (kind == Kind::Flags)
            write(pointer, type.size(), flag_bits(type, value));
        else if (kind == Kind::List || kind == Kind::String) {
            const auto [begin, length] = store_buffer(type, value);
            write(pointer, 4, begin);
            write(pointer + 4, 4, length);
        } else if (sequence(kind)) {
            const auto& values = as<Value::Sequence>(value);
            check_sequence(type, values);
            for (std::size_t i = 0; i < values.size(); ++i)
                store(type.fields()[i].type, values[i], pointer + type.field_offsets()[i]);
        } else {
            const auto& arm = as<Value::Case>(value);
            const auto& payload = check_arm(type, arm);
            write(pointer, type.discriminant_size(), arm.tag);
            if (payload)
                store(payload, arm.payload.front(), pointer + type.payload_offset());
        }
    }

    Value lift_flat(const Type& type, std::span<const core::Value> slots) {
        consume(limits_.values, 1);
        const auto kind = type.kind();
        if (kind == Kind::Scalar)
            return scalar_value(type.scalar(), slots.front().bits);
        if (kind == Kind::Flags)
            return {Value::Flags{static_cast<std::uint32_t>(slots.front().bits) & flag_mask(type)}};
        if (kind == Kind::List || kind == Kind::String)
            return load_buffer(type, slots[0].bits, slots[1].bits);
        if (sequence(kind)) {
            Value::Sequence values;
            for (const auto& field : type.fields()) {
                const auto count = field.type.flat_count();
                values.push_back(lift_flat(field.type, slots.first(count)));
                slots = slots.subspan(count);
            }
            return {std::move(values)};
        }
        const auto tag = static_cast<std::uint32_t>(slots.front().bits);
        const auto& payload = arm_type(type, tag);
        Value::Case arm{tag, {}};
        if (payload) {
            std::array<core::Value, max_flat_params> unpacked;
            const auto types = payload.flat_prefix();
            for (std::size_t i = 0; i < types.size(); ++i)
                unpacked[i] = {types[i], slots[i + 1].bits & mask_for(wide(types[i]) ? 64 : 32)};
            arm.payload.push_back(lift_flat(payload, std::span(unpacked).first(types.size())));
        }
        return {std::move(arm)};
    }

    void lower_flat(const Type& type, const Value& value, std::vector<core::Value>& slots) {
        consume(limits_.values, 1);
        const auto kind = type.kind();
        if (kind == Kind::Scalar)
            slots.push_back({type.flat_prefix().front(), scalar_bits(type.scalar(), value, true)});
        else if (kind == Kind::Flags)
            slots.push_back({CT::i32, flag_bits(type, value)});
        else if (kind == Kind::List || kind == Kind::String) {
            const auto [begin, length] = store_buffer(type, value);
            slots.push_back({CT::i32, begin});
            slots.push_back({CT::i32, length});
        } else if (sequence(kind)) {
            const auto& values = as<Value::Sequence>(value);
            check_sequence(type, values);
            for (std::size_t i = 0; i < values.size(); ++i)
                lower_flat(type.fields()[i].type, values[i], slots);
        } else {
            const auto& arm = as<Value::Case>(value);
            const auto& payload = check_arm(type, arm);
            const auto start = slots.size();
            slots.push_back({CT::i32, arm.tag});
            if (payload)
                lower_flat(payload, arm.payload.front(), slots);
            const auto types = type.flat_prefix();
            slots.resize(start + types.size()); // Zero-fill inactive slots.
            // Bit patterns already encode floats, and an i32/f32 slot has
            // exactly the zero-extension required for a join into i64.
            for (std::size_t i = 1; i < types.size(); ++i)
                slots[start + i].type = types[i];
        }
    }

private:
    const Memory& memory_;
    Limits limits_;

    static void consume(std::size_t& remaining, std::uint64_t count) {
        if (count > remaining)
            throw Error("Canonical ABI host value budget exceeded");
        remaining -= static_cast<std::size_t>(count);
    }

    std::uint64_t read(std::uint64_t pointer, std::uint32_t size) const {
        const auto bytes = range(pointer, size, 1);
        std::uint64_t result = 0;
        for (std::size_t i = 0; i < bytes.size(); ++i)
            result |= std::uint64_t{bytes[i]} << (8 * i);
        return result;
    }

    void write(std::uint64_t pointer, std::uint32_t size, std::uint64_t value) const {
        auto bytes = range(pointer, size, 1);
        for (std::size_t i = 0; i < bytes.size(); ++i)
            bytes[i] = static_cast<std::uint8_t>(value >> (8 * i));
    }

    static void check_sequence(const Type& type, std::span<const Value> values) {
        if (values.size() != type.fields().size())
            throw Error("wrong field count for Canonical ABI tuple or record");
    }

    Value load_buffer(const Type& type, std::uint64_t pointer, std::uint64_t length) {
        if (type.kind() == Kind::String) {
            const auto bytes = range(pointer, checked_size(length), 1);
            consume(limits_.string_bytes, length);
            if (!valid_utf8(bytes))
                throw Error("invalid UTF-8 in Canonical ABI string");
            if (bytes.empty())
                return {std::string{}};
            return {std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size())};
        }
        const auto& element = type.fields().front().type;
        range(pointer, checked_size(length * element.size()), element.alignment());
        if (length > limits_.values)
            throw Error("Canonical ABI host value budget exceeded");
        Value::Sequence values;
        // Grow only after successfully lifting children: nested lists may
        // alias the same range, so reserving every advertised length could
        // consume far more host memory than the value budget allows.
        for (std::uint64_t i = 0; i < length; ++i)
            values.push_back(load(element, pointer + i * element.size()));
        return {std::move(values)};
    }

    std::pair<std::uint32_t, std::uint32_t> store_buffer(const Type& type, const Value& value) {
        if (type.kind() == Kind::String) {
            const auto& text = as<std::string>(value);
            const auto size = checked_size(text.size());
            consume(limits_.string_bytes, size);
            // UTF-8 units are bytes. Copy through a character view so this
            // remains independent of whether plain char is signed.
            const std::span bytes(reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
            if (!valid_utf8(bytes))
                throw Error("invalid UTF-8 in Canonical ABI string");
            const auto pointer = allocate(1, size);
            if (size != 0)
                std::copy(bytes.begin(), bytes.end(), range(pointer, size, 1).begin());
            return {pointer, size};
        }
        const auto& values = as<Value::Sequence>(value);
        const auto& element = type.fields().front().type;
        if (values.size() > max_byte_length / element.size())
            throw Error("Canonical ABI list exceeds 2^28 - 1 bytes");
        if (values.size() > limits_.values)
            throw Error("Canonical ABI host value budget exceeded");
        const auto length = static_cast<std::uint32_t>(values.size());
        const auto pointer = allocate(element.alignment(), length * element.size());
        for (std::uint64_t i = 0; i < length; ++i)
            store(element, values[static_cast<std::size_t>(i)], pointer + i * element.size());
        return {pointer, length};
    }
};

} // namespace

Value load(const Type& type, const Memory& memory, std::uint32_t pointer) {
    return Marshal(memory).load(type, pointer);
}

void store(const Type& type, const Value& value, const Memory& memory, std::uint32_t pointer) {
    Marshal(memory).store(type, value, pointer);
}

std::optional<Value> call_lifted(const Function& function, std::span<const Value> arguments,
                                 const Memory& memory, const CoreCall& callee, const PostReturn& post_return) {
    const auto sig = signature(function, Direction::Lift);
    const auto fields = function.parameters ? function.parameters.fields() : std::span<const Field>{};
    if (arguments.size() != fields.size())
        throw Error("wrong component argument count for Canonical ABI call");
    if (!callee)
        throw Error("missing core callee for Canonical ABI lift");
    Marshal marshal(memory);
    std::vector<core::Value> flat;
    flat.reserve(sig.parameters.size());
    if (sig.indirect_parameters) {
        const auto& params = function.parameters;
        const auto pointer = marshal.allocate(params.alignment(), params.size());
        for (std::size_t i = 0; i < fields.size(); ++i)
            marshal.store(fields[i].type, arguments[i], std::uint64_t{pointer} + params.field_offsets()[i]);
        flat.push_back({CT::i32, pointer});
    } else {
        for (std::size_t i = 0; i < fields.size(); ++i)
            marshal.lower_flat(fields[i].type, arguments[i], flat);
    }
    const auto raw = callee(flat);
    check_core_values(raw, sig.results);
    std::optional<Value> result;
    if (sig.indirect_result)
        result = marshal.load(function.result, raw.front().bits);
    else if (function.result)
        result = marshal.lift_flat(function.result, raw);
    if (post_return)
        post_return(raw);
    return result;
}

std::vector<core::Value> call_lowered(const Function& function, std::span<const core::Value> arguments,
                                     const Memory& memory, const ComponentCall& callee) {
    const auto sig = signature(function, Direction::Lower);
    check_core_values(arguments, sig.parameters);
    if (!callee)
        throw Error("missing component callee for Canonical ABI lower");
    Marshal marshal(memory);
    Value::Sequence unpacked;
    if (function.parameters) {
        const auto& params = function.parameters;
        auto remaining = arguments;
        if (sig.indirect_parameters)
            marshal.range(remaining.front().bits, params.size(), params.alignment());
        for (std::size_t i = 0; i < params.fields().size(); ++i) {
            const auto& type = params.fields()[i].type;
            if (sig.indirect_parameters)
                unpacked.push_back(marshal.load(type, remaining.front().bits + params.field_offsets()[i]));
            else {
                unpacked.push_back(marshal.lift_flat(type, remaining.first(type.flat_count())));
                remaining = remaining.subspan(type.flat_count());
            }
        }
    }
    const auto result = callee(unpacked);
    if (bool(result) != bool(function.result))
        throw Error("wrong component result arity for Canonical ABI call");
    std::vector<core::Value> flat_result;
    if (sig.indirect_result)
        marshal.store(function.result, *result, arguments.back().bits);
    else if (result)
        marshal.lower_flat(function.result, *result, flat_result);
    return flat_result;
}

namespace {

std::string_view spelling(CT type) {
    switch (type) {
    case CT::i32: return "i32";
    case CT::i64: return "i64";
    case CT::f32: return "f32";
    case CT::f64: return "f64";
    default: throw Error("invalid Canonical ABI core type");
    }
}

void print_slots(std::ostringstream& out, std::span<const CT> types) {
    out << '(';
    for (std::size_t i = 0; i < types.size(); ++i) {
        if (i != 0)
            out << ", ";
        out << spelling(types[i]);
    }
    out << ')';
}

void print_layout(std::ostringstream& out, std::string_view label, const Type& type) {
    if (!type)
        return;
    out << "  " << label << ": size=" << type.size() << " align=" << type.alignment()
        << " flat=" << type.flat_count();
    if (sequence(type.kind())) {
        out << " fields=[";
        for (std::size_t i = 0; i < type.fields().size(); ++i) {
            if (i != 0)
                out << ", ";
            out << type.fields()[i].name << '@' << type.field_offsets()[i];
        }
        out << ']';
    } else if (type.discriminant_size() != 0) {
        out << " tag-size=" << type.discriminant_size() << " payload-offset=" << type.payload_offset();
    }
    out << '\n';
}

} // namespace

std::string describe(const WitDocument& document, std::span<const WitDocument> dependencies) {
    std::ostringstream out;
    out << "# WIT Canonical ABI: synchronous memory32, UTF-8; signatures/layouts only\n";
    for (const auto& function : resolve(document, dependencies)) {
        for (const auto direction : {Direction::Lift, Direction::Lower}) {
            if (function.world_direction && *function.world_direction != direction)
                continue;
            const auto sig = signature(function, direction);
            out << (direction == Direction::Lift ? "lift " : "lower ") << function.name << ' ';
            print_slots(out, sig.parameters);
            out << " -> ";
            print_slots(out, sig.results);
            if (sig.indirect_parameters)
                out << " [indirect parameters]";
            if (sig.indirect_result)
                out << (direction == Direction::Lift ? " [result pointer]" : " [result out-pointer]");
            out << '\n';
        }
        print_layout(out, "parameters", function.parameters);
        print_layout(out, "result", function.result);
    }
    return out.str();
}

} // namespace limestone::wasmutils::canonical
