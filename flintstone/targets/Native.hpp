#pragma once
#include "../Flintstone.hpp"
#include <sandstone.hpp>
#include <schedrow/schedrow.hpp>
#include <regtl/regtl.hpp>

namespace flintstone {
// This is an explicit integration boundary: existing Stone IRs, not a second
// instruction selector, scheduler, allocator, object format or ISA database.
struct FormBinding {
    enum class Kind { Value, PhysicalRegister, Literal };
    std::string name;
    std::uint32_t position = domsymas::kNoId; // Extra encoder metadata has no position.
    Kind kind = Kind::Literal;
    std::uint32_t id = 0;
    domsymas::encoding::Value literal;
};
struct FormEncoding { std::uint32_t form; std::vector<FormBinding> bindings; };
struct SelectedFunction {
    limestone::schedrow::Region scheduling;
    limestone::regtl::Function registers;
    std::map<std::uint32_t, FormEncoding> encodings; // Scheduling instruction ID.
};
enum class Allocator { LinearScan, Greedy, Coloring, PBQP };
struct NativeTarget {
    std::shared_ptr<const domsymas::Databank> bank;
    std::uint32_t target_id = 0;
    limestone::schedrow::MachineModel machine;
    Allocator allocator = Allocator::Coloring;
    std::function<SelectedFunction(const Module&, const Function&, const Registry&)> select;
    // Required for nonempty target-specific constraints and non-scalar operands.
    std::function<bool(const domsymas::OperandRec&, const domsymas::encoding::Value&)> prove_operand;
};
struct NativeFunctionReport {
    std::string name;
    limestone::schedrow::Region selected;
    std::map<std::uint32_t, FormEncoding> encodings;
    std::vector<limestone::schedrow::Scheduled> schedule;
    limestone::regtl::Program allocation_problem;
    limestone::regtl::Allocation allocation;
};
struct NativeCompilation { Artifact artifact; std::vector<NativeFunctionReport> functions; };
// Index width is a compiler policy and is deliberately not inferred from an ISA.
Target sandstone_target(const domsymas::Databank&, std::uint32_t target_id, unsigned index_bits);
limestone::schedrow::MachineModel sandstone_machine(const domsymas::Databank&, std::uint32_t model);
// Imports only timing actually present in the bank; missing timing is an error.
limestone::schedrow::Instruction sandstone_instruction(const domsymas::Databank&,
    std::uint32_t form, std::uint32_t model, std::uint32_t instruction_id);
NativeCompilation compile_native(const Module&, const Registry&, const NativeTarget&);
void register_native_backend(Registry&, std::string name, NativeTarget);
} // namespace flintstone
