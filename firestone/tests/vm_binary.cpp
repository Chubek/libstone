#include "VMTest.hpp"
#include <cmath>

int main() { return test_main([] {
    auto module = adder();
    module.constants.add(std::monostate{}); module.constants.add(true); module.constants.add(INT64_MIN);
    module.constants.add(INT64_MAX); module.constants.add(std::string("a\0b\"c", 6));
    module.constants.add(0.0); module.constants.add(-0.0); module.constants.add(std::numeric_limits<double>::quiet_NaN());
    auto bytes = vm::encode(module); auto decoded = vm::decode(bytes); CHECK(vm::encode(decoded) == bytes);
    CHECK(vm::disassemble(module).find("%2 = Add %0 %1 bits=64") != std::string::npos);
    CHECK(std::signbit(vm::require<double>(decoded.constants.at(6))));
    vm::Environment env; CHECK(integer_result(vm::execute(decoded, 0, {std::int64_t{20}, std::int64_t{22}}, env)) == 42);
    for (vm::Index n = 0; n < bytes.size(); ++n) {
        vm::BytecodeBytes truncated(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(n));
        throws<std::invalid_argument>([&] { vm::decode(truncated); });
    }
    auto bad = bytes; bad[0] = 0; throws<std::invalid_argument>([&] { vm::decode(bad); });
    bad = bytes; bad[4] = 2; throws<std::invalid_argument>([&] { vm::decode(bad); });
    bad = bytes; bad.push_back(0); throws<std::invalid_argument>([&] { vm::decode(bad); });
    bad = bytes; for (vm::Index n = 12; n < 20; ++n) bad[n] = 255;
    throws<std::invalid_argument>([&] { vm::decode(bad); });
    throws<std::invalid_argument>([&] { vm::decode(bytes, {bytes.size() - 1, 1000000, 1024}); });
    throws<std::invalid_argument>([&] { vm::decode(bytes, {bytes.size(), 2, 1024}); });
    throws<std::invalid_argument>([&] { vm::decode(bytes, {bytes.size(), 1000000, 1}); });
    throws<std::invalid_argument>([&] { module.constants.add(vm::Address{0}); });

    const vm::BigInt large("1267650600228229401496703205376");
    const vm::Value compact = vm::BigInt(INT64_MIN);
    const vm::Value explicit_wide(std::in_place_type<vm::BigInt>, INT64_MIN);
    const vm::Value wide_unsigned = UINT64_MAX;
    CHECK(vm::representation(compact) == vm::ValueRepresentation::Int64);
    CHECK(vm::representation(explicit_wide) == vm::ValueRepresentation::BigInt);
    CHECK(vm::kind(compact) == vm::ValueKind::Integer && vm::kind(explicit_wide) == vm::kind(compact));
    CHECK(compact == explicit_wide && explicit_wide == compact);
    CHECK(vm::require_integer64(explicit_wide) == INT64_MIN);
    CHECK(vm::representation(vm::BigInt(INT64_MAX)) == vm::ValueRepresentation::Int64);
    CHECK(vm::representation(vm::BigInt(INT64_MAX) + vm::BigInt(1)) == vm::ValueRepresentation::BigInt);
    CHECK(vm::representation(vm::BigInt(INT64_MIN) - vm::BigInt(1)) == vm::ValueRepresentation::BigInt);
    CHECK(vm::require_integer(wide_unsigned).to_string() == "18446744073709551615");
    vm_error("vm.overflow", [&] { vm::require_integer64(wide_unsigned); });
    vm_error("vm.type", [] { vm::require_integer(true); });
    vm::check_arguments({{vm::ValueKind::Integer}, vm::ValueKind::Integer}, {large});
    vm::check_result({{}, vm::ValueKind::Integer}, -large);

    auto wide_module = adder();
    CHECK(wide_module.constants.add(compact) == wide_module.constants.add(explicit_wide));
    CHECK(wide_module.constants.add(large) == 1);
    CHECK(wide_module.constants.add(-large) == 2);
    CHECK(wide_module.constants.add(large) == 1);
    auto wide_bytes = vm::encode(wide_module);
    CHECK(bytes[4] == 1 && wide_bytes[4] == 2);
    auto wide_decoded = vm::decode(wide_bytes);
    CHECK(vm::encode(wide_decoded) == wide_bytes);
    CHECK(vm::require_integer(wide_decoded.constants.at(1)) == large);
    CHECK(vm::require_integer(wide_decoded.constants.at(2)) == -large);
    CHECK(vm::format_value(wide_decoded.constants.at(2)) == "-1267650600228229401496703205376");
    bad = wide_bytes; bad[4] = 1; throws<std::invalid_argument>([&] { vm::decode(bad); });
    const auto payload_start = 20 + 9 + 1 + 8; // header, compact constant, wide tag and length
    bad = wide_bytes; bad[payload_start] = '0'; throws<std::invalid_argument>([&] { vm::decode(bad); });
    bad = wide_bytes; bad[payload_start] = 'G'; throws<std::invalid_argument>([&] { vm::decode(bad); });
    throws<std::invalid_argument>([&] { vm::decode(wide_bytes, {wide_bytes.size(), 1000000, 25}); });

    auto dynamic_module = adder();
    dynamic_module.function(0).code.at(0).bits = vm::dynamic_scalar_width;
    auto dynamic_bytes = vm::encode(dynamic_module);
    CHECK(dynamic_bytes[4] == 2);
    auto dynamic_decoded = vm::decode(dynamic_bytes);
    CHECK(vm::encode(dynamic_decoded) == dynamic_bytes);
    auto dynamic_result = vm::execute(dynamic_decoded, 0, {large, 1}, env);
    CHECK(dynamic_result.stop == vm::ExecutionStop::Returned && dynamic_result.state.result.has_value());
    CHECK(vm::require_integer(*dynamic_result.state.result) == large + vm::BigInt(1));
    bad = dynamic_bytes; bad[4] = 1; throws<std::invalid_argument>([&] { vm::decode(bad); });

    auto invalid = module; invalid.function(0).name.clear();
    CHECK(has_code(invalid.verify(), "vm.name")); throws<std::invalid_argument>([&] { vm::encode(invalid); });

    vm::BytecodeCache cache(1); auto first = cache.load(bytes); CHECK(cache.load(bytes) == first);
    CHECK(cache.hits() == 1 && cache.misses() == 1);
    throws<std::invalid_argument>([&] { cache.load(bytes, {1, 1000000, 1024}); });
    CHECK(cache.size() == 1);
    auto other = vm::encode(adder(vm::VMModel::Stack)); auto second = cache.load(other); CHECK(cache.size() == 1);
    CHECK(cache.load(bytes) != first); CHECK(first->size() == 1 && second->size() == 1);
    cache.set_capacity(2);
    auto recent = cache.load(bytes); auto older = cache.load(other);
    CHECK(cache.load(bytes) == recent && cache.size() == 2);
    cache.set_capacity(1);
    CHECK(cache.capacity() == 1 && cache.size() == 1 && cache.load(bytes) == recent);
    CHECK(cache.load(other) != older);
    cache.set_capacity(0);
    CHECK(cache.size() == 0 && cache.load(bytes) != cache.load(bytes));
    vm::Loader loader; auto loaded = loader.load("math", bytes);
    CHECK(loaded == loader.get("math")); loader.unload("math");
    vm_error("vm.module", [&] { loader.get("math"); });
    module.natives["external"] = {{vm::ValueKind::Integer}, vm::ValueKind::Integer};
    vm_error("vm.native", [&] { loader.link(module, env); });
    env.bind("external", {{}, vm::ValueKind::Null}, [](const std::vector<vm::Value>&) { return vm::Value{}; });
    vm_error("vm.signature", [&] { loader.link(module, env); });
    env.bind("external", module.natives.at("external"), [](const std::vector<vm::Value>& args) { return args[0]; });
    loader.link(module, env);
}); }
