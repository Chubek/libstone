#pragma once

#include "novostone/PCC/RegisterAllocation.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace novostone::pcc {

inline constexpr std::string_view native_target = "x86_64-linux-gnu (SysV LP64)";

enum class NativeRegister { Rax, Rcx, Rdx, Rbx, Rbp, Rsi, Rdi,
                            R8, R9, R10, R11, R12, R13, R14, R15 };

// All stack offsets are bytes above the post-prologue RSP. Each slot reserves
// eight bytes; spilled values and incoming arguments use only the low four.
struct NativeHome {
  enum class Kind { Register, Spill };
  Kind kind = Kind::Register;
  NativeRegister reg = NativeRegister::Rax;
  std::uint32_t offset = 0;
  bool operator==(const NativeHome&) const = default;
};

struct NativeFrame {
  std::uint32_t size = 0;
  std::map<std::uint32_t, std::uint32_t> arguments; // argument index -> offset
  std::map<NativeRegister, std::uint32_t> saved;    // callee-save register -> offset
  bool operator==(const NativeFrame&) const = default;
};

// A closed subset of x86-64 instructions, not raw machine code or text.
// Operands are dst, src (AT&T rendering reverses their order). Compare32
// sets flags from dst - src. The integer field is an immediate, byte offset
// or stack adjustment, depending on opcode. Unused fields must be defaulted.
enum class NativeOp {
  SubSp, AddSp, Mov32, Imm32, Load32, Store32, Save64, Restore64,
  Add32, Sub32, Mul32, Compare32, CompareZero32, SetEqual8, SetBelow8,
  MoveNonzero32, CallIdentity, Ret
};

struct NativeInstruction {
  NativeOp op = NativeOp::Ret;
  NativeRegister dst = NativeRegister::Rax;
  NativeRegister src = NativeRegister::Rax;
  std::uint32_t integer = 0;
  bool operator==(const NativeInstruction&) const = default;
};

struct NativeSpan {
  ir::Id instruction = 0;
  std::vector<NativeInstruction> code;
  bool operator==(const NativeSpan&) const = default;
};

struct NativeProgram {
  NativeFrame frame;
  std::map<ir::Id, NativeHome> homes;
  std::vector<NativeInstruction> prologue;
  std::vector<NativeSpan> body;
  std::vector<NativeInstruction> epilogue;
  std::vector<NativeInstruction> identity_helper;
  bool operator==(const NativeProgram&) const = default;
};

struct NativeRequest {
  std::string symbol = "novostone_pcc_entry";
  // The IR has no Call. An explicitly selected Copy may be implemented by
  // a direct call to the unit-local, independently checked i32 identity helper.
  std::set<ir::Id> identity_calls;
};

struct NativeLimits {
  std::size_t max_ir_instructions = 256;
  std::size_t max_native_instructions = 4096;
  std::size_t max_smt_bytes = 512 * 1024;
};

struct NativeLoweringResult {
  // ObligationReady here means a candidate was generated, not verified.
  EvidenceStatus status = EvidenceStatus::Unknown;
  std::string diagnostic;
  std::optional<NativeProgram> program;
};

enum class NativeStatus { Rejected, Unknown, Proved };

struct NativeCheckResult {
  NativeStatus status = NativeStatus::Unknown;
  // All structural checks passed, and the checker derived the symbolic
  // obligations for these exact assembly bytes. This alone does not prove
  // those obligations: an absent or inconclusive Satie still yields Unknown.
  bool binding_checked = false;
  std::string diagnostic;
  std::string smtlib2;
  std::string assembly;
};

[[nodiscard]] NativeLoweringResult lower_native(
    const ir::Function& function, const NativeRequest& request = {},
    NativeLimits limits = {});

// Checks the candidate against the original IR and request, without replaying
// the lowering allocator or consuming submitted SMT. Only a checked binding
// and UNSAT via the PCC SMT-LIB2/Satie conduit produce Proved.
[[nodiscard]] NativeCheckResult verify_native(
    const ir::Function& function, const NativeProgram& candidate,
    const NativeRequest& request = {}, NativeLimits limits = {});

// Same check, additionally requiring the supplied assembly bytes to match
// the canonical rendering verbatim. Does not assemble or examine object code.
[[nodiscard]] NativeCheckResult verify_native_assembly(
    const ir::Function& function, const NativeProgram& candidate,
    std::string_view assembly, const NativeRequest& request = {},
    NativeLimits limits = {});

} // namespace novostone::pcc
