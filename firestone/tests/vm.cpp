#include "VMTest.hpp"

int main() { return test_main([] {
    vm::Environment env;
    for (auto model : {vm::VMModel::Register, vm::VMModel::Stack}) {
        auto module = adder(model); CHECK(module.verify().empty());
        auto result = vm::execute(module, 0, {std::int64_t{19}, std::int64_t{23}}, env, {1, 16, 16});
        CHECK(result.stop == vm::ExecutionStop::Budget);
        result = vm::resume(module, env, result.state); CHECK(integer_result(result) == 42);
        vm_error("vm.type", [&] { vm::execute(module, 0, {true, std::int64_t{2}}, env); });
        vm_error("vm.overflow", [&] { vm::execute(module, 0, {INT64_MAX, std::int64_t{1}}, env); });
        auto malformed = module; malformed.function(0).code.at(0).opcode = vm::Opcode::Count;
        CHECK(has_code(malformed.verify(), "vm.opcode"));
        malformed = module;
        if (model == vm::VMModel::Register) malformed.function(0).code.at(0).operands[0] = 2;
        else malformed.function(0).code.at(0).index = 2;
        CHECK(!malformed.verify().empty());
    }
    CHECK(vm::require<std::int64_t>(vm::scalar(vm::Opcode::Not, {std::int64_t{0}}, 8)) == -1);
    CHECK(vm::require<std::int64_t>(vm::scalar(vm::Opcode::Shr, {std::int64_t{-5}, std::int64_t{1}}, 8)) == -3);
    CHECK(vm::require<bool>(vm::scalar(vm::Opcode::Xor, {true, false}, 1)));
    CHECK(vm::require<double>(vm::scalar(vm::Opcode::Div, {9.0, 2.0})) == 4.5);
    vm_error("vm.divzero", [] { vm::scalar(vm::Opcode::Div, {std::int64_t{4}, std::int64_t{0}}); });
    vm_error("vm.overflow", [] { vm::scalar(vm::Opcode::Div, {INT64_MIN, std::int64_t{-1}}); });
    vm_error("vm.shift", [] { vm::scalar(vm::Opcode::Shl, {std::int64_t{1}, std::int64_t{64}}); });
    CHECK(vm::scalar(vm::Opcode::Add, {"fire", "stone"}) == vm::Value{"firestone"});
    CHECK(vm::scalar(vm::Opcode::Lt, {"a", "b"}) == vm::Value{true});
    CHECK(vm::scalar(vm::Opcode::Eq, {vm::Value{}, vm::Value{}}) == vm::Value{true});
    CHECK(vm::scalar(vm::Opcode::Ne, {vm::Address{1}, vm::Address{2}}) == vm::Value{true});
    vm_error("vm.type", [] { vm::scalar(vm::Opcode::Sub, {"fire", "stone"}); });
    vm_error("vm.type", [] { vm::scalar(vm::Opcode::Add, {true, std::int64_t{1}}); });
    vm_error("vm.arity", [] { vm::scalar(vm::Opcode::Neg, {}); });
    vm_error("vm.width", [] { vm::scalar(vm::Opcode::Neg, {std::int64_t{1}}, 65); });

    constexpr auto dynamic = vm::dynamic_scalar_width;
    const vm::BigInt beyond64{"9223372036854775808"};
    const vm::Value wide{beyond64};
    CHECK(vm::representation(wide) == vm::ValueRepresentation::BigInt);
    CHECK(vm::scalar(vm::Opcode::Sub, {wide, std::int64_t{1}}, dynamic) == vm::Value{INT64_MAX});
    CHECK(vm::representation(vm::scalar(vm::Opcode::Sub, {wide, std::int64_t{1}}, dynamic)) ==
          vm::ValueRepresentation::Int64);
    CHECK(vm::scalar(vm::Opcode::Gt, {wide, std::int64_t{INT64_MAX}}, dynamic) == vm::Value{true});
    CHECK(vm::scalar(vm::Opcode::Or, {wide, std::int64_t{1}}, dynamic) == vm::Value{beyond64 + vm::BigInt{1}});
    CHECK(vm::scalar(vm::Opcode::And, {wide, std::int64_t{-1}}, dynamic) == wide);
    CHECK(vm::scalar(vm::Opcode::Xor, {wide, std::int64_t{-1}}, dynamic) ==
          vm::scalar(vm::Opcode::Not, {wide}, dynamic));
    for (auto code : {vm::Opcode::And, vm::Opcode::Or, vm::Opcode::Xor})
        for (std::int64_t x : {std::int64_t{INT64_MIN}, std::int64_t{-37}, std::int64_t{89}})
            for (std::int64_t y : {std::int64_t{-18}, std::int64_t{0}, std::int64_t{INT64_MAX}})
                CHECK(vm::scalar(code, {x, y}, dynamic) == vm::scalar(code, {x, y}));
    CHECK(vm::scalar(vm::Opcode::Shl, {std::int64_t{1}, std::int64_t{63}}, dynamic) == wide);
    CHECK(vm::scalar(vm::Opcode::Shr, {-beyond64 - vm::BigInt{1}, std::int64_t{63}}, dynamic) == vm::Value{-2});
    CHECK(vm::scalar(vm::Opcode::Shr, {-beyond64, wide}, dynamic) == vm::Value{-1});
    CHECK(vm::scalar(vm::Opcode::Shr, {std::int64_t{1}, beyond64 * beyond64}, dynamic) == vm::Value{0});
    CHECK(vm::scalar(vm::Opcode::Div, {std::int64_t{-5}, std::int64_t{2}}, dynamic) == vm::Value{-3});
    CHECK(vm::scalar(vm::Opcode::Mod, {std::int64_t{-5}, std::int64_t{2}}, dynamic) == vm::Value{1});
    vm_error("vm.divzero", [&] { vm::scalar(vm::Opcode::Mod, {wide, std::int64_t{0}}, dynamic); });
    vm_error("vm.shift", [&] { vm::scalar(vm::Opcode::Shl, {std::int64_t{1}, wide}, dynamic); });
    vm_error("vm.overflow", [&] { vm::scalar(vm::Opcode::Neg, {wide}); });
    vm_error("vm.shift", [&] { vm::scalar(vm::Opcode::Shl, {std::int64_t{1}, wide}); });
    for (auto model : {vm::VMModel::Register, vm::VMModel::Stack}) {
        auto module = adder(model);
        module.function(0).code.at(model == vm::VMModel::Register ? 0 : 2).bits = dynamic;
        CHECK(module.verify().empty());
        auto loaded = vm::decode(vm::encode(module));
        auto result = vm::execute(loaded, 0, {std::int64_t{INT64_MAX}, std::int64_t{1}}, env);
        CHECK(result.state.complete() && *result.state.result == wide);
        result = vm::execute(loaded, 0, {wide, std::int64_t{-1}}, env);
        CHECK(integer_result(result) == INT64_MAX);
    }

    // The same opcode and signature can select different methods on each receiver.
    const auto first = env.create_object({"First", {}, {{"Add", "first-add"}, {"Eq", "first-eq"}, {"Neg", "first-neg"}}}, {});
    const auto second = env.create_object({"Second", {}, {{"Add", "second-add"}}}, {});
    const auto no_add = env.create_object({"NoAdd", {}, {}}, {});
    const vm::Signature add_sig{{vm::ValueKind::Object, vm::ValueKind::Integer}, vm::ValueKind::Integer};
    env.bind("first-add", add_sig, [&](const std::vector<vm::Value>& args) -> vm::Value {
        CHECK(vm::require<vm::ObjectRef>(args[0]) == first);
        return vm::require<std::int64_t>(args[1]) + 1;
    });
    env.bind("second-add", add_sig, [&](const std::vector<vm::Value>& args) -> vm::Value {
        CHECK(vm::require<vm::ObjectRef>(args[0]) == second);
        return vm::require<std::int64_t>(args[1]) + 2;
    });
    for (auto model : {vm::VMModel::Register, vm::VMModel::Stack}) {
        auto module = adder(model);
        module.function(0).signature = add_sig;
        CHECK(integer_result(vm::execute(module, 0, {first, std::int64_t{40}}, env)) == 41);
        CHECK(integer_result(vm::execute(module, 0, {second, std::int64_t{40}}, env)) == 42);
        bool caught = false;
        try { vm::execute(module, 0, {no_add, std::int64_t{40}}, env); }
        catch (const vm::VMError& error) {
            CHECK(error.code == "vm.type");
            CHECK((error.position == vm::CodeAddress{0, model == vm::VMModel::Register ? 0u : 2u}));
            caught = true;
        }
        CHECK(caught);
    }
    auto equal_sig = vm::Signature{{vm::ValueKind::Object, vm::ValueKind::Object}, vm::ValueKind::Boolean};
    env.bind("first-eq", equal_sig, [](const auto&) { return vm::Value{true}; });
    CHECK(vm::scalar(vm::Opcode::Eq, {first, second}, env) == vm::Value{true});
    CHECK(vm::scalar(vm::Opcode::Eq, {second, first}, env) == vm::Value{false});
    CHECK(vm::scalar(vm::Opcode::Eq, {first, second}) == vm::Value{false});
    env.bind("first-neg", {{vm::ValueKind::Object}, vm::ValueKind::Integer}, [](const auto&) { return vm::Value{-1}; });
    CHECK(vm::scalar(vm::Opcode::Neg, {first}, env) == vm::Value{-1});
    equal_sig.result = vm::ValueKind::Integer;
    env.bind("first-eq", equal_sig, [](const auto&) -> vm::Value { CHECK(false); return std::int64_t{0}; });
    vm_error("vm.type", [&] { vm::scalar(vm::Opcode::Eq, {first, second}, env); });
    vm_error("vm.type", [&] { vm::scalar(vm::Opcode::Add, {first, false}, env); });

    // Mixed-model call frames preserve left-to-right argument order and return destinations.
    auto mixed = adder(vm::VMModel::Stack);
    vm::Function caller; caller.name = "caller"; caller.register_count = 3; caller.signature = mixed.function(0).signature;
    auto call = op(vm::Opcode::Call, {0, 1}, 2); call.index = 0; call.argument_count = 2;
    caller.code.emit(call); caller.code.emit(ret({2})); mixed.add_function(caller);
    CHECK(integer_result(vm::RegisterVM{}.run(mixed, 1, {std::int64_t{7}, std::int64_t{35}}, env)) == 42);
    vm_error("vm.call.limit", [&] { vm::execute(mixed, 1, {std::int64_t{7}, std::int64_t{35}}, env, {100, 1, 16}); });

    auto address = env.allocate(std::int64_t{1}); env.store(address, std::int64_t{42});
    CHECK(vm::require<std::int64_t>(env.load(address)) == 42);
    env.release(address); vm_error("vm.address", [&] { env.load(address); });
    CHECK(env.allocate().slot != address.slot);
    vm::MetaObject meta{"Counter", {{"count", vm::ValueKind::Integer}}, {{"read", "read-count"}}};
    auto object = env.create_object(meta, {{"count", std::int64_t{3}}}); env.set_field(object, "count", std::int64_t{42});
    env.bind("read-count", {{vm::ValueKind::Object}, vm::ValueKind::Integer}, [&](const std::vector<vm::Value>& args) { return env.field(vm::require<vm::ObjectRef>(args[0]), "count"); });
    CHECK(vm::require<std::int64_t>(env.invoke(object, "read", {})) == 42);
    vm_error("vm.type", [&] { env.set_field(object, "count", true); });

    // Field/global bytecodes and a void native result do not leak operands.
    for (auto model : {vm::VMModel::Register, vm::VMModel::Stack}) {
        vm::Module fields; vm::Function fn; fn.name = "field"; fn.model = model;
        fn.signature = {{vm::ValueKind::Object}, vm::ValueKind::Integer};
        fn.register_count = model == vm::VMModel::Register ? 2 : 0; fn.local_count = model == vm::VMModel::Stack ? 1 : 0;
        fields.natives["observe"] = {{vm::ValueKind::Integer}, vm::ValueKind::Null}; int observations = 0;
        env.bind("observe", fields.natives.at("observe"), [&](const std::vector<vm::Value>& args) { CHECK(vm::require<std::int64_t>(args[0]) == 42); ++observations; return vm::Value{}; });
        if (model == vm::VMModel::Stack) { auto get = op(vm::Opcode::LocalGet); get.index = 0; fn.code.emit(get); }
        auto get = op(vm::Opcode::GetField, model == vm::VMModel::Register ? std::vector<vm::Index>{0} : std::vector<vm::Index>{}, model == vm::VMModel::Register ? 1 : vm::no_index);
        get.symbol = "count"; fn.code.emit(get);
        auto set = op(vm::Opcode::SetGlobal, model == vm::VMModel::Register ? std::vector<vm::Index>{1} : std::vector<vm::Index>{}); set.symbol = "answer"; fn.code.emit(set);
        get = op(vm::Opcode::GetGlobal, {}, model == vm::VMModel::Register ? 1 : vm::no_index); get.symbol = "answer"; fn.code.emit(get);
        auto observe = op(vm::Opcode::NativeCall, model == vm::VMModel::Register ? std::vector<vm::Index>{1} : std::vector<vm::Index>{}); observe.symbol = "observe"; observe.argument_count = 1; fn.code.emit(observe);
        fn.code.emit(get); fn.code.emit(ret(model == vm::VMModel::Register ? std::vector<vm::Index>{1} : std::vector<vm::Index>{})); fields.add_function(fn);
        CHECK(integer_result(vm::execute(fields, 0, {object}, env)) == 42 && observations == 1);
        env.bind("observe", fields.natives.at("observe"), [](const std::vector<vm::Value>&) { return vm::Value{true}; });
        vm_error("vm.type", [&] { vm::execute(fields, 0, {object}, env); });
    }

    // A nonterminating program exits only at the instruction budget, at a valid PC.
    vm::Module infinite; vm::Function forever; forever.name = "forever";
    auto backedge = op(vm::Opcode::Jump); backedge.target = 0; forever.code.emit(backedge); infinite.add_function(forever);
    auto budget_exit = vm::execute(infinite, 0, {}, env, {7, 1, 1});
    CHECK(budget_exit.stop == vm::ExecutionStop::Budget && budget_exit.state.instructions == 7 && budget_exit.state.frames.top().pc == 0);

    // Join verification checks stack heights and path-sensitive register availability.
    vm::Module bad; vm::Function join; join.name = "join"; join.register_count = 2;
    join.signature = {{vm::ValueKind::Boolean}, vm::ValueKind::Boolean};
    auto branch = op(vm::Opcode::Branch, {0}); branch.target = 1; branch.alternate = 2; join.code.emit(branch);
    auto move = op(vm::Opcode::Move, {0}, 1); join.code.emit(move); join.code.emit(ret({1})); bad.add_function(join);
    CHECK(has_code(bad.verify(), "vm.uninitialized"));
    bad = {}; join = {}; join.name = "stack-join"; join.model = vm::VMModel::Stack; join.signature.result = vm::ValueKind::Boolean;
    auto constant = op(vm::Opcode::Constant); constant.index = bad.constants.add(true); join.code.emit(constant);
    branch = op(vm::Opcode::Branch); branch.target = 2; branch.alternate = 3; join.code.emit(branch);
    join.code.emit(constant); join.code.emit(ret()); bad.add_function(join);
    CHECK(has_code(bad.verify(), "vm.stack.join") || has_code(bad.verify(), "vm.stack.underflow"));

    // Portable dispatch forms share handlers, budgets, and explicit control transfer.
    vm::CodeTape tape; tape.emit(vm::Opcode::Nop); tape.emit(vm::Opcode::Return);
    vm::HandlerTable handlers; int visits = 0;
    handlers[static_cast<vm::Index>(vm::Opcode::Nop)] = [&](const vm::Instruction&, vm::Index pc) { ++visits; return pc + 1; };
    handlers[static_cast<vm::Index>(vm::Opcode::Return)] = [](const vm::Instruction&, vm::Index) { return vm::no_index; };
    vm::DirectThreadedCode direct(tape, handlers); vm::TableThreadedCode table(tape, handlers);
    vm::SubroutineThgreadedCode subroutine(tape, handlers);
    auto shared = std::make_shared<vm::HandlerTable>(handlers); vm::IndirectThreadedCode indirect(tape, shared);
    CHECK(direct.run(0, 1).next == 1); CHECK(table.run().complete); CHECK(subroutine.run().complete); CHECK(indirect.run().complete);
    CHECK(visits == 4);
    (*shared)[static_cast<vm::Index>(vm::Opcode::Nop)] = [](const vm::Instruction&, vm::Index) { return vm::no_index; };
    CHECK(indirect.run().instructions == 1);
    vm_error("vm.pc", [&] { direct.run(99); });
}); }
