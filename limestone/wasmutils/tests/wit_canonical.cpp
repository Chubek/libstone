#include "wasmutils/WAT-Module.hpp"
#include "wasmutils/WIT-Canonical.hpp"
#include "wasmutils/WITParser.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <iostream>
#include <limits>

namespace {

namespace abi = limestone::wasmutils::canonical;
namespace core = limestone::wasmutils::core;
using limestone::wasmutils::parse_wit;
using limestone::wasmutils::WitDocument;
using limestone::wasmutils::WitError;
using limestone::wasmutils::WitScalar;
using abi::Value;
using CT = core::Type;

void check(bool condition, std::string_view label) {
    if (!condition)
        throw std::runtime_error("canonical test failure: " + std::string(label));
}

template<class Exception = abi::Error, class Fn>
void fails(Fn&& fn, std::string_view diagnostic) {
    try {
        fn();
    } catch (const Exception& error) {
        check(std::string_view(error.what()).find(diagnostic) != std::string_view::npos, error.what());
        return;
    }
    check(false, "expected error: " + std::string(diagnostic));
}

Value u(std::uint64_t n) { return {n}; }
Value s(std::int64_t n) { return {n}; }
Value seq(std::initializer_list<Value> values) { return {Value::Sequence(values)}; }
Value arm(std::uint32_t tag, std::initializer_list<Value> payload = {}) {
    return {Value::Case{tag, payload}};
}

struct GuestMemory {
    std::vector<std::uint8_t> bytes = std::vector<std::uint8_t>(128, 0xCC);
    std::uint32_t next = 128;
    unsigned allocations = 0;
    abi::Memory memory{
        [this] { return std::span(bytes); },
        [this](std::uint32_t old, std::uint32_t old_size, std::uint32_t alignment, std::uint32_t size) {
            check(old == 0 && old_size == 0, "fresh realloc arguments");
            next = (next + alignment - 1) & ~(alignment - 1);
            const auto begin = next;
            next += size;
            bytes.resize(next, 0xCC);
            ++allocations;
            return begin;
        }
    };
};

const abi::Function& find(const std::vector<abi::Function>& funcs, std::string_view name) {
    const auto found = std::find_if(funcs.begin(), funcs.end(),
        [name](const auto& fn) { return fn.name == name; });
    check(found != funcs.end(), name);
    return *found;
}

void scalars() {
    const auto functions = abi::resolve(parse_wit(R"(
        interface samples {
            check: func(a: bool, b: s8, c: u8, d: s16, e: u16, f: s32, g: u32,
                        h: s64, i: u64, j: f32, k: f64, l: char) -> s8;
        })"));
    const auto& fn = functions.front();
    const auto sig = abi::signature(fn, abi::Direction::Lift);
    check(sig.parameters == std::vector{CT::i32, CT::i32, CT::i32, CT::i32, CT::i32, CT::i32,
          CT::i32, CT::i64, CT::i64, CT::f32, CT::f64, CT::i32}, "every scalar flat type");
    check(sig.results == std::vector{CT::i32}, "s8 result slot");

    const std::vector<core::Value> raw{{CT::i32, 7}, {CT::i32, 0xABCD0080}, {CT::i32, 0xFFFFFFFE},
        {CT::i32, 0x12348000}, {CT::i32, 0xABCDFFFF}, {CT::i32, 0x80000000}, {CT::i32, 0xFFFFFFFF},
        {CT::i64, 0x8000000000000000ULL}, {CT::i64, 0xFFFFFFFFFFFFFFFFULL},
        {CT::f32, 0x80000000}, {CT::f64, 0xFFF0000000000042ULL}, {CT::i32, 0x1F40C}};
    const auto check_scalars = [&](std::span<const Value> values) -> std::optional<Value> {
        check(values.size() == 12 && values[0] == Value{true}, "nonzero bool lifts true");
        check(values[1] == s(-128) && values[2] == u(254) && values[3] == s(-32768) &&
              values[4] == u(65535) && values[5] == s(-2147483647LL - 1) && values[6] == u(0xFFFFFFFF),
              "integer low bits and sign");
        check(values[7] == s(std::numeric_limits<std::int64_t>::min()) &&
              values[8] == u(std::numeric_limits<std::uint64_t>::max()), "i64 boundaries");
        check(std::bit_cast<std::uint32_t>(std::get<float>(values[9].data)) == 0x80000000,
              "negative float zero");
        check(std::bit_cast<std::uint64_t>(std::get<double>(values[10].data)) == 0x7FF8000000000000ULL,
              "canonical double NaN");
        check(values[11] == Value{U'\U0001F40C'}, "Unicode scalar");
        return s(-128);
    };
    const auto lowered = abi::call_lowered(fn, raw, {}, check_scalars);
    check(lowered == std::vector<core::Value>{{CT::i32, 0xFFFFFF80}}, "sign-extended s8 result");

    auto broken = raw;
    broken[2].bits = std::uint64_t{1} << 32;
    fails([&] { abi::call_lowered(fn, broken, {}, check_scalars); }, "core value at");
    broken = raw;
    broken[11].bits = 0xD800;
    fails([&] { abi::call_lowered(fn, broken, {}, check_scalars); }, "Unicode scalar");
    fails([&] { abi::call_lowered(fn, raw, {}, [](auto) { return s(128); }); }, "out of range");
    fails([&] { abi::call_lowered(fn, raw, {}, [](auto) { return u(1); }); }, "wrong component value type");
    fails([&] { abi::call_lowered(fn, raw, {}, [](auto) { return std::nullopt; }); }, "result arity");

    GuestMemory guest;
    abi::store(abi::Type::scalar(WitScalar::S16), s(-100), guest.memory, 8);
    check(guest.bytes[8] == 0x9C && guest.bytes[9] == 0xFF, "little-endian s16");
    check(abi::load(abi::Type::scalar(WitScalar::S16), guest.memory, 8) == s(-100), "memory sign");
    check(abi::load(abi::Type::scalar(WitScalar::Bool), guest.memory, 10) == Value{true},
          "nonzero memory bool");
    const auto f32 = abi::Type::scalar(WitScalar::F32);
    abi::store(f32, {std::bit_cast<float>(0xFF800123u)}, guest.memory, 12);
    check(std::bit_cast<std::uint32_t>(std::get<float>(abi::load(f32, guest.memory, 12).data)) ==
          0x7FC00000, "canonical single NaN");
    fails([&] { abi::store(abi::Type::scalar(WitScalar::Char), {char32_t{0x110000}}, guest.memory, 0); },
          "Unicode scalar");
}

void layouts_and_choices() {
    const auto functions = abi::resolve(parse_wit(R"(
        interface numbers {
            flags small { a, b, c }
            flags medium { a, b, c, d, e, f, g, h, i }
            flags large { a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p, q }
            enum color { red, green, blue }
            record row { flag: bool, timestamp: u64, shade: color, features: medium }
            variant mixed { integer(s8), single(f32), double(f64), pair(tuple<u32, f32>), empty }
            variant narrow { integer(u32), single(f32) }
            inspect: func(r: row, f: small, g: large) -> result<_, color>;
            choose: func(m: mixed, n: narrow, o: option<u64>) -> result;
        })"));
    const auto& inspect = find(functions, "wit_numbers_W_inspect");
    const auto& row = inspect.parameters.fields()[0].type;
    const auto& flags = row.fields()[3].type;
    const auto& small = inspect.parameters.fields()[1].type;
    const auto& large = inspect.parameters.fields()[2].type;
    check(row.size() == 24 && row.alignment() == 8 &&
          std::ranges::equal(row.field_offsets(), std::array{0u, 8u, 16u, 18u}), "record tail padding");
    check(flags.size() == 2 && flags.alignment() == 2, "nine memory flags");
    check(small.size() == 1 && large.size() == 4 && large.flat_count() == 1, "flag thresholds");
    check(inspect.result.size() == 2 && inspect.result.payload_offset() == 1 &&
          !inspect.result.fields()[0].type, "result<_, enum>");
    check(row.fields()[2].type.discriminant_size() == 1, "small enum discriminant");
    std::vector<abi::Field> labels(256);
    check(abi::Type::choice(abi::Kind::Enum, labels).size() == 1, "256 cases use one byte");
    labels.emplace_back();
    check(abi::Type::choice(abi::Kind::Enum, labels).size() == 2, "257 cases use two bytes");
    labels.resize(65'537);
    check(abi::Type::choice(abi::Kind::Enum, std::move(labels)).size() == 4, "65,537 cases use four bytes");

    const auto& fn = find(functions, "wit_numbers_W_choose");
    const auto& mixed = fn.parameters.fields()[0].type;
    check(mixed.alignment() == 8 && mixed.size() == 16 && mixed.payload_offset() == 8,
          "variant payload union");
    check(std::ranges::equal(mixed.flat_prefix(), std::array{CT::i32, CT::i64, CT::f32}),
          "variant joins and unequal arity");
    check(std::ranges::equal(fn.parameters.fields()[1].type.flat_prefix(), std::array{CT::i32, CT::i32}),
          "f32/i32 join");
    check(fn.result.size() == 1 && fn.result.flat_count() == 1, "untyped result discriminant");

    const std::vector<Value> values{arm(0, {s(-1)}), arm(1, {Value{-0.0f}}), arm(0)};
    const std::vector<core::Value> flat{{CT::i32, 0}, {CT::i64, 0xFFFFFFFF}, {CT::f32, 0},
        {CT::i32, 1}, {CT::i32, 0x80000000}, {CT::i32, 0}, {CT::i64, 0}};
    const auto result = abi::call_lifted(fn, values, {}, [&](auto slots) {
        check(std::ranges::equal(slots, flat), "coercion zero-extends i32 and bitcasts f32");
        return std::vector<core::Value>{{CT::i32, 1}};
    });
    check(result == arm(1), "no-payload error result");
    auto dirty = flat;
    dirty[1].bits |= 0x1234567800000000ULL;
    dirty[2].bits = 0x7FC00001;
    dirty[6].bits = 123;
    check(abi::call_lowered(fn, dirty, {}, [&](auto actual) {
        check(std::ranges::equal(actual, values), "coercion wraps i64 and ignores inactive variant slots");
        return arm(0);
    }) == std::vector<core::Value>{{CT::i32, 0}}, "no-payload ok result");
    dirty[0].bits = 5;
    fails([&] { abi::call_lowered(fn, dirty, {}, [](auto) { return arm(0); }); }, "discriminant");
    auto invalid = values;
    invalid[2] = arm(0, {u(3)});
    fails([&] { abi::call_lifted(fn, invalid, {}, [](auto) { return std::vector<core::Value>{}; }); },
          "wrong payload count");

    GuestMemory guest;
    const auto max_flags = abi::Type::flags(std::vector<abi::Field>(32));
    abi::store(max_flags, {Value::Flags{0x80000000}}, guest.memory, 0);
    check(guest.bytes[3] == 0x80 && abi::load(max_flags, guest.memory, 0) == Value{Value::Flags{0x80000000}},
          "top bit is preserved with 32 flags");
    const Value record = seq({Value{true}, u(0x0102030405060708), arm(2), {Value::Flags{0x101}}});
    abi::store(row, record, guest.memory, 8);
    check(guest.bytes[8] == 1 && guest.bytes[9] == 0xCC && guest.bytes[16] == 8 &&
          guest.bytes[23] == 1 && guest.bytes[24] == 2 && guest.bytes[25] == 0xCC &&
          guest.bytes[26] == 1 && guest.bytes[27] == 1 && guest.bytes[28] == 0xCC, "record wire bytes");
    guest.bytes[27] = 0xFF;
    check(abi::load(row, guest.memory, 8) == record, "ignore undeclared flag bits on lift");
    fails([&] { abi::store(flags, {Value::Flags{0x200}}, guest.memory, 0); }, "undeclared flags");
    fails([&] { abi::load(row, guest.memory, 4); }, "misaligned");
    guest.bytes[24] = 3;
    fails([&] { abi::load(row, guest.memory, 8); }, "discriminant");
}

void strings_and_lists() {
    const auto type = abi::Type::list(abi::Type::list(abi::Type::string()));
    const auto text = std::string("a\0\xF0\x9F\x90\x8C", 6);
    const auto value = seq({seq({Value{text}, Value{std::string{}}}), seq({})});
    const auto flat_functions = abi::resolve(
        parse_wit("interface i { pass: func(text: string, words: list<list<string>>); }"));
    const auto& flat_call = flat_functions.front();
    GuestMemory flat_guest;
    const std::array flat_args{Value{text}, value};
    const auto nothing = abi::call_lifted(flat_call, flat_args, flat_guest.memory, [&](auto raw) {
        check(std::ranges::equal(raw, std::array{core::Value{CT::i32, 128}, core::Value{CT::i32, 6},
            core::Value{CT::i32, 136}, core::Value{CT::i32, 2}}), "flat string/list descriptors");
        return abi::call_lowered(flat_call, raw, flat_guest.memory, [&](auto actual) {
            check(std::ranges::equal(actual, flat_args), "lift flat string/list");
            return std::nullopt;
        });
    });
    check(!nothing, "void call");

    GuestMemory guest;
    abi::store(type, value, guest.memory, 8);
    check(guest.allocations == 5, "nested and empty allocations");
    check(guest.bytes[8] == 128 && guest.bytes[12] == 2, "outer list stores pointer and element count");
    check(guest.bytes[128] == 144 && guest.bytes[132] == 2 && guest.bytes[144] == 160 &&
          guest.bytes[148] == 6 && guest.bytes[160] == 'a' && guest.bytes[161] == 0, "nested ABI bytes");
    check(abi::load(type, guest.memory, 8) == value, "nested list copies survive memory growth");
    guest.memory.limits.values = 3;
    fails([&] { abi::load(type, guest.memory, 8); }, "value budget");
    guest.memory.limits.values = 100;
    guest.memory.limits.string_bytes = 5;
    fails([&] { abi::load(type, guest.memory, 8); }, "value budget");
    guest.memory.limits.string_bytes = 100;

    const auto string = abi::Type::string();
    for (const auto* bytes : {"\xC0\x80", "\xED\xA0\x80", "\xF4\x90\x80\x80", "\xE2\x82"}) {
        const auto text_value = std::string(bytes);
        fails([&] { abi::store(string, {text_value}, guest.memory, 0); }, "UTF-8");
        std::copy(text_value.begin(), text_value.end(), guest.bytes.begin() + 32);
        guest.bytes[0] = 32;
        std::fill(guest.bytes.begin() + 1, guest.bytes.begin() + 8, 0);
        guest.bytes[4] = static_cast<std::uint8_t>(text_value.size());
        fails([&] { abi::load(string, guest.memory, 0); }, "UTF-8");
    }
    guest.bytes[7] = 0x10;
    fails([&] { abi::load(string, guest.memory, 0); }, "2^28");

    const auto list = abi::Type::list(abi::Type::scalar(WitScalar::U64));
    const auto funcs = abi::resolve(parse_wit("interface i { a: func(xs: list<u64>); }"));
    const auto call = [&](std::uint32_t ptr, std::uint32_t length) {
        return abi::call_lowered(funcs.front(), std::array{core::Value{CT::i32, ptr},
            core::Value{CT::i32, length}}, guest.memory, [](auto) { return std::nullopt; });
    };
    fails([&] { call(124, 0); }, "misaligned");
    fails([&] { call(0xFFFFFFF8, 1); }, "out-of-bounds");
    fails([&] { call(0, 0xFFFFFFFF); }, "2^28");
    guest.memory.realloc = [](auto, auto, auto, auto) { return 1; };
    fails([&] { abi::store(list, seq({u(1)}), guest.memory, 0); }, "misaligned");
    guest.memory.realloc = [](auto, auto, auto, auto) { return 0xFFFFFFF8; };
    fails([&] { abi::store(list, seq({u(1)}), guest.memory, 0); }, "out-of-bounds");
    guest.memory.realloc = {};
    fails([&] { abi::store(list, seq({}), guest.memory, 0); }, "realloc");
}

void indirect_calls() {
    std::string wit = "world demo { import relay: func(";
    const auto params = [&](unsigned count) {
        std::string result;
        for (unsigned i = 0; i < count; ++i) {
            if (i != 0)
                result += ", ";
            result += i == 0 ? "item-a: u8" : i == 1 ? "item-b: u64" :
                "item-" + std::string(1, static_cast<char>('a' + i)) + ": u32";
        }
        return result;
    };
    wit += params(17) + ") -> tuple<u8, string>; export other: func(" + params(16) + ") -> tuple<u8, string>; }";
    const auto functions = abi::resolve(parse_wit(wit));
    const auto& fn = find(functions, "wit_world_G_demo_I_relay");
    check(fn.world_direction == abi::Direction::Lower, "world import direction");
    const auto& other = find(functions, "wit_world_G_demo_E_other");
    check(other.world_direction == abi::Direction::Lift, "world export direction");
    const auto up = abi::signature(fn, abi::Direction::Lift);
    const auto down = abi::signature(fn, abi::Direction::Lower);
    check(up.indirect_parameters && up.indirect_result && up.parameters == std::vector{CT::i32} &&
          up.results == std::vector{CT::i32}, "canon lift returns indirect pointer");
    check(down.indirect_parameters && down.indirect_result && down.parameters == std::vector{CT::i32, CT::i32} &&
          down.results.empty(), "canon lower takes out-pointer");
    check(!abi::signature(other, abi::Direction::Lift).indirect_parameters &&
          abi::signature(other, abi::Direction::Lower).parameters.size() == 17, "16 flat args plus out-pointer");

    GuestMemory guest;
    const auto ret = seq({u(7), Value{std::string("mango")}});
    Value::Sequence arguments(17, u(42));
    bool post_return = false;
    const auto lifted = abi::call_lifted(fn, arguments, guest.memory, [&](auto raw) {
        check(raw.size() == 1 && raw[0] == core::Value{CT::i32, 128}, "indirect argument pointer");
        check(guest.bytes[128] == 42 && guest.bytes[129] == 0xCC && guest.bytes[136] == 42,
              "ABI argument tuple padding");
        const auto stored = abi::load(fn.parameters, guest.memory, 128);
        check(std::get<Value::Sequence>(stored.data) == arguments, "indirect argument values");
        abi::store(fn.result, ret, guest.memory, 0);
        return std::vector<core::Value>{{CT::i32, 0}};
    }, [&](auto raw) {
        post_return = true;
        check(raw.size() == 1 && raw[0] == core::Value{CT::i32, 0}, "post-return gets original core return");
        std::fill(guest.bytes.begin(), guest.bytes.begin() + 12, 0);
    });
    check(post_return && lifted == ret, "read result before post-return");

    const auto core_args = std::array{core::Value{CT::i32, 128}, core::Value{CT::i32, 16}};
    const auto raw = abi::call_lowered(fn, core_args, guest.memory, [&](auto values) {
        check(std::ranges::equal(values, arguments), "lowered indirect argument values");
        return ret;
    });
    check(raw.empty() && abi::load(fn.result, guest.memory, 16) == ret, "store to caller out-pointer");
    auto bad = core_args;
    bad[1].bits = 17;
    fails([&] { abi::call_lowered(fn, bad, guest.memory, [&](auto) { return ret; }); }, "misaligned");
    bad = core_args;
    bad[0].bits = 0xFFFFFFF8;
    fails([&] { abi::call_lowered(fn, bad, guest.memory, [&](auto) { return ret; }); }, "out-of-bounds");
    post_return = false;
    fails([&] {
        abi::call_lifted(fn, arguments, guest.memory, [](auto) {
            return std::vector<core::Value>{{CT::i32, 1}};
        }, [&](auto) { post_return = true; });
    }, "misaligned");
    check(!post_return, "failed lift does not call post-return");
}

void resolution_and_limits() {
    const auto dep = parse_wit(R"(
        package fixtures:fruit@1.0.0;
        interface shape { type fruit = tuple<u64, u8>; }
        world service { import shape; export count: func() -> u32; }
        interface unused { ping: func(); }
    )");
    const auto root = parse_wit(R"(
        package fixtures:demo;
        use fixtures:fruit/shape@1.0.0 as shapes;
        interface data { use shapes.{fruit as item}; label: func(item: item) -> string; }
        world client {
            include fixtures:fruit/service@1.0.0 with { count as total }
            import data;
            export service: interface { ping: func(); }
        }
    )");
    const auto functions = abi::resolve(root, std::span(&dep, 1));
    check(functions.size() == 4, "standalone and reachable functions only");
    check(find(functions, "wit_fixtures_W_demo_W_data_W_label").parameters.size() == 16,
          "aliased dependency retains tuple layout");
    check(find(functions, "wit_fixtures_W_demo_W_world_G_client_E_total").world_direction ==
          abi::Direction::Lift, "renamed include preserves direction");
    const auto report = abi::describe(root, std::span(&dep, 1));
    check(report.find("lower wit_fixtures_W_demo_W_world_G_client_I_path_P_data_W_label (i64, i32, i32) -> ()"
                      " [result out-pointer]") != std::string::npos, "canonical report");
    check(report.find("lift wit_fixtures_W_demo_W_world_G_client_I_path_P_data_W_label") ==
          std::string::npos, "no mislabeled import ABI");

    fails<WitError>([&] { abi::resolve(root); }, "missing external");
    fails<WitError>([] { abi::resolve(parse_wit("interface i { resource item; }")); }, "resource");
    const auto resource_dep = parse_wit("package fixtures:bare; interface schema { resource item; }");
    const auto resource_root = parse_wit(
        "interface i { use fixtures:bare/schema.{item}; take: func(value: own<item>); }");
    try {
        abi::resolve(resource_root, std::span(&resource_dep, 1));
        check(false, "resource dependency was accepted");
    } catch (const WitError& error) {
        check(error.dependency_index == 0 && std::string_view(error.what()).find("resource") != std::string_view::npos,
              "canonical errors identify the originating WIT dependency");
    }
    fails<WitError>([] { abi::resolve(parse_wit("interface i { flags empty {} }")); }, "1 to 32");
    fails<WitError>([] { abi::resolve(parse_wit("interface i { record empty {} }")); }, "empty records");
    std::string many_flags = "interface i { flags many {";
    for (unsigned i = 0; i < 33; ++i)
        many_flags += "flag" + std::to_string(i) + ',';
    fails<WitError>([&] { abi::resolve(parse_wit(many_flags + "} }")); }, "1 to 32");
    auto too_wide = abi::Type::scalar(WitScalar::U64);
    fails([&] {
        for (unsigned i = 0; i != 25; ++i)
            too_wide = abi::Type::sequence(abi::Kind::Tuple, {{"0", too_wide}, {"1", too_wide}});
    }, "2^28");
    check(too_wide.flat_count() > abi::max_flat_params &&
          too_wide.flat_prefix().size() == abi::max_flat_params, "large layouts do not explode flat vectors");
}

void core_integration() {
    const auto functions = abi::resolve(parse_wit(R"(
        world snacks {
            import double: func(value: u32) -> u32;
            export fetch: func(value: u32) -> tuple<u32, string>;
        })"));
    const auto& double_it = find(functions, "wit_world_G_snacks_I_double");
    const auto& fetch = find(functions, "wit_world_G_snacks_E_fetch");
    const std::array imports{core::HostFunction{"host", "double", [&](auto raw) {
        return abi::call_lowered(double_it, raw, {}, [](auto component) {
            return u(2 * std::get<std::uint64_t>(component[0].data));
        });
    }}};
    core::Instance instance(core::assemble_module(R"(
        (module
          (import "host" "double" (func $double (param i32) (result i32)))
          (memory (export "memory") 1)
          (data (i32.const 4) "\10\00\00\00\03\00\00\00")
          (data (i32.const 16) "fig")
          (func (export "fetch") (param i32) (result i32)
            i32.const 0
            local.get 0
            call $double
            i32.store
            i32.const 0))
    )"), imports);
    std::vector<std::uint8_t> snapshot;
    const abi::Memory memory{[&] { return std::span(snapshot); }, {}};
    const auto got = abi::call_lifted(fetch, std::array{u(21)}, memory, [&](auto raw) {
        auto result = instance.invoke("fetch", raw);
        snapshot = instance.read_memory("memory", 0, 32);
        return result;
    });
    check(got == seq({u(42), Value{std::string("fig")}}), "adapt calls across running core module");
}

} // namespace

int main() {
    try {
        scalars();
        layouts_and_choices();
        strings_and_lists();
        indirect_calls();
        resolution_and_limits();
        core_integration();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
