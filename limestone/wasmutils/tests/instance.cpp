#include "wasmutils/WASM-Instance.hpp"
#include "../../tests/test.hpp"

#include <array>
#include <initializer_list>
#include <limits>
#include <utility>

namespace core = limestone::wasmutils::core;
using limestone::wasmutils::Error;

namespace {

using core::Value;
using enum core::Type;
using Values = std::vector<Value>;

core::Program program(std::initializer_list<std::uint8_t> bytes) {
    return core::decode({bytes.begin(), bytes.size()});
}

void add_function(core::Module& module, std::uint32_t type,
                  std::initializer_list<std::uint8_t> bytes,
                  std::vector<core::LocalRun> locals = {}) {
    module.functions.push_back({type, {0, std::move(locals), program(bytes)}});
}

template<class Fn>
void traps(Fn test, std::string_view message, std::size_t offset) {
    try {
        test();
    } catch (const core::Trap& error) {
        CHECK(error.offset == offset);
        CHECK(std::string_view(error.what()).find(message) != std::string_view::npos);
        return;
    }
    CHECK(false && "expected execution trap");
}

template<class Fn>
void rejects(Fn test, std::string_view message) {
    try {
        test();
    } catch (const core::Trap&) {
        CHECK(false && "expected validation or linking error, not an execution trap");
    } catch (const Error& error) {
        CHECK(std::string_view(error.what()).find(message) != std::string_view::npos);
        return;
    }
    CHECK(false && "expected rejection");
}

void test_calls_and_locals() {
    core::Module module;
    module.types = {{{i32, i64}, {i64, i32}}, {{}, {i32, i64, f32, f64, funcref, externref}}};
    // Store arguments in the declared locals, then return them in reverse order.
    add_function(module, 0, {0x20,1, 0x22,2, 0x1a, 0x20,0, 0x21,3, 0x20,2, 0x20,3, 0x0b},
                 {{1,i64}, {1,i32}});
    add_function(module, 0, {0x20,0, 0x20,1, 0x10,0, 0x0b});
    add_function(module, 1, {0x20,0, 0x20,1, 0x20,2, 0x20,3, 0x20,4, 0x20,5, 0x0b},
                 {{1,i32}, {1,i64}, {1,f32}, {1,f64}, {1,funcref}, {1,externref}});
    add_function(module, 0, {0x20,0, 0x20,1, 0x10,0, 0x1a, 0x1a, 0x20,0, 0x20,1, 0x10,0, 0x0b});
    module.exports = {{"reverse", core::ExternalKind::function, 1}};

    core::Instance instance(module);
    const Values args{{i32, 77}, {i64, 0x123456789abcdef0ULL}};
    CHECK(instance.invoke("reverse", args) == Values{{i64, args[1].bits}, {i32, 77}});
    CHECK(instance.invoke(2) == Values{{i32, 0}, {i64, 0}, {f32, 0}, {f64, 0},
                                       {funcref, 0}, {externref, 0}});
    rejects([&] { instance.invoke("reverse", Values{{i64, 77}, {i64, 1}}); }, "argument type");
    rejects([&] { instance.invoke("reverse", Values{{i32, 1ULL << 32}, {i64, 1}}); }, "argument type");
    rejects([&] { instance.invoke("reverse", Values{{i32, 77}, {i64, 1, 1}}); }, "argument type");
    rejects([&] { instance.invoke("missing"); }, "unknown function export");
    rejects([&] { instance.invoke(4); }, "unknown function index");
    rejects([&] { core::Instance too_small(module, {}, {.max_locals = 3}); }, "local count");
    core::Instance too_small(module, {}, {.max_live_locals = 5});
    traps([&] { too_small.invoke("reverse", args); }, "live function local count", 4);
    CHECK(too_small.invoke(0, args) == Values{{i64, args[1].bits}, {i32, 77}});
    core::Instance reuse_locals(module, {}, {.max_live_locals = 6});
    CHECK(reuse_locals.invoke(3, args) == Values{{i64, args[1].bits}, {i32, 77}});
}

void test_structured_control() {
    core::Module module;
    module.types = {{{i32}, {i32}}, {{}, {i32}}, {{externref, externref, i32}, {externref}},
                    {{i32, i32}, {i32}}};
    // A backedge carries the accumulator and counter; the exit returns only the accumulator.
    add_function(module, 0, {0x41,0, 0x20,0, 0x03,3, 0x21,0, 0x41,1, 0x6a, 0x20,0, 0x41,1, 0x6b,
                             0x22,0, 0x20,0, 0x0d,0, 0x1a, 0x0b, 0x0b});
    // Branch carries 9 across a nested block, discarding 8 but preserving 7 below the label.
    add_function(module, 1, {0x41,7, 0x02,0x7f, 0x02,0x40, 0x41,8, 0x41,9, 0x0c,1,
                             0x0b, 0x00, 0x0b, 0x6a, 0x0b});
    // br_table 0 1 chooses the inner block only for selector 0.
    add_function(module, 0, {0x02,0x7f, 0x02,0x7f, 0x41,41, 0x20,0, 0x0e,1,0,1,
                             0x0b, 0x41,1, 0x6a, 0x0b, 0x0b});
    // Both arms receive the block parameter below the if condition.
    add_function(module, 0, {0x41,10, 0x20,0, 0x04,0, 0x41,1, 0x6a, 0x05, 0x41,2, 0x6a,
                             0x0b, 0x0b});
    // No explicit else: the false path passes its parameter through as the result.
    add_function(module, 0, {0x41,10, 0x20,0, 0x04,0, 0x41,1, 0x6a, 0x0b, 0x0b});
    // return branches to the function label from a nested block and discards 8.
    add_function(module, 1, {0x02,0x40, 0x41,8, 0x41,22, 0x0f, 0x0b, 0x00, 0x0b});
    add_function(module, 2, {0x20,0, 0x20,1, 0x20,2, 0x1c,1,0x6f, 0x0b});
    add_function(module, 0, {0x41,2, 0x41,3, 0x20,0, 0x1b, 0x0b});

    core::Instance instance(std::move(module));
    CHECK(instance.invoke(0, Values{{i32, 4}}) == Values{{i32, 4}});
    CHECK(instance.invoke(1) == Values{{i32, 16}});
    CHECK(instance.invoke(2, Values{{i32, 0}}) == Values{{i32, 42}});
    CHECK(instance.invoke(2, Values{{i32, 1}}) == Values{{i32, 41}});
    CHECK(instance.invoke(2, Values{{i32, 0xffffffff}}) == Values{{i32, 41}});
    CHECK(instance.invoke(3, Values{{i32, 0}}) == Values{{i32, 12}});
    CHECK(instance.invoke(3, Values{{i32, 0xffffffff}}) == Values{{i32, 11}});
    CHECK(instance.invoke(4, Values{{i32, 0}}) == Values{{i32, 10}});
    CHECK(instance.invoke(4, Values{{i32, 1}}) == Values{{i32, 11}});
    CHECK(instance.invoke(5) == Values{{i32, 22}});
    CHECK(instance.invoke(6, Values{{externref, 123}, {externref, 456}, {i32, 1}}) ==
          Values{{externref, 123}});
    CHECK(instance.invoke(6, Values{{externref, 123}, {externref, 456}, {i32, 0}}) ==
          Values{{externref, 456}});
    CHECK(instance.invoke(7, Values{{i32, 0}}) == Values{{i32, 3}});
    CHECK(instance.invoke(7, Values{{i32, 1}}) == Values{{i32, 2}});
}

void test_fuel_and_traps() {
    core::Module module;
    module.types = {{{}, {}}, {{}, {i32}}};
    add_function(module, 0, {0x03,0x40, 0x0c,0, 0x0b, 0x0b}); // Infinite loop.
    add_function(module, 0, {0x10,1, 0x0b});                   // Infinite recursion.
    add_function(module, 1, {0x10,3, 0x0b});
    add_function(module, 1, {0x41,5, 0x0b});
    add_function(module, 0, {0x01, 0x00, 0x0b});
    core::Instance instance(module, {}, {.max_call_depth = 8});

    traps([&] { instance.invoke(0, {}, 4); }, "fuel", 2);
    traps([&] { instance.invoke(1, {}, 4); }, "fuel", 0);
    traps([&] { instance.invoke(1); }, "call depth", 0);
    traps([&] { instance.invoke(2, {}, 3); }, "fuel", 2);
    CHECK(instance.invoke(2, {}, 4) == Values{{i32, 5}});
    traps([&] { instance.invoke(4); }, "unreachable", 1);
    CHECK(instance.invoke(3) == Values{{i32, 5}});

    // Start uses its own fuel and is run only at instantiation.
    module.start = 0;
    traps([&] { core::Instance only_three(module, {}, {.start_fuel = 3}); }, "fuel", 2);
}

void test_host_imports() {
    core::Module module;
    module.types = {{{i32}, {i32, i64}}, {{}, {}}};
    module.imports = {{"host", "tuple", std::uint32_t{0}}, {"other", "start", std::uint32_t{1}}};
    add_function(module, 0, {0x20,0, 0x10,0, 0x0b});
    module.start = 1;
    module.exports = {{"indirect-host", core::ExternalKind::function, 2},
                      {"direct-host", core::ExternalKind::function, 0}};

    unsigned started = 0;
    unsigned called = 0;
    const std::vector<core::HostFunction> bindings{
        {"other", "start", [&](std::span<const Value> args) {
            CHECK(args.empty());
            ++started;
            return Values{};
        }},
        {"host", "tuple", [&](std::span<const Value> args) {
            CHECK(args.size() == 1);
            ++called;
            return Values{args[0], {i64, called}};
        }},
    };
    rejects([&] { core::Instance no_bindings(module); }, "missing host function binding");
    const std::array duplicates{bindings[0], bindings[1], bindings[0]};
    rejects([&] { core::Instance duplicate_binding(module, duplicates); }, "duplicate host function binding");
    traps([&] { core::Instance no_fuel(module, bindings, {.start_fuel = 0}); }, "fuel", 0);
    CHECK(started == 0);

    core::Instance instance(module, bindings);
    CHECK(started == 1);
    CHECK(instance.invoke("indirect-host", Values{{i32, 5}}) == Values{{i32, 5}, {i64, 1}});
    traps([&] { instance.invoke("direct-host", Values{{i32, 5}}, 0); }, "fuel", 0);
    CHECK(instance.invoke("direct-host", Values{{i32, 6}}, 1) == Values{{i32, 6}, {i64, 2}});
    CHECK(started == 1);

    auto broken = bindings;
    broken[1].callback = [](std::span<const Value>) { return Values{{i32, 0}, {i32, 0}}; };
    core::Instance wrong_result(module, broken);
    traps([&] { wrong_result.invoke("indirect-host", Values{{i32, 0}}); }, "result type", 2);

    broken[1].callback = [](std::span<const Value>) -> Values { throw std::runtime_error("callback diagnostic"); };
    core::Instance throwing(module, broken);
    traps([&] { throwing.invoke("indirect-host", Values{{i32, 0}}); }, "callback diagnostic", 2);

    core::Instance* self = nullptr;
    broken[1].callback = [&](std::span<const Value> values) { return self->invoke(0, values); };
    core::Instance reentrant(module, broken);
    self = &reentrant;
    traps([&] { reentrant.invoke("indirect-host", Values{{i32, 0}}); }, "reentrant invocation", 2);
}

void test_indirect_calls() {
    core::Module module;
    module.types = {{{i32}, {i32}}, {{i32}, {i32}}, {{i32, i32}, {i32}}, {{}, {}}};
    add_function(module, 0, {0x20,0, 0x0b});
    add_function(module, 3, {0x0b});
    add_function(module, 2, {0x20,0, 0x20,1, 0x11,1,0, 0x0b});
    module.tables = {{funcref, {3, 3}}};
    module.elements = {{core::SegmentMode::active, 0, program({0x41,0, 0x0b}), funcref,
                        {program({0xd2,0, 0x0b}), program({0xd2,1, 0x0b})}}};

    core::Instance instance(std::move(module));
    // A different (structurally equivalent) type index matches.
    CHECK(instance.invoke(2, Values{{i32, 9}, {i32, 0}}) == Values{{i32, 9}});
    traps([&] { instance.invoke(2, Values{{i32, 9}, {i32, 1}}); }, "type mismatch", 4);
    traps([&] { instance.invoke(2, Values{{i32, 9}, {i32, 2}}); }, "null function", 4);
    traps([&] { instance.invoke(2, Values{{i32, 9}, {i32, 3}}); }, "out-of-bounds", 4);
}

void test_start_and_instance_state() {
    core::Module module;
    module.types = {{{}, {}}, {{}, {i32}}};
    module.globals = {{{i32, true}, program({0x41,0, 0x0b})}};
    add_function(module, 0, {0x41,5, 0x24,0, 0x0b});
    add_function(module, 1, {0x23,0, 0x41,1, 0x6a, 0x24,0, 0x23,0, 0x0b});
    add_function(module, 0, {0x41,9, 0x24,0, 0x00, 0x0b});
    module.start = 0;

    core::Instance first(module), second(module);
    CHECK(first.invoke(1) == Values{{i32, 6}});
    CHECK(first.invoke(1) == Values{{i32, 7}});
    CHECK(second.invoke(1) == Values{{i32, 6}});
    traps([&] { first.invoke(2); }, "unreachable", 4);
    CHECK(first.invoke(1) == Values{{i32, 10}}); // A trap does not roll back prior writes.
}

void test_validates_before_start() {
    core::Module module;
    module.types = {{{}, {}}};
    module.imports = {{"host", "start", std::uint32_t{0}}};
    module.start = 0;
    // Validation rejects an undeclared result even in a function the start
    // never calls, before a host callback could observe any side effects.
    add_function(module, 0, {0x41, 0, 0x0b});
    unsigned started = 0;
    const std::array bindings{core::HostFunction{"host", "start", [&](std::span<const Value>) {
        ++started;
        return Values{};
    }}};
    rejects([&] { core::Instance instance(module, bindings); }, "operand");
    CHECK(started == 0);

    module.functions.clear();
    module.imports.push_back({"other", "global", core::GlobalType{i32, false}});
    rejects([&] { core::Instance instance(module, bindings); }, "missing host global binding");
    CHECK(started == 0);
}

void test_binary_offsets() {
    core::Module source;
    source.types = {{{}, {}}};
    source.exports = {{"fail", core::ExternalKind::function, 0}};
    add_function(source, 0, {0x01, 0x00, 0x0b});
    auto module = core::decode_module(core::encode_module(source));
    const auto offset = module.functions.front().body.instructions[1].offset;
    CHECK(offset > 8);
    core::Instance instance(std::move(module));
    traps([&] { instance.invoke("fail"); }, "unreachable", offset);
}

void test_memory_exports() {
    using Bytes = std::vector<std::uint8_t>;
    core::Module module;
    module.types = {{{}, {i32}}, {{}, {}}};
    module.memories = {{{1, 2}}};
    module.data = {{core::SegmentMode::active, 0, program({0x41,1, 0x0b}), {1, 2, 3}}};
    module.exports = {{"mem", core::ExternalKind::memory, 0},
                      {"alias", core::ExternalKind::memory, 0},
                      {"load", core::ExternalKind::function, 0}};
    add_function(module, 0, {0x41,1, 0x2d,0,0, 0x0b});
    add_function(module, 1, {0x41,1, 0x41,9, 0x3a,0,0, 0x0b});
    add_function(module, 0, {0x41,1, 0x40,0, 0x0b});

    constexpr std::uint64_t page = 65'536;
    core::Instance instance(std::move(module));
    const auto& view = instance;
    const auto snapshot = view.read_memory("mem", 0, 4);
    CHECK(snapshot == Bytes{0, 1, 2, 3});
    auto source = Bytes{42, 43};
    instance.write_memory("alias", 1, source);
    source[0] = 88;
    CHECK(instance.invoke("load") == Values{{i32, 42}});
    instance.invoke(1);
    CHECK(view.read_memory("mem", 1, 2) == Bytes{9, 43});

    CHECK(view.read_memory("mem", page, 0).empty());
    instance.write_memory("mem", page, {});
    rejects([&] { (void)view.read_memory("mem", page + 1, 0); }, "out-of-bounds");
    rejects([&] { (void)view.read_memory("mem", 1, std::numeric_limits<std::size_t>::max()); }, "out-of-bounds");
    rejects([&] { instance.write_memory("mem", std::numeric_limits<std::uint64_t>::max(), {}); },
            "out-of-bounds");
    rejects([&] { instance.write_memory("mem", page - 1, source); }, "out-of-bounds");
    CHECK(view.read_memory("mem", page - 1, 1) == Bytes{0}); // Invalid writes don't partly copy.
    rejects([&] { (void)view.read_memory("load", 0, 1); }, "unknown memory export");
    rejects([&] { instance.write_memory("missing", 0, source); }, "unknown memory export");

    CHECK(instance.invoke(2) == Values{{i32, 1}});
    CHECK(view.read_memory("mem", page, 2) == Bytes{0, 0});
    instance.write_memory("mem", page, source);
    CHECK(view.read_memory("mem", page, 2) == source);
    CHECK(snapshot == Bytes{0, 1, 2, 3}); // The copy survives writes and growth.

    core::Instance moved(std::move(instance));
    CHECK(moved.read_memory("mem", page, 2) == source);
    rejects([&] { (void)view.read_memory("mem", 0, 1); }, "moved-from instance");
}

void test_global_exports() {
    core::Module module;
    module.types = {{{}, {i32}}, {{i32}, {}}};
    module.globals = {
        {{i32, true}, program({0x41,4, 0x0b})},
        {{i32, false}, program({0x41,7, 0x0b})},
        {{v128, true}, {{{0xfd, 0x0c}, core::VectorBytes{}}, {{0x0b}, {}}}},
    };
    module.exports = {{"counter", core::ExternalKind::global, 0},
                      {"constant", core::ExternalKind::global, 1},
                      {"vector", core::ExternalKind::global, 2},
                      {"get", core::ExternalKind::function, 0}};
    add_function(module, 0, {0x23,0, 0x0b});
    add_function(module, 1, {0x20,0, 0x24,0, 0x0b});

    core::Instance instance(std::move(module));
    const auto& view = instance;
    CHECK(view.get_global("counter") == Value{i32, 4});
    CHECK(view.get_global("constant") == Value{i32, 7});
    instance.set_global("counter", {i32, 5});
    CHECK(instance.invoke("get") == Values{{i32, 5}});
    instance.invoke(1, Values{{i32, 6}});
    CHECK(view.get_global("counter") == Value{i32, 6});

    CHECK(view.get_global("vector") == Value{v128, 0, 0});
    const Value vector{v128, 0x0102030405060708ULL, 0x1112131415161718ULL};
    instance.set_global("vector", vector);
    CHECK(view.get_global("vector") == vector);
    rejects([&] { instance.set_global("counter", {i64, 0}); }, "type mismatch");
    rejects([&] { instance.set_global("counter", {i32, 1ULL << 32}); }, "type mismatch");
    rejects([&] { instance.set_global("counter", {i32, 0, 1}); }, "type mismatch");
    rejects([&] { instance.set_global("constant", {i32, 0}); }, "immutable");
    rejects([&] { (void)view.get_global("missing"); }, "unknown global export");
    rejects([&] { instance.set_global("get", {i32, 0}); }, "unknown global export");
    CHECK(view.get_global("counter") == Value{i32, 6});
    CHECK(view.get_global("constant") == Value{i32, 7});
}

void test_table_exports() {
    core::Module module;
    module.types = {{{}, {i32}}, {{i32}, {i32}}, {{externref}, {}}, {{}, {externref}}};
    module.tables = {{funcref, {1, 1}}, {externref, {1, 2}}};
    module.exports = {{"functions", core::ExternalKind::table, 0},
                      {"refs", core::ExternalKind::table, 1},
                      {"call", core::ExternalKind::function, 1}};
    add_function(module, 0, {0x41,37, 0x0b});
    add_function(module, 1, {0x20,0, 0x11,0,0, 0x0b});
    add_function(module, 2, {0x41,0, 0x20,0, 0x26,1, 0x0b});
    add_function(module, 3, {0x41,0, 0x25,1, 0x0b});
    add_function(module, 0, {0xd0,0x6f, 0x41,1, 0xfc,0x0f,1, 0x0b});

    core::Instance instance(std::move(module));
    const auto& view = instance;
    CHECK(view.get_table("functions", 0) == Value{funcref, 0});
    instance.set_table("functions", 0, {funcref, 1});
    CHECK(instance.invoke("call", Values{{i32, 0}}) == Values{{i32, 37}});
    rejects([&] { instance.set_table("functions", 0, {funcref, 6}); }, "element type");
    rejects([&] { instance.set_table("functions", 0, {funcref, 1, 1}); }, "element type");
    rejects([&] { instance.set_table("functions", 0, {externref, 0}); }, "element type");
    CHECK(view.get_table("functions", 0) == Value{funcref, 1});
    instance.set_table("functions", 0, {funcref, 0});
    traps([&] { instance.invoke("call", Values{{i32, 0}}); }, "null function", 2);

    CHECK(view.get_table("refs", 0) == Value{externref, 0});
    instance.set_table("refs", 0, {externref, 0x1234});
    CHECK(instance.invoke(3) == Values{{externref, 0x1234}});
    instance.invoke(2, Values{{externref, 0x4321}});
    CHECK(view.get_table("refs", 0) == Value{externref, 0x4321});
    rejects([&] { (void)view.get_table("refs", 1); }, "out-of-bounds");
    rejects([&] { instance.set_table("refs", 1ULL << 32, {externref, 0}); }, "out-of-bounds");
    CHECK(instance.invoke(4) == Values{{i32, 1}});
    instance.set_table("refs", 1, {externref, 0x55});
    CHECK(view.get_table("refs", 1) == Value{externref, 0x55});
    rejects([&] { (void)view.get_table("call", 0); }, "unknown table export");
    rejects([&] { instance.set_table("missing", 0, {externref, 0}); }, "unknown table export");
}

void test_shared_objects() {
    using enum core::ExternalKind;
    using Bytes = std::vector<std::uint8_t>;
    core::HostMemory host_memory(core::MemoryType{{1, 2}});
    core::HostTable host_table({externref, {3, 4}});
    core::HostGlobal counter({i32, true}, {i32, 0});
    const core::HostGlobal offset({i32, false}, {i32, 1});
    const core::HostGlobal element({externref, false}, {externref, 31});
    const std::array objects{
        core::HostObjectImport{"host", "alias", host_table},
        core::HostObjectImport{"host", "table", host_table},
        core::HostObjectImport{"host", "memory", host_memory},
        core::HostObjectImport{"host", "counter", counter},
        core::HostObjectImport{"host", "offset", offset},
        core::HostObjectImport{"host", "element", element},
    };
    host_memory.write(0, Bytes{7});
    host_table.set(0, {externref, 17});
    const auto snapshot = host_memory.read(0, 2);
    unsigned started = 0;
    const std::array functions{core::HostFunction{"host", "check", [&](std::span<const Value>) {
        CHECK(host_memory.read(0, 3) == Bytes{7, 8, 9});
        CHECK(host_table.get(1) == Value{externref, 31});
        CHECK(counter.get() == Value{i32, 1});
        ++started;
        return Values{};
    }}};

    core::Module module;
    module.types = {{{}, {}}, {{i32}, {i32}}, {{}, {v128}}};
    module.imports = {
        {"host", "table", core::TableType{externref, {2, 5}}},
        {"host", "counter", core::GlobalType{i32, true}},
        {"host", "check", std::uint32_t{0}},
        {"host", "memory", core::MemoryType{{1, 3}}},
        {"host", "offset", core::GlobalType{i32, false}},
        {"host", "alias", core::TableType{externref, {3, 4}}},
        {"host", "element", core::GlobalType{externref, false}},
    };
    module.tables = {{externref, {1, {}}}};
    module.globals = {{{i32, false}, program({0x23,1, 0x0b})}};
    module.exports = {{"memory", memory, 0}, {"counter", global, 0}, {"constant", global, 1},
                      {"defined-global", global, 3}, {"table", table, 0}, {"alias", table, 1},
                      {"defined-table", table, 2}};
    module.elements = {{core::SegmentMode::active, 0, program({0x23,1, 0x0b}), externref,
                        {program({0x23,2, 0x0b})}}};
    module.data = {{core::SegmentMode::active, 0, program({0x23,1, 0x0b}), {8, 9}}};
    module.start = 1;
    add_function(module, 0, {0x23,1, 0x24,0, 0x10,0, 0x0b});
    add_function(module, 1, {0x20,0, 0x40,0, 0x0b});
    add_function(module, 1, {0x23,2, 0x20,0, 0xfc,15,1, 0x0b});
    add_function(module, 0, {0x41,1, 0x41,0, 0x41,2, 0xfc,14,1,0, 0x0b});
    add_function(module, 2, {0x41,0, 0xfd,0,0,0, 0x0b});
    add_function(module, 1, {0x20,0, 0x2d,0,0, 0x0b});

    const core::HostImports imports{functions, objects};
    auto first = core::Instance::with_imports(module, imports);
    CHECK(started == 1);
    CHECK(first.get_global("defined-global") == Value{i32, 1});
    CHECK(first.get_table("defined-table", 0) == Value{externref, 0});
    CHECK(first.invoke(5) == Values{{v128, 0x090807, 0}});
    CHECK(first.read_memory("memory", 0, 3) == Bytes{7, 8, 9});
    CHECK(snapshot == Bytes{7, 0});
    rejects([&] { first.set_global("constant", {i32, 0}); }, "immutable");
    rejects([&] { first.set_table("alias", 0, {funcref, 0}); }, "element type");

    // This instance requires the new sizes, not the original declared minima.
    CHECK(first.invoke(2, Values{{i32, 1}}) == Values{{i32, 1}});
    CHECK(first.invoke(3, Values{{i32, 1}}) == Values{{i32, 3}});
    CHECK(host_memory.type().limits == core::Limits{2, 2});
    CHECK(host_table.type().limits == core::Limits{4, 4});
    module.start.reset();
    module.data.clear();
    module.elements.clear();
    std::get<core::MemoryType>(module.imports[3].desc).limits.min = 2;
    std::get<core::TableType>(module.imports[0].desc).limits.min = 4;
    auto second = core::Instance::with_imports(module, imports);

    CHECK(second.invoke(2, Values{{i32, 1}}) == Values{{i32, 0xffffffff}});
    CHECK(second.invoke(3, Values{{i32, 1}}) == Values{{i32, 0xffffffff}});
    second.write_memory("memory", 65536, Bytes{22});
    CHECK(host_memory.read(65536, 1) == Bytes{22});
    CHECK(first.invoke(6, Values{{i32, 65536}}) == Values{{i32, 22}});
    first.set_global("counter", {i32, 41});
    CHECK(counter.get() == Value{i32, 41});
    CHECK(second.get_global("counter") == Value{i32, 41});
    counter.set({i32, 99});
    CHECK(first.get_global("counter") == Value{i32, 99});

    first.invoke(4); // Overlapping table.copy through two indices that alias.
    CHECK(second.get_table("table", 1) == Value{externref, 17});
    CHECK(second.get_table("alias", 2) == Value{externref, 31});
    second.set_table("table", 3, {externref, 42});
    CHECK(host_table.get(3) == Value{externref, 42});
    host_table.set(3, {externref, 28});
    CHECK(first.get_table("alias", 3) == Value{externref, 28});
}

void test_object_link_errors() {
    core::Module module;
    const auto try_binding = [&](core::ImportDesc type, core::HostObjectImport object) {
        module.imports = {{"host", "item", std::move(type)}};
        const std::array bindings{std::move(object)};
        (void)core::Instance::with_imports(module, {.objects = bindings});
    };
    const core::HostObjectImport memory{"host", "item", core::HostMemory(core::MemoryType{{1, {}}})};
    const core::HostObjectImport bounded_memory{"host", "item", core::HostMemory(core::MemoryType{{1, 3}})};
    const core::HostObjectImport table{"host", "item", core::HostTable({externref, {1, 3}})};
    const core::HostObjectImport unbounded_table{"host", "item", core::HostTable({externref, {1, {}}})};
    const core::HostObjectImport global{"host", "item", core::HostGlobal({i32, true}, {i32, 0})};
    rejects([&] { try_binding(core::MemoryType{{2, {}}}, memory); }, "memory limits");
    rejects([&] { try_binding(core::MemoryType{{1, 4096}}, memory); }, "memory limits");
    rejects([&] { try_binding(core::MemoryType{{1, 2}}, bounded_memory); }, "memory limits");
    rejects([&] { try_binding(core::MemoryType{{1, 2}}, table); }, "wrong kind");
    rejects([&] { try_binding(core::TableType{funcref, {1, 3}}, table); }, "table element type");
    rejects([&] { try_binding(core::TableType{externref, {2, 3}}, table); }, "table limits");
    rejects([&] { try_binding(core::TableType{externref, {1, 2}}, table); }, "table limits");
    rejects([&] { try_binding(core::TableType{externref, {1, 4}}, unbounded_table); }, "table limits");
    rejects([&] { try_binding(core::GlobalType{i32, false}, global); }, "mutability mismatch");
    rejects([&] { try_binding(core::GlobalType{i64, true}, global); }, "type or mutability mismatch");
    rejects([&] { try_binding(core::GlobalType{i32, true}, {"Host", "item", global.object}); },
            "missing host global binding");
    try_binding(core::MemoryType{{1, {}}}, memory);
    try_binding(core::TableType{externref, {1, {}}}, unbounded_table);
    module.imports = {{"host", "item", core::MemoryType{{1, {}}}}};
    const std::array duplicate_objects{memory, table};
    rejects([&] { (void)core::Instance::with_imports(module, {.objects = duplicate_objects}); }, "duplicate");
    const std::array objects{memory};
    const std::array functions{core::HostFunction{"host", "item", [](auto) { return Values{}; }}};
    rejects([&] { (void)core::Instance::with_imports(module, {functions, objects}); }, "duplicate");
    rejects([&] { core::Instance instance(module, functions); }, "wrong kind");

    rejects([&] { core::HostMemory invalid(core::MemoryType{{0, 65'537}}); }, "invalid memory limits");
    rejects([&] { core::HostMemory invalid(core::MemoryType{{2, 1}}); }, "invalid memory limits");
    rejects([&] { core::HostMemory invalid(core::MemoryType{{1025, {}}}); }, "instance limit");
    rejects([&] { core::HostTable invalid({i32, {0, {}}}); }, "invalid table element");
    rejects([&] { core::HostTable invalid({externref, {2, 1}}); }, "invalid table limits");
    rejects([&] { core::HostTable invalid({externref, {1'048'577, {}}}); }, "instance limit");
    rejects([&] { core::HostGlobal invalid({i64, false}, {i32, 0}); }, "initial value type");
    rejects([&] { core::HostGlobal invalid({f32, true}, {f32, 1ULL << 32}); }, "initial value type");
    rejects([&] { core::HostGlobal invalid({funcref, true}, {funcref, 1}); }, "non-null funcref");
    auto object = std::get<core::HostGlobal>(global.object);
    rejects([&] { object.set({i32, 1, 1}); }, "type mismatch");
    core::HostGlobal immutable({v128, false}, {v128, 5, 9});
    CHECK(immutable.get() == Value{v128, 5, 9});
    rejects([&] { immutable.set({v128, 0}); }, "immutable");

    auto retained = [&] {
        core::Module source;
        source.imports = {{"host", "item", core::GlobalType{i32, true}}};
        source.exports = {{"retained", core::ExternalKind::global, 0}};
        const std::array temporary{
            core::HostObjectImport{"host", "item", core::HostGlobal({i32, true}, {i32, 10})}};
        return core::Instance::with_imports(std::move(source), {.objects = temporary});
    }();
    CHECK(retained.get_global("retained") == Value{i32, 10});
}

void test_funcref_import_lifetimes() {
    core::HostTable table({funcref, {1, 2}});
    core::HostGlobal global({funcref, true}, {funcref, 0});
    core::HostGlobal immutable({funcref, false}, {funcref, 0});
    const std::array objects{core::HostObjectImport{"host", "table", table},
                            core::HostObjectImport{"host", "global", global},
                            core::HostObjectImport{"host", "immutable", immutable}};
    core::Module module;
    module.types = {{{}, {i32}}, {{}, {}}};
    module.imports = {{"host", "table", core::TableType{funcref, {1, {}}}},
                      {"host", "global", core::GlobalType{funcref, true}},
                      {"host", "immutable", core::GlobalType{funcref, false}}};
    module.exports = {{"table", core::ExternalKind::table, 0}, {"global", core::ExternalKind::global, 0}};
    module.elements = {{core::SegmentMode::active, 0, program({0x41,0, 0x0b}), funcref,
                        {program({0xd2,0, 0x0b})}}};
    add_function(module, 0, {0x41,42, 0x0b});
    add_function(module, 0, {0x41,0, 0x11,0,0, 0x0b});
    add_function(module, 1, {0xd2,0, 0x24,0, 0x00, 0x0b});
    const core::HostImports imports{.objects = objects};
    {
        auto instance = core::Instance::with_imports(module, imports);
        CHECK(instance.invoke(1) == Values{{i32, 42}});
        CHECK(table.get(0) == Value{funcref, 1});
        instance.set_global("global", {funcref, 1});
        CHECK(global.get() == Value{funcref, 1});
        rejects([&] { table.set(0, {funcref, 1}); }, "non-null funcref");
        rejects([&] { global.set({funcref, 1}); }, "non-null funcref");
        rejects([&] { (void)core::Instance::with_imports(module, imports); }, "table already bound");

        core::Module other;
        other.imports = {module.imports[1]};
        rejects([&] { (void)core::Instance::with_imports(other, imports); }, "global already bound");
        other.imports = {module.imports[2]};
        auto shared_null = core::Instance::with_imports(other, imports);
        auto moved = std::move(instance);
        CHECK(moved.invoke(1) == Values{{i32, 42}});
    }
    CHECK(table.get(0) == Value{funcref, 0});
    CHECK(global.get() == Value{funcref, 0});

    module.start = 2;
    traps([&] { (void)core::Instance::with_imports(module, imports); }, "unreachable", 4);
    CHECK(table.get(0) == Value{funcref, 0});
    CHECK(global.get() == Value{funcref, 0});
    module.start.reset();
    CHECK(core::Instance::with_imports(module, imports).invoke(1) == Values{{i32, 42}});
    CHECK(table.get(0) == Value{funcref, 0});
}

void test_active_import_segment_trap() {
    core::HostMemory memory(core::MemoryType{{1, 1}});
    core::HostTable table({externref, {1, 1}});
    const std::array objects{core::HostObjectImport{"host", "memory", memory},
                            core::HostObjectImport{"host", "table", table}};
    core::Module module;
    module.imports = {{"host", "memory", core::MemoryType{{1, 1}}},
                      {"host", "table", core::TableType{externref, {1, 1}}}};
    module.elements = {{core::SegmentMode::active, 0, program({0x41,1, 0x0b}), externref,
                        {program({0xd0,0x6f, 0x0b})}}};
    const core::HostImports imports{.objects = objects};
    traps([&] { (void)core::Instance::with_imports(module, imports); }, "active element segment", 0);
    module.elements.clear();
    module.data = {{core::SegmentMode::active, 0, program({0x41,1, 0x0b}), {26}},
                   {core::SegmentMode::active, 0, program({0x41,0x7f, 0x0b}), {27}}};
    traps([&] { (void)core::Instance::with_imports(module, imports); }, "active data segment", 0);
    CHECK(memory.read(1, 1) == std::vector<std::uint8_t>{26}); // Earlier segments stay applied.
}

void test_shared_table_budget() {
    core::HostTable table({externref, {1, {}}});
    const std::array objects{core::HostObjectImport{"host", "table", table}};
    core::Module module;
    module.types = {{{i32}, {i32}}};
    module.imports = {{"host", "table", core::TableType{externref, {1, {}}}},
                      {"host", "table", core::TableType{externref, {1, {}}}}};
    add_function(module, 0, {0xd0,0x6f, 0x20,0, 0xfc,15,1, 0x0b});
    const core::HostImports imports{.objects = objects};
    auto first = core::Instance::with_imports(module, imports);
    {
        module.tables = {{externref, {1'048'574, {}}}};
        auto more_tables = core::Instance::with_imports(module, imports);
        CHECK(first.invoke(0, Values{{i32, 2}}) == Values{{i32, 0xffffffff}});
        CHECK(first.invoke(0, Values{{i32, 1}}) == Values{{i32, 1}});
        CHECK(more_tables.invoke(0, Values{{i32, 1}}) == Values{{i32, 0xffffffff}});
    }
    CHECK(first.invoke(0, Values{{i32, 2}}) == Values{{i32, 2}});
}

} // namespace

int main() {
    return test_main([] {
        test_calls_and_locals();
        test_structured_control();
        test_fuel_and_traps();
        test_host_imports();
        test_indirect_calls();
        test_start_and_instance_state();
        test_validates_before_start();
        test_binary_offsets();
        test_memory_exports();
        test_global_exports();
        test_table_exports();
        test_shared_objects();
        test_object_link_errors();
        test_funcref_import_lifetimes();
        test_active_import_segment_trap();
        test_shared_table_budget();
    });
}
