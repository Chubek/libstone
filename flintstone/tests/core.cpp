#include "test.hpp"
#include "frontend/visitor.hpp"
#include <libvm/BytecodeBinary.hpp>
#include <libvm/RegisterVM.hpp>
#include <libvm/StackVM.hpp>
#include <algorithm>
#include <bit>
#include <chrono>
#include <filesystem>
#include <limits>
#include <thread>

using namespace flintstone;
namespace {
void workflows() {
    Registry registry;
    auto module = example("affine.flint");
    require_valid(module, registry);
    CHECK(run(module, registry, "affine", {std::int64_t(10)}) == 42);
    auto text = print(module);
    CHECK(print(parse(text)) == text);
    lower(module, registry, Level::Core);
    CHECK(module.level == Level::Core && module.revision == 1);
    CHECK(module.function("affine").parameters[0].type == Type::integer(32));
    CHECK(run(module, registry, "affine", {std::int64_t(10)}) == 42);
    lower(module, registry, Level::Machine);
    CHECK(module.level == Level::Machine && module.revision == 2);
    CHECK(run(module, registry, "affine", {std::int64_t(10)}) == 42);
    CHECK(module.function("affine").blocks[0].operations[0].name == "vm.mul");
    auto sum = example("sum.flint");
    CHECK(verify(sum, registry).empty());
    CHECK(run(sum, registry, "sum", {std::int64_t(0)}) == 0);
    CHECK(run(sum, registry, "sum", {std::int64_t(10)}) == 45);
    CHECK(run(sum, registry, "sum", {std::int64_t(-1)}) == 0);
    CHECK(to_ssa(sum, sum.function("sum"), registry).verify().empty());
    for (const auto* backend : {"vm", "vm-stack"}) {
        auto artifact = emit(sum, registry, backend);
        CHECK(artifact.format == "fsvm" && artifact.relocations.empty());
        auto bytes = firestone::vm::decode(artifact.bytes);
        CHECK(bytes.verify().empty());
        firestone::vm::Environment environment; bind_vm_runtime(environment, bytes);
        auto answer = backend == std::string("vm") ?
            firestone::vm::RegisterVM{}.run(bytes, bytes.find("sum"), {std::int64_t(10)}, environment) :
            firestone::vm::StackVM{}.run(bytes, bytes.find("sum"), {std::int64_t(10)}, environment);
        CHECK(answer.state.result && std::get<std::int64_t>(*answer.state.result) == 45);
    }
    rejects([&] { execute(sum, registry, "sum", {std::int64_t(100)}, 3); }, "budget");
    auto cyclic = parse("module m level core { func @loop() -> void { ^e: cf.jump() to [^b]; ^b: cf.jump() to [^b]; } }");
    rejects([&] { execute(cyclic, registry, "loop", {}, 50); }, "budget");
    CHECK(binding_targets().size() == 9);
    CHECK(binding_spec(FLINTSTONE_SOURCE_DIR).find("Flintstone.xbind") != std::string::npos);
    rejects([&] { emit(module, registry, "nonexistent"); }, "backend.unavailable");
    auto wrong_target = module; wrong_target.target.name = "some-native-isa";
    rejects([&] { compile_vm(wrong_target, registry); }, "vm.target");
}
void diagnostics() {
    Registry r;
    const std::pair<const char*, const char*> cases[] = {
        {"module m level core {func @f()->i32 {^e: func.return(%bad);}}", "ssa.undefined"},
        {"module m level core {func @f()->i32 {^e: %x:i32=arith.constant(1:i32); %x:i32=arith.constant(2:i32); func.return(%x);}}", "ssa.definition"},
        {"module m level core {func @f()->i32 {^e: %x:i32=arith.add(1:i32); func.return(%x);}}", "operation.arity"},
        {"module m level core {func @f()->i32 {^e: %x:i32=arith.add(1:i32, 2:i64); func.return(%x);}}", "type.mismatch"},
        {"module m level core {func @f()->i8 {^e: func.return(128:i8);}}", "literal.range"},
        {"module m level core {func @f()->i1 {^e: func.return(2:i1);}}", "literal.range"},
        {"module m level core {func @f()->i32 {^e: func.return(1:i32); arith.copy(2:i32);}}", "cfg.terminator"},
        {"module m level core {func @f()->i32 {^e: func.return(1:i32) to [^e];}}", "cfg.entry"},
        {"module m level core {func @f()->i32 {^e: cf.jump() to [^missing];}}", "cfg.successor"},
        {"module m level core {func @f()->i32 {^e: func.return(1:i32); ^dead:func.return(0:i32);}}", "cfg.unreachable"},
        {"module m level core {func @f()->i32 {^e: func.return(1:i64);}}", "return.type"},
        {"module m level core {func @f()->i32 {^e: %x:i32=alien.add(1:i32,2:i32); func.return(%x);}}", "operation.unknown"},
        {"module m level core {dialect arith {overflow=\"unchecked\";} func @f()->i32 {^e:func.return(0:i32);}}", "arith.semantics"},
        {"module m level core {dialect arith {missing=true;} }", "dialect.parameter"},
        {"module m level core {types {T=param<U>; U=param<T>;} }", "type.cycle"},
        {"module m level core {types {T=param<T>;} }", "type.cycle"},
        {"module m level core {func @f(%x:param<T>)->void {^e:func.return();}}", "type.unbound"},
        {"module m level core {func @f(%x:void)->void {^e:func.return();}}", "type.void"},
        {"module m level core {func @f(%x:vector<0,i32>)->void {^e:func.return();}}", "parse.type"},
        {"module m level core {func @f(%x:opaque<alien.box>)->void {^e:func.return();}}", "dialect.unknown"},
        {"module m level core {func @f()->void {^e: func.call() attrs {callee=\"absent\";};func.return();}}", "symbol.unknown"},
        {"module m level core {extern func @f(%a:i32)->void; func @g()->void {^e:func.call() attrs {callee=\"f\";};func.return();}}", "call.arity"},
        {"module m level machine {func @f()->i32 {^e:func.return(1:i32);}}", "level.operation"},
        {"module m level core {func @f(%p:ptr<gpu>)->void {^e:mem.store(%p,1:i32);func.return();}}", "memory.address-space"},
        {"module m level core {func @f()->f32 {^e:func.return(1.0:f32);}}", "literal.float"},
        // The reserved namespace holds the trap intrinsic and nothing else.
        {"module m level core {func @f()->i32 {^e:%r:i32=func.call() attrs {callee=\"__flintstone_evil\";};func.return(%r);}}", "symbol.reserved"},
        {"module m level core {func @f()->i32 {^e:%r:i32=func.call(1:i32) attrs {callee=\"__flintstone_trap\";};func.return(%r);}}", "call.arity"},
        {"module m level core {func @f()->i32 {^e:%r:i64=func.call() attrs {callee=\"__flintstone_trap\";};func.return(%r);}}", "call.result"},
        {"module m level core {func @__flintstone_trap()->i32 {^e:func.return(1:i32);}}", "symbol.reserved"},
    };
    for (auto [source, code] : cases) rejects([&] { require_valid(parse(source, "bad.flint"), r); }, code);
    auto bad_phi = example("sum.flint");
    bad_phi.function("sum").blocks[1].operations[0].incoming.pop_back();
    auto errors = verify(bad_phi, r); CHECK(!errors.empty()); CHECK(errors.front().code == "ssa.phi");
    CHECK(errors.front().location.file == "sum.flint" && errors.front().function == "sum");
    auto dominance = parse(R"(module m level core { func @f(%c:i1)->i32 {
      ^e: cf.branch(%c) to [^a,^b];
      ^a: %x:i32=arith.constant(1:i32); cf.jump() to [^b];
      ^b: func.return(%x);
    } })");
    rejects([&] { require_valid(dominance, r); }, "ssa.dominance");
    rejects([&] { parse("module m level core {} trailing"); }, "trailing");
    rejects([&] { parse("module m level core { attrs { s=\"bad\\q\"; } }"); }, "escape");
    rejects([&] { parse("module m level core { attrs { s=1; s=2; } }"); }, "duplicate");
    rejects([&] { parse("module m level core { attrs { s=9223372036854775808; } }"); }, "int64");
    rejects([&] { parse("module m level core {}", "limits", {3, 10, 2}); }, "limit");
    rejects([&] { parse("module m level core {}", "limits", {100, 2, 2}); }, "limit");
    rejects([&] { parse("module m level core {types{T=tuple<tuple<i32>>;}}", "limits", {100, 100, 2}); }, "nesting");
    auto attrs = parse("module m level core {attrs {s=\"quoted \\\"x\\\"\\n\"; f=1.0; b=true; t=type<tuple<i32,ptr<gpu>>>;}} ");
    CHECK(print(parse(print(attrs))) == print(attrs));
    Module aliases; aliases.name = "expansion";
    aliases.type_bindings["T0"] = Type::integer();
    for (unsigned n = 1; n <= 19; ++n) {
        auto previous = Type::parameter("T" + std::to_string(n - 1));
        aliases.type_bindings["T" + std::to_string(n)] = Type::tuple({previous, previous});
    }
    rejects([&] { require_valid(aliases, r); }, "type.limit");
}
void calls_and_memory() {
    Registry r;
    auto module = parse(R"(module calls level core {
      func @even(%n:i32)->i1 {
        ^e: %done:i1=arith.eq(%n,0:i32); cf.branch(%done) to [^yes,^more];
        ^yes: func.return(1:i1);
        ^more: %next:i32=arith.sub(%n,1:i32); %r:i1=func.call(%next) attrs {callee="odd";}; func.return(%r);
      }
      func @odd(%n:i32)->i1 {
        ^e: %done:i1=arith.eq(%n,0:i32); cf.branch(%done) to [^yes,^more];
        ^yes: func.return(0:i1);
        ^more: %next:i32=arith.sub(%n,1:i32); %r:i1=func.call(%next) attrs {callee="even";}; func.return(%r);
      }
      extern func @host(%a:i32)->i32;
      func @external()->i32 {^e: %r:i32=func.call(40:i32) attrs {callee="host";}; func.return(%r);}
      func @memory(%p:ptr<generic>)->i32 {^e: mem.store(%p,42:i32); %x:i32=mem.load(%p); func.return(%x);}
      func @fp(%a:f64)->f64 {^e: %x:f64=arith.mul(%a,2.0:f64); func.return(%x);}
      func @void()->void {^e: func.return();}
    })");
    CHECK(std::get<bool>(execute(module, r, "even", {std::int64_t(10)})));
    CHECK(!std::get<bool>(execute(module, r, "even", {std::int64_t(11)})));
    firestone::vm::Environment environment;
    environment.bind("host", {{firestone::vm::ValueKind::Integer}, firestone::vm::ValueKind::Integer},
                     [](const auto& args) -> firestone::vm::Value { return std::get<std::int64_t>(args[0]) + 2; });
    CHECK(std::get<std::int64_t>(execute(module, r, "external", {}, 100, &environment)) == 42);
    auto address = environment.allocate();
    CHECK(std::get<std::int64_t>(execute(module, r, "memory", {address}, 100, &environment)) == 42);
    CHECK(std::get<std::int64_t>(environment.load(address)) == 42);
    CHECK(std::get<double>(execute(module, r, "fp", {21.0})) == 42.0);
    CHECK(std::holds_alternative<std::monostate>(execute(module, r, "void")));
    rejects([&] { execute(module, r, "host"); }, "entry");
    auto boolean = parse("module m level core {func @id(%b:i1)->i1 {^e:func.return(%b);}}");
    CHECK(std::get<bool>(execute(boolean, r, "id", {true})));
    rejects([&] { execute(boolean, r, "id", {std::int64_t(1)}); }, "vm.arguments");
    rejects([&] { execute(module, r, "fp", {std::numeric_limits<double>::infinity()}); }, "vm.arguments");
    rejects([&] { execute(module, r, "memory", {std::int64_t(0)}); }, "vm.arguments");
    rejects([&] { execute(module, r, "even", {true}); }, "vm.arguments");
    auto narrow = parse("module narrow level core {func @f(%x:i8)->i8 {^e:func.return(%x);}}");
    auto bytes = compile_vm(narrow, r);
    bind_vm_runtime(environment, bytes);
    rejects([&] { firestone::vm::RegisterVM{}.run(bytes, bytes.find("f"), {std::int64_t(128)}, environment); }, "width");
    auto external = parse("module m level core {extern func @bad()->i8;func @f()->i8 {^e:%x:i8=func.call() attrs{callee=\"bad\";};func.return(%x);}}");
    environment.bind("bad", {{}, firestone::vm::ValueKind::Integer}, [](const auto&) -> firestone::vm::Value { return std::int64_t(128); });
    rejects([&] { execute(external, r, "f", {}, 1000, &environment); }, "width");
    auto gpu = parse("module m level core {func @f(%p:ptr<gpu>)->void {^e:mem.store(%p,1:i32);func.return();}}");
    rejects([&] { compile_vm(gpu, r); }, "address-space");
}
void modular_and_optimization() {
    Registry r;
    for (unsigned bits : {8u, 32u, 64u}) {
        const auto t = "i" + std::to_string(bits);
        auto module = parse("module arithmetic level core {func @f(%a:" + t + ",%b:" + t + ")->" + t +
            "{^e:%p:" + t + "=arith.mul(%a,%b);%z:" + t + "=arith.add(%p,0:" + t + ");func.return(%z);}}");
        auto optimized = module;
        PassManager manager; manager.add(blackstone_pass(BLACKSTONE_KERNEL_DIR, {"add_zero"})); manager.add(dead_code_elimination());
        auto reports = manager.run(optimized, r);
        CHECK(reports.size() == 2 && reports[0].changed && optimized.revision >= 1);
        for (auto a : {INT64_MIN, INT64_C(-128), INT64_C(-1), INT64_C(0), INT64_C(1), INT64_C(127), INT64_MAX}) {
            if (bits < 64 && (a < -(INT64_C(1) << (bits - 1)) || a >= (INT64_C(1) << (bits - 1)))) continue;
            auto value = static_cast<std::uint64_t>(a) * std::uint64_t(3);
            auto mask = bits == 64 ? UINT64_MAX : (UINT64_C(1) << bits) - 1;
            value &= mask;
            if (bits < 64 && (value & (UINT64_C(1) << (bits - 1)))) value |= ~mask;
            auto expected = std::bit_cast<std::int64_t>(value);
            CHECK(run(module, r, "f", {a, std::int64_t(3)}) == expected);
            CHECK(run(optimized, r, "f", {a, std::int64_t(3)}) == expected);
        }
        auto simple = [&](const std::string& op, const std::string& args) {
            return parse("module m level core {func @f()->" + t + " {^e:%r:" + t + "=arith." + op + "(" + args + ");func.return(%r);}}");
        };
        auto min = bits == 64 ? INT64_MIN : -(INT64_C(1) << (bits - 1));
        auto max = bits == 64 ? INT64_MAX : (INT64_C(1) << (bits - 1)) - 1;
        CHECK(run(simple("add", std::to_string(max) + ":" + t + ",1:" + t), r, "f") == min);
        CHECK(run(simple("neg", std::to_string(min) + ":" + t), r, "f") == min);
        CHECK(run(simple("div", std::to_string(min) + ":" + t + ",-1:" + t), r, "f") == min);
        CHECK(run(simple("shl", "-1:" + t + ",1:" + t), r, "f") == -2);
        CHECK(run(simple("shr", "-2:" + t + ",1:" + t), r, "f") == -1);
        rejects([&] { run(simple("div", "1:" + t + ",0:" + t), r, "f"); }, "zero");
        rejects([&] { run(simple("shl", "1:" + t + "," + std::to_string(bits) + ":" + t), r, "f"); }, "shift");
    }
    auto effects = parse(R"(module effects level core {extern func @effect()->i32;
      func @f(%p:ptr<generic>)->i32 {^e:
        %dead:i32=arith.add(1:i32,2:i32);
        %load:i32=mem.load(%p);
        %call:i32=func.call() attrs{callee="effect";};
        %trap:i32=arith.div(1:i32,0:i32);
        func.return(42:i32);
      }})");
    PassManager dce; dce.add(dead_code_elimination()); CHECK(dce.run(effects, r)[0].changed);
    CHECK(effects.function("f").blocks[0].operations.size() == 4);
    auto sum = example("sum.flint"); auto original = sum;
    // Add a legal IV scale in the loop body, retaining stable phi identities.
    auto& f = sum.function("sum"); auto& body = f.blocks[2];
    auto iv = f.blocks[1].operations[0].results[0]; auto id = f.fresh_value();
    Operation scaled{"arith.mul", {{id, Type::integer(), {}}}, {Operand::ref(iv.id), Operand::constant(7)}, {}, {}, {}, {}};
    body.operations.insert(body.operations.begin(), scaled);
    body.operations[1].operands[1] = Operand::ref(id);
    original = sum;
    PassManager strength; strength.add(blackstone_pass(BLACKSTONE_KERNEL_DIR, {"strength_reduction"}));
    CHECK(strength.run(sum, r)[0].changed);
    for (int limit : {0, 1, 2, 10, 20}) CHECK(run(sum, r, "sum", {std::int64_t(limit)}) == run(original, r, "sum", {std::int64_t(limit)}));
}
void aggregate_dialects() {
    Registry r;
    {
        auto names = r.dialect_names();
        CHECK(names.size() == 10);
        for (const char* dialect : {"arith", "atomic", "cf", "func", "math", "mem", "ptr", "tuple", "vector", "vm"})
            CHECK(std::find(names.begin(), names.end(), dialect) != names.end());
        auto backends = r.backend_names();
        CHECK(std::find(backends.begin(), backends.end(), "vm") != backends.end());
        CHECK(std::find(backends.begin(), backends.end(), "vm-stack") != backends.end());
    }
    auto scalarized = [&](Module module) {
        PassManager manager;
        manager.add(vector_scalarize_pass());
        manager.add(tuple_sroa_pass());
        manager.add(dead_code_elimination());
        manager.run(module, r);
        return module;
    };
    // DCE must not erase an unused vector operation that can trap.
    for (const char* operation : {"div", "mod"}) {
        auto module = parse(std::string(R"(module v level high { func @f() -> i32 {
          ^e: %a:vector<2,i32> = vector.splat(1:i32);
              %b:vector<2,i32> = vector.splat(0:i32);
              %unused:vector<2,i32> = vector.)") + operation + R"((%a, %b);
              func.return(42:i32);
        }})");
        PassManager dce;
        dce.add(dead_code_elimination());
        CHECK(!dce.run(module, r)[0].changed);
        auto lowered = scalarized(module);
        rejects([&] { run(lowered, r, "f"); }, "zero");
    }
    {
        auto module = parse(R"(module t level high { func @f() -> i32 {
          ^e: %t:tuple<i32> = tuple.build(@answer:i32);
              %r:i32 = tuple.extract(%t) attrs {index=0;};
              func.return(%r);
        }})");
        require_valid(module, r);
        auto lowered = scalarized(module);
        CHECK(lowered.function("f").blocks[0].operations[0].name == "arith.copy");
        firestone::vm::Environment environment;
        environment.set_global("answer", std::int64_t(42));
        CHECK(std::get<std::int64_t>(execute(lowered, r, "f", {}, 1000, &environment)) == 42);
    }
    {
        auto module = parse(R"(module v level high { func @f(%x:i32) -> i32 {
          ^e:
            %v:vector<4,i32> = vector.splat(%x);
            %w:vector<4,i32> = vector.mul(%v, %v);
            %r:i32 = vector.extract(%w) attrs {lane = 2;};
            func.return(%r);
        }})");
        require_valid(module, r);
        CHECK(run(scalarized(module), r, "f", {std::int64_t(6)}) == 36);
    }
    {
        auto module = parse(R"(module vl level high { func @sum(%n:i32) -> i32 {
          ^entry:
            %init:vector<4,i32> = vector.splat(0:i32);
            cf.jump() to [^loop];
          ^loop:
            %i:i32 = cf.phi(0:i32, %next) from [^entry, ^body];
            %acc:vector<4,i32> = cf.phi(%init, %step) from [^entry, ^body];
            %continue:i1 = arith.lt(%i, %n);
            cf.branch(%continue) to [^body, ^exit];
          ^body:
            %one:vector<4,i32> = vector.splat(1:i32);
            %step:vector<4,i32> = vector.add(%acc, %one);
            %next:i32 = arith.add(%i, 1:i32);
            cf.jump() to [^loop];
          ^exit:
            %r:i32 = vector.extract(%acc) attrs {lane = 0;};
            func.return(%r);
        }})");
        require_valid(module, r);
        CHECK(run(scalarized(module), r, "sum", {std::int64_t(10)}) == 10);
        CHECK(run(scalarized(module), r, "sum", {std::int64_t(0)}) == 0);
    }
    {
        auto module = parse(R"(module t level high { func @f(%x:i32, %y:i32) -> i32 {
          ^e:
            %t:tuple<i32,i32> = tuple.build(%x, %y);
            %a:i32 = tuple.extract(%t) attrs {index = 1;};
            %b:i32 = tuple.extract(%t) attrs {index = 0;};
            %r:i32 = arith.add(%a, %b);
            func.return(%r);
        }})");
        require_valid(module, r);
        CHECK(run(scalarized(module), r, "f", {std::int64_t(20), std::int64_t(22)}) == 42);
    }
    {
        auto module = parse(R"(module tl level high { func @f(%n:i32) -> i32 {
          ^entry:
            %z:tuple<i32,i32> = tuple.build(0:i32, 0:i32);
            cf.jump() to [^loop];
          ^loop:
            %i:i32 = cf.phi(0:i32, %next) from [^entry, ^body];
            %p:tuple<i32,i32> = cf.phi(%z, %q) from [^entry, ^body];
            %c:i1 = arith.lt(%i, %n);
            cf.branch(%c) to [^body, ^exit];
          ^body:
            %a:i32 = tuple.extract(%p) attrs {index = 0;};
            %b:i32 = tuple.extract(%p) attrs {index = 1;};
            %a2:i32 = arith.add(%a, 1:i32);
            %b2:i32 = arith.add(%b, 2:i32);
            %q:tuple<i32,i32> = tuple.build(%a2, %b2);
            %next:i32 = arith.add(%i, 1:i32);
            cf.jump() to [^loop];
          ^exit:
            %x:i32 = tuple.extract(%p) attrs {index = 0;};
            %y:i32 = tuple.extract(%p) attrs {index = 1;};
            %t:i32 = arith.add(%x, %y);
            func.return(%t);
        }})");
        require_valid(module, r);
        CHECK(run(scalarized(module), r, "f", {std::int64_t(10)}) == 30);
    }
    {
        auto module = parse(R"(module m level high { func @f(%x:i32) -> i32 {
          ^e:
            %c:i32 = math.cube(%x);
            %a:i32 = math.abs(%x);
            %r:i32 = arith.add(%c, %a);
            func.return(%r);
        }})");
        require_valid(module, r);
        CHECK(run(module, r, "f", {std::int64_t(3)}) == 30);
        CHECK(run(module, r, "f", {std::int64_t(-3)}) == -24);
    }
    {
        auto module = parse(R"(module tr level high { func @f(%c:i1) -> i32 {
          ^e:
            cf.branch(%c) to [^taken, ^live];
          ^taken:
            cf.trap();
          ^live:
            func.return(42:i32);
        }})");
        require_valid(module, r);
        CHECK(run(module, r, "f", {false}) == 42);
        rejects([&] { run(module, r, "f", {true}); }, "vm.trap");
        auto done = module;
        lower(done, r, Level::Core);
        CHECK(done.function("f").blocks[1].operations[0].name == "func.call");
        auto idle = parse("module t level high { func @t() -> void { ^e: cf.trap(); } }");
        rejects([&] { run(idle, r, "t"); }, "vm.trap");
        // The trap intrinsic itself is reachable from IL with the documented
        // signature: no arguments, and the enclosing function's result type.
        auto direct = parse(R"(module d level core { func @d() -> i32 {
          ^e: %r:i32 = func.call() attrs {callee="__flintstone_trap";}; func.return(%r); } })");
        require_valid(direct, r);
        rejects([&] { run(direct, r, "d"); }, "vm.trap");
        auto void_trap = parse(R"(module dv level core { func @v() -> void {
          ^e: func.call() attrs {callee="__flintstone_trap";}; func.return(); } })");
        require_valid(void_trap, r);
        rejects([&] { run(void_trap, r, "v"); }, "vm.trap");
    }
    const std::pair<const char*, const char*> cases[] = {
        {"module m level core {func @f()->i32 {^e:%v:vector<4,i32>=vector.splat(1:i32);func.return(1:i32);}}", "level.operation"},
        {"module m level high {func @f()->i32 {^e:%a:vector<4,i32>=vector.splat(1:i32);%b:vector<2,i32>=vector.splat(2:i32);%c:vector<4,i32>=vector.add(%a,%b);func.return(1:i32);}}", "vector.type"},
        {"module m level high {func @f()->i32 {^e:%v:vector<300,i32>=vector.splat(1:i32);func.return(1:i32);}}", "vector.width"},
        {"module m level high {func @f()->i32 {^e:%v:vector<4,i1>=vector.splat(1:i1);func.return(1:i32);}}", "vector.element"},
        {"module m level high {func @f()->i32 {^e:%v:vector<4,i32>=vector.splat(1:i32);%r:i32=vector.extract(%v) attrs {lane = 4;};func.return(%r);}}", "vector.lane"},
        {"module m level high {func @f()->i32 {^e:%t:tuple<i32,i32,i32>=tuple.build(1:i32,2:i32);func.return(1:i32);}}", "tuple.arity"},
        {"module m level high {func @f(%x:i32)->i32 {^e:%t:tuple<i32>=tuple.build(%x);%r:i32=tuple.extract(%t) attrs {index = 1;};func.return(%r);}}", "tuple.index"},
        {"module m level high {func @f(%x:i32)->i32 {^e:%u:tuple<i32>=tuple.build(%x);%t:tuple<tuple<i32>>=tuple.build(%u);func.return(%x);}}", "tuple.field"},
        {"module m level core {func @f(%p:ptr<generic>)->i32 {^e:%x:i32=atomic.load(%p) attrs {ordering = \"release\";};func.return(%x);}}", "atomic.ordering"},
        {"module m level core {func @f(%p:ptr<generic>,%x:i32)->void {^e:atomic.store(%p,%x) attrs {ordering = \"acquire\";};func.return();}}", "atomic.ordering"},
        {"module m level core {func @f(%x:i32)->i32 {^e:%q:ptr<generic>=ptr.add(%x,%x);func.return(%x);}}", "ptr.address"},
        {"module m level high {func @f()->void {^e:cf.jump() to [^t]; ^t:cf.trap() to [^t];}}", "cf.targets"},
        {"module m level high {func @f()->i32 {^e:%r:i32=math.abs(1.0:f64);func.return(%r);}}", "math.type"},
    };
    for (auto [source, code] : cases) rejects([&] { require_valid(parse(source, "bad.flint"), r); }, code);
    {
        auto direct = parse("module m level core {func @f(%x:i32)->i32 {^e:%r:i32=arith.add(%x,1:i32);func.return(%r);}}");
        PassManager unchanged;
        unchanged.add(vector_scalarize_pass());
        unchanged.add(tuple_sroa_pass());
        CHECK(!unchanged.run(direct, r)[0].changed && !unchanged.run(direct, r)[1].changed);
    }
    {
        auto param = parse("module m level high {func @f(%v:vector<4,i32>)->i32 {^e:%r:i32=vector.extract(%v) attrs {lane=0;};func.return(%r);}}");
        PassManager manager;
        manager.add(vector_scalarize_pass());
        rejects([&] { manager.run(param, r); }, "vector.escape");
    }
    {
        auto stored = parse(R"(module m level high { func @f(%p:ptr<generic>, %x:i32) -> i32 {
          ^e: %v:vector<4,i32> = vector.splat(%x); mem.store(%p, %x); %r:i32 = vector.extract(%v) attrs {lane = 1;}; func.return(%r); } })");
        // The stored scalar is fine; the extracted lane keeps the vector live and scalarizable.
        firestone::vm::Environment environment;
        const auto cell = environment.allocate();
        CHECK(std::get<std::int64_t>(execute(scalarized(stored), r, "f", {cell, std::int64_t(7)}, 1000000, &environment)) == 7);
        auto vector_store = parse(R"(module m level high { func @f(%p:ptr<generic>, %x:i32) -> i32 {
          ^e: %v:vector<4,i32> = vector.splat(%x); mem.store(%p, %v); func.return(%x); } })");
        PassManager manager;
        manager.add(vector_scalarize_pass());
        rejects([&] { manager.run(vector_store, r); }, "vector.escape");
    }
    {
        auto called = parse(R"(module m level high {
          extern func @g(%t:tuple<i32>) -> void;
          func @f(%x:i32) -> i32 { ^e: %t:tuple<i32> = tuple.build(%x); func.call(%t) attrs {callee = "g";}; func.return(%x); } })");
        PassManager manager;
        manager.add(tuple_sroa_pass());
        rejects([&] { manager.run(called, r); }, "tuple.escape");
    }
    {
        auto ptr = parse("module m level core {func @f(%p:ptr<generic>,%o:index)->ptr<generic> {^e:%q:ptr<generic>=ptr.add(%p,%o);func.return(%q);}}");
        require_valid(ptr, r);
        rejects([&] { compile_vm(ptr, r); }, "ssa.operation");
        auto atom = parse("module m level core {func @f(%p:ptr<generic>)->i32 {^e:%x:i32=atomic.load(%p) attrs {ordering=\"relaxed\";};func.return(%x);}}");
        require_valid(atom, r);
        rejects([&] { compile_vm(atom, r); }, "ssa.operation");
    }
}
void stdlib_and_imports() {
    Registry r;
    CHECK(!default_stdlib_dir().empty());
    // Parse + print round-trip of an unresolved module with imports.
    {
        auto m = parse(R"(module imp level core { import "math"; import "bit"; })");
        CHECK(m.imports == (std::vector<std::string>{"math", "bit"}));
        CHECK(print(parse(print(m))) == print(m));
        rejects([&] { require_valid(m, r); }, "import.unresolved");
    }
    // Importing the umbrella `std` links math, bit, fp and mem.
    {
        auto m = parse(R"(module app level core {
          import "std";
          extern func @std.math.gcd_i32(%a:i32, %b:i32) -> i32;
          func @entry() -> i32 {
            ^e:
              %g:i32 = func.call(-42:i32, 56:i32) attrs {callee = "std.math.gcd_i32";};
              %l:i32 = func.call(12:i32, 18:i32) attrs {callee = "std.math.lcm_i32";};
              %p:i32 = func.call(2:i32, 10:i32) attrs {callee = "std.math.pow_i32";};
              %f:i32 = func.call(5:i32) attrs {callee = "std.math.factorial_i32";};
              %fib:i32 = func.call(10:i32) attrs {callee = "std.math.fib_i32";};
              %sq:i32 = func.call(50:i32) attrs {callee = "std.math.isqrt_i32";};
              %cl:i32 = func.call(100:i32, 0:i32, 42:i32) attrs {callee = "std.math.clamp_i32";};
              %sg:i32 = func.call(-9:i32) attrs {callee = "std.math.sign_i32";};
              %pop:i32 = func.call(29:i32) attrs {callee = "std.bit.popcount_i32";};
              %ctz:i32 = func.call(40:i32) attrs {callee = "std.bit.ctz_i32";};
              %clz:i32 = func.call(16:i32) attrs {callee = "std.bit.clz_i32";};
              %bs:i32 = func.call(305419896:i32) attrs {callee = "std.bit.bswap_i32";};
              %t0:i32 = arith.add(%g, %l);
              %t1:i32 = arith.add(%t0, %p);
              %t2:i32 = arith.add(%t1, %f);
              %t3:i32 = arith.add(%t2, %fib);
              %t4:i32 = arith.add(%t3, %sq);
              %t5:i32 = arith.add(%t4, %cl);
              %t6:i32 = arith.add(%t5, %sg);
              %t7:i32 = arith.add(%t6, %pop);
              %t8:i32 = arith.add(%t7, %ctz);
              %t9:i32 = arith.add(%t8, %clz);
              %ok:i1 = arith.eq(%bs, 2018915346:i32);
              %r:i32 = arith.select(%ok, %t9, -1:i32);
              func.return(%r);
          }
          func @pow2(%x:i32) -> i1 { ^e: %r:i1 = func.call(%x) attrs {callee = "std.bit.is_pow2_i32";}; func.return(%r); }
          func @fp() -> f64 {
            ^e:
              %a:f64 = func.call(-4.0:f64) attrs {callee = "std.fp.abs_f64";};
              %c:f64 = func.call(%a, 0.0:f64, 3.0:f64) attrs {callee = "std.fp.clamp_f64";};
              %l:f64 = func.call(10.0:f64, 20.0:f64, 0.25:f64) attrs {callee = "std.fp.lerp_f64";};
              %r:f64 = arith.add(%c, %l);
              func.return(%r);
          }
          func @cells(%p:ptr<generic>, %q:ptr<generic>) -> i32 {
            ^e:
              mem.store(%p, 10:i32);
              mem.store(%q, 30:i32);
              func.call(%p, %q) attrs {callee = "std.mem.swap_i32";};
              %old:i32 = func.call(%p, 12:i32) attrs {callee = "std.mem.fetch_add_i32";};
              %cur:i32 = mem.load(%p);
              %r:i32 = arith.add(%old, %cur);
              func.return(%r);
          }
        })");
        resolve_imports(m, r);
        CHECK(m.imports.empty() && m.revision == 1);
        require_valid(m, r);
        // 14 + 36 + 1024 + 120 + 55 + 7 + 42 + (-1) + 4 + 3 + 27 = 1331
        CHECK(run(m, r, "entry") == 1331);
        CHECK(std::get<bool>(execute(m, r, "pow2", {std::int64_t(64)})));
        CHECK(!std::get<bool>(execute(m, r, "pow2", {std::int64_t(0)})));
        CHECK(!std::get<bool>(execute(m, r, "pow2", {std::int64_t(12)})));
        CHECK(std::get<double>(execute(m, r, "fp")) == 15.5);
        firestone::vm::Environment env;
        auto p = env.allocate(), q = env.allocate();
        CHECK(std::get<std::int64_t>(execute(m, r, "cells", {p, q}, 100000, &env)) == 72);
        CHECK(std::get<std::int64_t>(env.load(q)) == 10);
    }
    // Negative import contracts.
    rejects([&] { parse(R"(module m level core { import ""; })"); }, "parse.import");
    rejects([&] { parse(R"(module m level core { import "math"; import "math"; })"); }, "duplicate import");
    {
        auto m = parse(R"(module m level core { import "../secret"; })");
        rejects([&] { resolve_imports(m, r); }, "import.name");
    }
    {
        auto m = parse(R"(module m level core { import "no_such_lib"; })");
        rejects([&] { resolve_imports(m, r); }, "import.not_found");
    }
    {
        auto m = parse(R"(module m level core {
          import "math";
          extern func @std.math.min_i32(%a:i64, %b:i64) -> i64;
        })");
        rejects([&] { resolve_imports(m, r); }, "import.signature");
    }
    {
        auto m = parse(R"(module m level core {
          import "math";
          func @std.math.min_i32(%a:i32, %b:i32) -> i32 { ^e: func.return(%a); }
        })");
        rejects([&] { resolve_imports(m, r); }, "import.duplicate");
    }
    {
        auto m = parse(R"(module m level core {
          target "vm" { pointer_bits = 32; index_bits = 32; endian = "little"; }
          import "math";
        })");
        rejects([&] { resolve_imports(m, r); }, "import.target");
    }
    {
        auto m = parse(R"(module m level machine { import "math"; })");
        rejects([&] { resolve_imports(m, r); }, "import.level");
    }
    {
        const auto dir = std::filesystem::temp_directory_path() / "flintstone-cycle-test";
        std::filesystem::create_directories(dir);
        {
            std::ofstream a(dir / "ca.flint");
            a << "module ca level core { import \"cb\"; }\n";
            std::ofstream b(dir / "cb.flint");
            b << "module cb level core { import \"ca\"; }\n";
        }
        auto m = parse(R"(module m level core { import "ca"; })");
        rejects([&] { resolve_imports(m, r, {dir.string()}); }, "import.cycle");
        std::filesystem::remove_all(dir);
    }
}
void transitive_import_contracts() {
    Registry r;
    const auto dir = std::filesystem::temp_directory_path() /
        ("flintstone-import-contracts-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    CHECK(std::filesystem::create_directory(dir));
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
    } cleanup{dir};
    auto library = [&](const char* name, const std::string& source) {
        std::ofstream file(dir / (std::string(name) + ".flint"));
        file << source;
        file.close();
        CHECK(file.good());
    };
    library("parent", R"(module parent level core { import "child"; })");
    const std::pair<const char*, const char*> invalid_children[] = {
        {R"(module child level core { target "other" {} })", "import.target"},
        {R"(module child level core { target "vm" {pointer_bits=32;} })", "import.target"},
        {R"(module child level core { target "vm" {index_bits=32;} })", "import.target"},
        {R"(module child level core { target "vm" {endian="big";} })", "import.target"},
        {R"(module child level core { target "vm" {feature.required=true;} })", "import.feature"},
        {R"(module child level high {})", "import.level"},
    };
    for (auto [source, diagnostic] : invalid_children) {
        library("child", source);
        auto module = parse(R"(module app level core { import "parent"; })");
        module.revision = 9;
        const auto before = print(module);
        rejects([&] { resolve_imports(module, r, {dir.string()}); }, diagnostic);
        CHECK(print(module) == before && module.revision == 9);
    }
    library("parent", R"(module parent level high {
      import "child";
      dialect math {factor=2;}
    })");
    library("child", R"(module child level high { dialect math {factor=7;} })");
    auto conflict = parse(R"(module app level high { import "parent"; })");
    const auto before = print(conflict);
    rejects([&] { resolve_imports(conflict, r, {dir.string()}); }, "import.dialect");
    CHECK(print(conflict) == before && conflict.revision == 0);

    // Each module specializes its own aliases before comparing signatures.
    // A dependency's dialect parameters and extern declarations must survive
    // the intermediate library, not just its function definitions.
    library("child", R"(module child level high {
      types {T=i32;}
      dialect math {factor=7;}
      extern func @host() -> param<T>;
      func @leaf(%x:param<T>) -> param<T> {
        ^e: %r:i32=math.scale(%x); func.return(%r);
      }
      func @external() -> i32 {
        ^e: %r:i32=func.call() attrs {callee="host";}; func.return(%r);
      }
    })");
    library("parent", R"(module parent level high {
      import "child";
      types {T=i64; U=i32;}
      extern func @leaf(%x:param<U>) -> param<U>;
    })");
    auto module = parse(R"(module app level high {
      import "parent";
      types {T=i32;}
      extern func @leaf(%x:param<T>) -> param<T>;
      func @host() -> i32 { ^e: func.return(42:i32); }
    })");
    resolve_imports(module, r, {dir.string()});
    CHECK(module.imports.empty() && module.revision == 1);
    CHECK(run(module, r, "leaf", {std::int64_t(6)}) == 42);
    CHECK(run(module, r, "external") == 42);
    CHECK(module.function("leaf").parameters[0].type == Type::integer(32));
    CHECK(std::get<std::int64_t>(module.dialects.at("math").at("factor")) == 7);

    // A dependency's required feature travels up to the host, which must
    // declare it. An intermediate library need not declare it itself.
    library("child", R"(module child level core { target "vm" { feature.gpu = true; } })");
    library("parent", R"(module parent level core { import "child"; })");
    auto featured = parse(R"(module app level core {
      target "vm" { feature.gpu = true; }
      import "parent";
    })");
    resolve_imports(featured, r, {dir.string()});
    CHECK(featured.target.features.count("gpu") == 1);
    auto missing = parse(R"(module app level core { import "parent"; })");
    const auto missing_before = print(missing);
    rejects([&] { resolve_imports(missing, r, {dir.string()}); }, "import.feature");
    CHECK(print(missing) == missing_before && missing.revision == 0);
}
void extensions_and_transactions() {
    Registry r;
    DialectDefinition toy; toy.name = "toy";
    toy.parameters = {{"amount", std::int64_t(7)}, {"element", Type::integer()}};
    toy.verify_type = [](const Type& t, const Attributes& a, const Target&) {
        return t.name == "toy.box" && t.elements.size() == 1 && t.elements[0] == std::get<Type>(a.at("element"));
    };
    OperationDefinition offset; offset.name = "toy.offset"; offset.min_operands = offset.max_operands = offset.min_results = offset.max_results = 1;
    offset.effect = Effect::Pure;
    offset.verify = [](const Operation& op, const OperationContext& c) {
        if (c.type(op.operands[0]) != std::get<Type>(c.dialect_parameters.at("element")) || op.results[0].type != c.type(op.operands[0])) throw Error("toy.type", "type mismatch");
    };
    toy.operations.push_back(offset); r.add_dialect(toy);
    auto module = parse(R"(module extension level high {types {T=i16;}
      dialect toy {element=type<param<T>>;amount=12;}
      func @f(%x:param<T>)->param<T> {^e:%r:param<T>=toy.offset(%x);func.return(%r);}
    })");
    require_valid(module, r); const auto before = print(module);
    rejects([&] { lower(module, r, Level::Core); }, "lowering.missing"); CHECK(print(module) == before);
    r.add_lowering({"offset", "toy.offset", Level::Core, [](const Operation& op, const Function&, const Module& m, const Registry& r) {
        auto result = op; result.name = "arith.add";
        result.operands.push_back(Operand::constant(std::get<std::int64_t>(r.parameters(m, "toy").at("amount")), op.results[0].type));
        return std::vector<Operation>{result};
    }});
    rejects([&] { lower(module, r, Level::Core, 0); }, "budget"); CHECK(print(module) == before);
    lower(module, r, Level::Core); CHECK(run(module, r, "f", {std::int64_t(30)}) == 42);
    auto opaque = parse("module m level high {extern func @box(%v:opaque<toy.box,i32>)->opaque<toy.box,i32>;}");
    require_valid(opaque, r);
    opaque.functions[0].parameters[0].type.elements[0] = Type::integer(64);
    rejects([&] { require_valid(opaque, r); }, "type.opaque");
    auto state = example("affine.flint"); lower(state, r, Level::Core); auto snapshot = print(state); auto revision = state.revision;
    PassManager failing;
    failing.add({"rename", {}, {}, [](Module& m, const Registry&, AnalysisManager&) { m.name = "renamed"; return true; }});
    failing.add({"invalid", {}, {}, [](Module& m, const Registry&, AnalysisManager&) { m.functions[0].result = Type::integer(8); return true; }});
    rejects([&] { failing.run(state, r); }, "pass.invalid"); CHECK(print(state) == snapshot && state.revision == revision);
    PassManager lying; lying.add({"liar", {}, {}, [](Module& m, const Registry&, AnalysisManager&) { m.name = "changed"; return false; }});
    rejects([&] { lying.run(state, r); }, "changed flag"); CHECK(print(state) == snapshot);
    PassManager unavailable; unavailable.add({"needs", {"unregistered"}, {}, [](Module&, const Registry&, AnalysisManager&) { return false; }});
    rejects([&] { unavailable.run(state, r); }, "unknown analysis");
    AnalysisManager a; auto& uses = std::any_cast<const DefUse&>(a.get("def_use", state, r)); CHECK(!uses.uses.empty());
    a.get("cfg", state, r); a.get("dominators", state, r); a.get("loops", state, r); a.invalidate({"cfg"});
    CHECK(layout(Type::tuple({Type::integer(8), Type::integer(64)}), {}).size == 16);
    CHECK(layout(Type::vector(4, Type::integer(32)), {}).size == 16);
    Target index_target; index_target.index_bits = 32;
    CHECK(layout(Type::index(), index_target).size == 4);
    rejects([&] { layout(Type::opaque("toy.box"), {}); }, "explicit target layout");
    std::size_t visited = 0; frontend::visit(state, [&](const auto&, const auto&, const auto&) { ++visited; }); CHECK(visited == 3);
    Registry independent; rejects([&] { require_valid(opaque, independent); }, "unregistered dialect");
    PassManager custom;
    custom.add_analysis("count", [](const Module& m, const Registry&) -> std::any { return m.functions.size(); });
    custom.add({"read-count", {"count"}, {}, [](Module& m, const Registry& r, AnalysisManager& a) {
        CHECK(std::any_cast<std::size_t>(a.get("count", m, r)) == m.functions.size()); return false;
    }});
    CHECK(!custom.run(state, r)[0].changed);
    rejects([&] { custom.add_analysis("count", [](const auto&, const auto&) -> std::any { return 0; }); }, "duplicate");
    DialectDefinition pair; pair.name = "pair";
    OperationDefinition dup; dup.name = "pair.dup"; dup.min_operands = dup.max_operands = 1; dup.min_results = dup.max_results = 2;
    dup.effect = Effect::Pure;
    dup.verify = [](const Operation& op, const OperationContext& context) {
        for (const auto& result : op.results) CHECK(result.type == context.type(op.operands[0]));
    };
    pair.operations.push_back(dup); r.add_dialect(pair);
    r.add_lowering({"pair-copy", "pair.dup", Level::Core, [](const Operation& source, const auto&, const auto&, const auto&) {
        Operation a = source, b = source; a.name = b.name = "arith.copy";
        a.results = {source.results[0]}; b.results = {source.results[1]}; return std::vector<Operation>{a,b};
    }});
    auto pairs = parse("module m level high {func @f(%x:i32)->i32 {^e:%a:i32,%b:i32=pair.dup(%x);%r:i32=arith.add(%a,%b);func.return(%r);}}");
    require_valid(pairs, r);
    rejects([&] { to_ssa(pairs, pairs.function("f"), r); }, "multi-result");
    lower(pairs, r, Level::Core); CHECK(run(pairs, r, "f", {std::int64_t(21)}) == 42);
    auto other_function = pairs.function("f");
    rejects([&] { to_ssa(pairs, other_function, r); }, "owned");
    Function built; built.name = "built"; built.result = Type::integer(); Builder builder(built);
    auto p = builder.parameter(Type::integer()); auto entry = builder.block("entry");
    auto value = builder.emit(entry, "arith.add", Type::integer(), {Operand::ref(p.id), Operand::constant(12)});
    builder.return_value(entry, Operand::ref(value.id));
    Module program; program.name = "builder"; program.level = Level::Core; program.functions.push_back(std::move(built));
    CHECK(run(program, r, "built", {std::int64_t(30)}) == 42);
}
}
int main() {
    try { workflows(); diagnostics(); calls_and_memory(); modular_and_optimization(); aggregate_dialects(); stdlib_and_imports(); transitive_import_contracts(); extensions_and_transactions(); std::cout << "Flintstone core contracts passed\n"; }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
