#include "novostone/PCC/Native.hpp"
#include "novostone/PCC/SMTProof.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>

#if defined(__linux__) && defined(__x86_64__) && defined(__LP64__)
#include <cerrno>
#include <sys/wait.h>
#include <unistd.h>
#define NOVOSTONE_TEST_EXECUTES_X86_64 1
#endif

using namespace novostone;
using namespace novostone::pcc;

namespace {

void check(bool good, const std::string& message) {
  if (!good) throw std::runtime_error(message);
}

struct Pressure {
  ir::Function function;
  NativeRequest request;
  ir::Id add;
};

Pressure pressure() {
  using ir::Op;
  Pressure p;
  p.function.arity = 6;
  p.function.entry = 1;
  auto& block = p.function.blocks.emplace(1, ir::Block{1, {}}).first->second;
  auto emit = [&](Op op, std::vector<ir::Id> operands = {}, std::uint32_t immediate = 0) {
    auto id = static_cast<ir::Id>(block.instructions.size()) + 2;
    block.instructions.push_back({id, op, immediate, std::move(operands), {}});
    return id;
  };
  std::vector<ir::Id> values;
  for (std::uint32_t arg = 0; arg < 6; ++arg) values.push_back(emit(Op::Param, {}, arg));
  for (std::uint32_t k = 0; k < 10; ++k) values.push_back(emit(Op::Const, {}, 0xfffffff0u + k));
  const auto call = emit(Op::Copy, {values.back()});
  p.request.symbol = "pcc_pressure";
  p.request.identity_calls.insert(call);
  // ReadParam after the helper has destroyed RDI through R9. The six
  // original Params are also simultaneously live across the call.
  const auto late = emit(Op::Param, {}, 0);
  auto sum = emit(Op::Add, {call, late});
  p.add = sum;
  for (const auto v : values) sum = emit(Op::Add, {sum, v});
  const auto diff = emit(Op::Sub, {sum, values.front()});
  const auto product = emit(Op::Mul, {diff, values.at(1)});
  const auto eq = emit(Op::Eq, {values.at(2), values.at(3)});
  const auto lt = emit(Op::Ult, {values.at(4), values.at(5)});
  const auto choice = emit(Op::Select, {eq, diff, product});
  const auto result = emit(Op::Select, {lt, choice, sum});
  emit(Op::Return, {result});
  return p;
}

NativeProgram lower(const ir::Function& f, const NativeRequest& request = {}) {
  auto r = lower_native(f, request);
  check(r.status == EvidenceStatus::ObligationReady && r.program.has_value(),
        "lowering failed: " + r.diagnostic);
  return std::move(*r.program);
}

NativeCheckResult checked(const ir::Function& f, const NativeProgram& p,
                          const NativeRequest& request = {}) {
  const auto result = verify_native(f, p, request);
  const auto expected = satie_smtlib2_available() ? NativeStatus::Proved : NativeStatus::Unknown;
  check(result.status == expected && result.binding_checked &&
            !result.assembly.empty() && !result.smtlib2.empty(),
        "unexpected native check result: " + result.diagnostic);
  return result;
}

void negative(const Pressure& p, const NativeProgram& program, const NativeCheckResult& result) {
  using K = NativeHome::Kind;
  using R = NativeRegister;
  const auto reject = [&](const NativeProgram& bad, const std::string& reason) {
    const auto rejected = verify_native(p.function, bad, p.request);
    check(rejected.status == NativeStatus::Rejected && !rejected.binding_checked &&
              rejected.assembly.empty(),
          "expected rejection: " + reason + " (got " + rejected.diagnostic + ")");
  };
  auto bad = program;
  bad.frame.saved.begin()->second = bad.frame.arguments.begin()->second;
  reject(bad, "overlapping frame locations");
  bad = program;
  bad.prologue.erase(bad.prologue.begin() + 1);
  reject(bad, "missing callee save");
  bad = program;
  bad.epilogue.erase(bad.epilogue.begin());
  reject(bad, "missing callee restore");
  bad = program;
  bad.prologue.front().integer += 8;
  bad.epilogue[bad.epilogue.size() - 2].integer += 8;
  bad.frame.size += 8;
  reject(bad, "misaligned call");
  bad = program;
  auto incoming = std::find_if(bad.prologue.begin(), bad.prologue.end(),
      [](auto i) { return i.op == NativeOp::Store32; });
  check(incoming != bad.prologue.end(), "missing test argument capture");
  incoming->src = R::Rax;
  reject(bad, "incorrect incoming argument");
  bad = program;
  bad.identity_helper.front().src = R::Rsi;
  reject(bad, "wrong helper contract");
  bad = program;
  bad.identity_helper.insert(bad.identity_helper.end() - 1,
                             {NativeOp::Imm32, R::R12, R::Rax, 0});
  reject(bad, "helper clobbers a callee-saved register");

  const auto call_site = [&](auto& candidate) -> auto& {
    return std::find_if(candidate.body.begin(), candidate.body.end(), [&](const auto& span) {
      return p.request.identity_calls.count(span.instruction) != 0;
    })->code;
  };
  bad = program;
  auto& site = call_site(bad);
  site.erase(std::find_if(site.begin(), site.end(), [](auto i) {
    return i.op == NativeOp::CallIdentity;
  }));
  reject(bad, "missing call");
  bad = program;
  auto& wrong_arg = call_site(bad);
  wrong_arg.insert(std::find_if(wrong_arg.begin(), wrong_arg.end(), [](auto i) {
    return i.op == NativeOp::CallIdentity;
  }), {NativeOp::Imm32, R::Rdi, R::Rax, 0});
  reject(bad, "wrong outgoing call argument");
  bad = program;
  bad.homes.begin()->second = {K::Register, R::Rdi, 0};
  const auto clobber = verify_native(p.function, bad, p.request);
  check(clobber.status == NativeStatus::Rejected &&
            clobber.diagnostic.find("call-clobbered") != std::string::npos,
        "live-through caller-save assignment not specifically rejected: " + clobber.diagnostic);

  bad = program;
  auto spill = std::find_if(bad.homes.begin(), bad.homes.end(), [](auto it) {
    return it.second.kind == K::Spill;
  });
  check(spill != bad.homes.end(), "fixture did not spill");
  const auto spill_offset = spill->second.offset;
  bool tampered = false;
  for (auto& span : bad.body) {
    for (auto& instr : span.code) {
      if (instr.op == NativeOp::Load32 && instr.integer == spill_offset) {
        instr.integer = bad.frame.size;  // would read the return address
        tampered = true;
        break;
      }
    }
    if (tampered) break;
  }
  check(tampered, "fixture did not reload its spill");
  reject(bad, "reload outside the frame");
  bad = program;
  bad.body.back().instruction = bad.body.front().instruction;
  reject(bad, "native span bound to the wrong IR instruction");
  bad = program;
  const auto add = std::find_if(bad.body.begin(), bad.body.end(), [&](const auto& span) {
    return span.instruction == p.add;
  });
  auto opcode = std::find_if(add->code.begin(), add->code.end(), [](auto i) {
    return i.op == NativeOp::Add32;
  });
  opcode->op = NativeOp::Sub32;
  const auto wrong_math = verify_native(p.function, bad, p.request);
  const auto expected = satie_smtlib2_available() ? NativeStatus::Rejected : NativeStatus::Unknown;
  check(wrong_math.status == expected &&
            wrong_math.smtlib2.find("(declare-const") != std::string::npos,
        "did not discharge a nontrivial arithmetic tamper check: " + wrong_math.diagnostic);

  const auto bytes = verify_native_assembly(p.function, program, result.assembly, p.request);
  check(bytes.status == result.status && bytes.binding_checked, "canonical bytes not accepted");
  check(verify_native_assembly(p.function, program, result.assembly + "\tnop\n", p.request)
                .status == NativeStatus::Rejected,
        "tampered assembly text accepted");
  auto changed_ir = p.function;
  for (auto& i : changed_ir.blocks.begin()->second.instructions) {
    if (i.id == p.add) i.op = ir::Op::Sub;
  }
  check(verify_native(changed_ir, program, p.request).status == expected,
        "wrong IR subject not detected");
}

void small_cases(const Pressure& p, const NativeProgram& program) {
  const auto leaf = ir::Function{0, 1, {{1, {1, {
      {2, ir::Op::Const, 0xffffffffu, {}, {}}, {3, ir::Op::Return, 0, {2}, {}}
  }}}}};
  auto candidate = lower(leaf);
  auto leaf_result = checked(leaf, candidate);
  check(candidate.frame.size == 0 && candidate.prologue.empty(), "frameless leaf not used");

  // Give the verifier a legal instruction sequence that differs from its
  // producer's sequence. It should check the code, not compare to lowering.
  candidate.body.back().code.push_back({NativeOp::Imm32, NativeRegister::R11, NativeRegister::Rax, 1});
  candidate.body.back().code.push_back({NativeOp::Add32, NativeRegister::Rax, NativeRegister::R11});
  candidate.body.back().code.push_back({NativeOp::Sub32, NativeRegister::Rax, NativeRegister::R11});
  const auto alternate = checked(leaf, candidate);
  check(alternate.assembly != leaf_result.assembly &&
            alternate.smtlib2.find("(declare-const") != std::string::npos &&
            alternate.smtlib2.find("(assert false)") == std::string::npos,
        "alternate valid lowering did not use its own nontrivial obligation");

  auto stack_args = p.function;
  stack_args.arity = 7;
  check(lower_native(stack_args, p.request).status == EvidenceStatus::Unknown,
        "stack argument subject not marked unsupported");
  auto bad_request = p.request;
  bad_request.symbol = "invalid:\n\tcall foo";
  check(lower_native(p.function, bad_request).status == EvidenceStatus::Rejected,
        "invalid assembly identifier accepted");
  bad_request = p.request;
  bad_request.identity_calls.insert(p.add);
  check(lower_native(p.function, bad_request).status == EvidenceStatus::Rejected,
        "requested call on an invalid IR opcode accepted");
  check(lower_native(p.function, p.request, {.max_ir_instructions = 1}).status == EvidenceStatus::Unknown,
        "IR resource exhaustion not Unknown");
  check(verify_native(p.function, program, p.request, {.max_native_instructions = 1}).status ==
            NativeStatus::Unknown,
        "native instruction exhaustion not Unknown");
  check(verify_native(p.function, program, p.request, {.max_smt_bytes = 0}).status ==
            NativeStatus::Unknown,
        "SMT resource exhaustion not Unknown");

  auto memory = leaf;
  auto& list = memory.blocks.at(1).instructions;
  list.insert(list.end() - 1, {4, ir::Op::Load, 0, {2}, {}});
  check(lower_native(memory).status == EvidenceStatus::Unknown, "IR memory operation not excluded");
}

#if defined(NOVOSTONE_TEST_EXECUTES_X86_64)
class TempDir {
  std::filesystem::path path_;
public:
  TempDir() {
    auto pattern = (std::filesystem::temp_directory_path() / "novostone-native-XXXXXX").string();
    const auto path = mkdtemp(pattern.data());
    check(path != nullptr, "mkdtemp failed");
    path_ = path;
  }
  ~TempDir() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }
  std::string file(const char* name) const { return (path_ / name).string(); }
};

void run(std::vector<std::string> command) {
  std::vector<char*> argv;
  for (auto& word : command) argv.push_back(word.data());
  argv.push_back(nullptr);
  const auto pid = fork();
  check(pid >= 0, "fork failed");
  if (pid == 0) {
    execvp(argv[0], argv.data());
    _exit(127);
  }
  int status = 0;
  pid_t done;
  do { done = waitpid(pid, &status, 0); } while (done < 0 && errno == EINTR);
  check(done == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0,
        "native execution command did not exit successfully: " + command.front() +
            " (wait status " + std::to_string(status) + ")");
}

void write(const std::string& path, const std::string& text) {
  std::ofstream out(path, std::ios::binary);
  out << text;
  out.close();
  check(out.good(), "could not write native test fixture");
}

void execute(const Pressure& p, const NativeCheckResult& verified, const char* compiler) {
  // Independent in-assembly wrapper: set full-width, distinct sentinels in
  // all SysV callee-saved GPRs, compare them and RSP across the real call.
  // The verifier is not used to generate this wrapper.
  const std::array regs{"rbx", "rbp", "r12", "r13", "r14", "r15"};
  const std::array inputs{
      std::array<std::uint32_t, 6>{0, 0, 0, 0, 0, 0},
      std::array<std::uint32_t, 6>{0xffffffffu, 0xffffffffu, 9, 8, 0, 0xffffffffu},
      std::array<std::uint32_t, 6>{0x80000000u, 0x80000000u, 9, 9, 8, 9},
      std::array<std::uint32_t, 6>{7, 33, 17, 18, 0xffffffffu, 1},
      std::array<std::uint32_t, 6>{42, 345, 6, 7, 0x7fffffffu, 0x80000000u},
  };
  std::ostringstream probe;
  probe << "\t.text\n\t.globl pcc_probe\n\t.type pcc_probe, @function\npcc_probe:\n";
  for (auto r : regs) probe << "\tpushq %" << r << '\n';
  probe << "\tsubq $8, %rsp\n\tmovq %rsp, (%rsp)\n";
  std::uint64_t sentinel = 0x8123456789abcdefull;
  for (auto r : regs) probe << "\tmovabsq $" << sentinel++ << ", %" << r << '\n';
  probe << "\tcall pcc_pressure\n\tcmpq (%rsp), %rsp\n\tjne .Lprobe_fail\n";
  sentinel = 0x8123456789abcdefull;
  for (auto r : regs) {
    probe << "\tmovabsq $" << sentinel++ << ", %r10\n"
          << "\tcmpq %r10, %" << r << "\n\tjne .Lprobe_fail\n";
  }
  probe << "\taddq $8, %rsp\n";
  for (auto it = regs.rbegin(); it != regs.rend(); ++it) probe << "\tpopq %" << *it << '\n';
  probe << "\tret\n.Lprobe_fail:\n\tud2\n\t.size pcc_probe, .-pcc_probe\n"
        << "\t.section .note.GNU-stack,\"\",@progbits\n";

  std::ostringstream driver;
  driver << "#include <stdint.h>\n"
         << "extern uint32_t pcc_probe(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);\n"
         << "int main(void) {\n";
  for (const auto& args : inputs) {
    const auto expected = ir::evaluate(p.function, {args.begin(), args.end()}).value;
    check(expected.has_value(), "reference evaluation failed");
    driver << "  if (pcc_probe(";
    for (std::size_t n = 0; n < args.size(); ++n)
      driver << (n ? ", " : "") << args[n] << 'u';
    driver << ") != " << *expected << "u) return 1;\n";
  }
  driver << "  return 0;\n}\n";
  TempDir tmp;
  const auto unit = tmp.file("unit.s"), wrapper = tmp.file("wrapper.s"),
             source = tmp.file("driver.c"), binary = tmp.file("driver");
  write(unit, verified.assembly);
  write(wrapper, probe.str());
  write(source, driver.str());
  run({compiler, "-std=c11", "-Werror", source, unit, wrapper, "-o", binary});
  run({binary});
}
#endif

} // namespace

int main(int argc, char** argv) {
  try {
    const auto p = pressure();
    const auto program = lower(p.function, p.request);
    check(program == lower(p.function, p.request), "non-deterministic native lowering");
    const auto result = checked(p.function, program, p.request);
    check(program.frame.saved.size() == 6 && program.frame.arguments.size() == 6 &&
              program.frame.size % 16 == 8,
          "pressure fixture did not exercise all callee saves and ABI parameters");
    negative(p, program, result);
    small_cases(p, program);
#if defined(NOVOSTONE_TEST_EXECUTES_X86_64)
    if (argc == 2) execute(p, result, argv[1]);
    else check(argc == 1, "usage: pcc-native [path-to-C-compiler]");
#else
    (void)argv;
    check(argc == 1, "native execution test requires an x86-64 Linux LP64 host");
#endif
    std::cout << "native PCC checks passed" << (argc == 2 ? " (including assembly execution)" : "") << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
