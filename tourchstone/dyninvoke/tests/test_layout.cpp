// test_layout.cpp - cross-ABI layout queries (no execution).
// Verifies register/stack assignment for all supported ABIs on any host,
// using only the portable classification logic plus Sandstone ISA counts.
#undef NDEBUG
#include "dyninvoke.h"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

static dinv_cif_t* make(dinv_abi_t abi, const dinv_type_t* ret,
                        std::vector<const dinv_type_t*> args) {
  dinv_cif_t* c = nullptr;
  assert(dinv_cif_create(abi, ret, args.size(), args.data(), &c) == DINV_OK);
  return c;
}

static std::vector<dinv_slot_t> slots(dinv_cif_t* c, size_t i) {
  size_t n = 0;
  assert(dinv_cif_arg_slots(c, i, nullptr, &n) == DINV_OK);
  std::vector<dinv_slot_t> out(n);
  assert(dinv_cif_arg_slots(c, i, out.data(), &n) == DINV_OK);
  return out;
}

int main() {
  // Failure paths must clear outputs, reject invalid graphs and not leak.
  dinv_type_t invalid{static_cast<dinv_kind_t>(14), nullptr};
  dinv_type_t cycle{DINV_STRUCT, nullptr};
  const dinv_type_t* cycle_elements[] = {&cycle, nullptr};
  cycle.elements = cycle_elements;
  const dinv_type_t* void_elements[] = {&dinv_type_void, nullptr};
  dinv_type_t void_field{DINV_STRUCT, void_elements};
  for (auto* bad : {&invalid, &cycle, &void_field}) {
    for (int i = 0; i < 100; ++i) {
      dinv_cif_t* output = reinterpret_cast<dinv_cif_t*>(1);
      assert(dinv_cif_create(DINV_ABI_SYSV_AMD64, bad, 0, nullptr, &output) == DINV_LAYOUT_ERROR);
      assert(output == nullptr);
    }
  }
  dinv_cif_t* empty = nullptr;
  assert(dinv_cif_create(DINV_ABI_SYSV_AMD64, &dinv_type_void, 0, nullptr, &empty) == DINV_OK);
  assert(dinv_cif_nargs(empty) == 0);
  dinv_cif_destroy(empty);
  assert(dinv_type_named("i32") == &dinv_type_i32);
  assert(dinv_type_named("nonexistent") == nullptr);
  assert(dinv_abi_can_execute(static_cast<dinv_abi_t>(7)) == 0);
  dinv_closure_t* closure = reinterpret_cast<dinv_closure_t*>(1);
  void* code = reinterpret_cast<void*>(1);
  assert(dinv_closure_create(nullptr, nullptr, nullptr, &closure, &code) == DINV_INVALID_ARGUMENT);
  assert(closure == nullptr && code == nullptr);
  const dinv_type_t* i32 = &dinv_type_i32;
  const dinv_type_t* f64 = &dinv_type_f64;
  const dinv_type_t* f32 = &dinv_type_f32;
  const dinv_type_t* i64 = &dinv_type_i64;
  const dinv_type_t* doublesFields[] = {f64, f64, nullptr};
  const dinv_type_t doubles{DINV_STRUCT, doublesFields};
  const dinv_type_t* floatFields[] = {f32, f32, nullptr};
  const dinv_type_t floats{DINV_STRUCT, floatFields};
  const dinv_type_t* nestedFields[] = {&floats, f32, nullptr};
  const dinv_type_t nested{DINV_STRUCT, nestedFields};
  const dinv_type_t* pairFields[] = {i64, i64, nullptr};
  const dinv_type_t pair{DINV_STRUCT, pairFields};

  {
    dinv_abi_t abi = DINV_ABI_AUTO;
    assert(dinv_abi_for_name("DARWIN_ARM64", &abi) == DINV_OK && abi == DINV_ABI_DARWIN_ARM64);
    assert(dinv_abi_for_name("PE_AMD64", &abi) == DINV_OK && abi == DINV_ABI_WIN64);
    assert(dinv_abi_for_name("not_abi", &abi) == DINV_NOT_FOUND && abi == DINV_ABI_AUTO);
    const dinv_type_t* fields[] = {&dinv_type_ptr, i32, nullptr};
    const dinv_type_t rec{DINV_STRUCT, fields};
    size_t size = 0, align = 0;
    assert(dinv_type_layout(DINV_ABI_RISCV32, &rec, &size, &align) == DINV_OK);
    assert(size == 8 && align == 4);
    assert(dinv_type_layout(DINV_ABI_RISCV64, &rec, &size, &align) == DINV_OK);
    assert(size == 16 && align == 8);
  }

  // SysV: 6 GPR + 8 FPR; 7th int goes to stack.
  {
    dinv_cif_t* c = make(DINV_ABI_SYSV_AMD64, i32,
                         {i32, i32, i32, i32, i32, i32, i32});
    assert(dinv_cif_gpr_used(c) == 6);
    assert(dinv_cif_stack_bytes(c) == 8);
    size_t n = 0;
    assert(dinv_cif_arg_slots(c, 6, nullptr, &n) == DINV_OK && n == 1);
    dinv_slot_t s{};
    size_t m = 1;
    assert(dinv_cif_arg_slots(c, 6, &s, &m) == DINV_OK);
    assert(s.where == DINV_LOC_STACK && s.stack_offset == 0);
    dinv_cif_destroy(c);
  }
  // INTEGER wins over SSE in the same eightbyte; if either bank cannot hold
  // a mixed aggregate, it spills whole without consuming from the other bank.
  {
    const dinv_type_t* fields[] = {f32, i32, nullptr};
    const dinv_type_t packedEightbyte{DINV_STRUCT, fields};
    const dinv_type_t* mixedFields[] = {f64, i64, nullptr};
    const dinv_type_t mixed{DINV_STRUCT, mixedFields};
    dinv_cif_t* c = make(DINV_ABI_SYSV_AMD64, &mixed,
                        {&packedEightbyte, i64, i64, i64, i64, i64, &mixed, f64});
    assert(slots(c, 0)[0].where == DINV_LOC_GPR);
    assert(slots(c, 6)[0].where == DINV_LOC_STACK && slots(c, 6)[0].size == 16);
    assert(slots(c, 7)[0].where == DINV_LOC_FPR && slots(c, 7)[0].reg == 0);
    dinv_slot_t ret[2]{};
    size_t n = 2;
    assert(dinv_cif_return_slots(c, ret, &n) == DINV_OK && n == 2);
    assert(ret[0].where == DINV_LOC_FPR && ret[0].reg == 0);
    assert(ret[1].where == DINV_LOC_GPR && ret[1].reg == 0 && ret[1].field_offset == 8);
    dinv_cif_destroy(c);
  }
  // SysV mixed: int,double,int -> gpr=2 fpr=1.
  {
    dinv_cif_t* c = make(DINV_ABI_SYSV_AMD64, i32, {i32, f64, i32});
    assert(dinv_cif_gpr_used(c) == 2 && dinv_cif_fpr_used(c) == 1);
    assert(dinv_cif_stack_bytes(c) == 0);
    dinv_cif_destroy(c);
  }
  {
    dinv_cif_t* c = make(DINV_ABI_WIN64, f64, {i32, f64, i32, f32, i32});
    assert(slots(c, 1)[0].where == DINV_LOC_FPR && slots(c, 1)[0].reg == 1);
    assert(slots(c, 3)[0].where == DINV_LOC_FPR && slots(c, 3)[0].reg == 3);
    assert(slots(c, 4)[0].stack_offset == 0);
    dinv_cif_destroy(c);
  }
  // Win64: first four in regs, fifth on stack after 32-byte shadow.
  {
    dinv_cif_t* c =
        make(DINV_ABI_WIN64, i32, {i32, i32, i32, i32, i32});
    assert(dinv_cif_stack_bytes(c) == 8);
    size_t n = 0;
    dinv_cif_arg_slots(c, 4, nullptr, &n);
    dinv_slot_t s{};
    size_t m = 1;
    dinv_cif_arg_slots(c, 4, &s, &m);
    assert(s.where == DINV_LOC_STACK && s.stack_offset == 0);
    dinv_cif_destroy(c);
  }
  // AArch64: 8 ints in regs, 9th on stack.
  {
    std::vector<const dinv_type_t*> a(9, i32);
    dinv_cif_t* c = make(DINV_ABI_AAPCS64, i32, a);
    assert(dinv_cif_gpr_used(c) == 8);
    assert(dinv_cif_stack_bytes(c) >= 4);
    dinv_cif_destroy(c);
  }
  // AAPCS64 scalar stack slots are eight bytes even for i32/f32 payloads.
  {
    std::vector<const dinv_type_t*> a(10, i32);
    dinv_cif_t* c = make(DINV_ABI_AAPCS64, i32, a);
    assert(dinv_cif_stack_bytes(c) == 16);
    dinv_slot_t slot{};
    size_t n = 1;
    assert(dinv_cif_arg_slots(c, 9, &slot, &n) == DINV_OK);
    assert(slot.stack_offset == 8);
    dinv_cif_destroy(c);
  }
  // Darwin packs spilled arguments on their natural alignment, unlike AAPCS.
  for (auto abi : {DINV_ABI_AAPCS64, DINV_ABI_DARWIN_ARM64}) {
    const bool darwin = abi == DINV_ABI_DARWIN_ARM64;
    std::vector<const dinv_type_t*> a(8, i32);
    a.insert(a.end(), {&dinv_type_i8, &dinv_type_i16, i32});
    dinv_cif_t* c = make(abi, i32, a);
    assert(slots(c, 8)[0].stack_offset == 0);
    assert(slots(c, 9)[0].stack_offset == (darwin ? 2 : 8));
    assert(slots(c, 10)[0].stack_offset == (darwin ? 4 : 16));
    dinv_cif_destroy(c);
    c = make(abi, &nested, {&nested});
    assert(dinv_cif_fpr_used(c) == 3 && dinv_cif_gpr_used(c) == 0);
    auto hfa = slots(c, 0);
    assert(hfa.size() == 3 && hfa[2].reg == 2 && hfa[2].field_offset == 8);
    dinv_cif_destroy(c);
    c = make(abi, i64, {i64, i64, i64, i64, i64, i64, i64, &pair, i64});
    assert(slots(c, 7)[0].where == DINV_LOC_STACK && slots(c, 7)[0].size == 16);
    assert(slots(c, 8)[0].stack_offset == 16);
    dinv_cif_destroy(c);
  }
  // RISC-V64: int,double mix uses one GPR + one FPR.
  {
    dinv_cif_t* c = make(DINV_ABI_RISCV64, i32, {i32, f64});
    assert(dinv_cif_gpr_used(c) == 1 && dinv_cif_fpr_used(c) == 1);
    dinv_cif_destroy(c);
  }
  // RV64 uses its final GPR for the first half of a two-XLEN argument,
  // then the stack for the remaining half.
  {
    dinv_cif_t* c = make(DINV_ABI_RISCV64, i64, {i64, i64, i64, i64, i64, i64, i64, &pair});
    auto p = slots(c, 7);
    assert(p.size() == 2 && p[0].where == DINV_LOC_GPR && p[0].reg == 7);
    assert(p[1].where == DINV_LOC_STACK && p[1].stack_offset == 0 && p[1].field_offset == 8);
    dinv_cif_destroy(c);
  }
  // ILP32D's FP slot still holds a double; integer i64 is split.
  {
    dinv_cif_t* c = make(DINV_ABI_RISCV32, &doubles, {i64, &doubles});
    auto wide = slots(c, 0);
    assert(wide.size() == 2 && wide[0].size == 4 && wide[1].field_offset == 4);
    assert(slots(c, 1).size() == 2 && slots(c, 1)[1].where == DINV_LOC_FPR);
    assert(!dinv_cif_uses_hidden_ret(c));
    dinv_cif_destroy(c);
  }
  for (auto abi : {DINV_ABI_RISCV64, DINV_ABI_RISCV32}) {
    dinv_cif_t* c = make(abi, f32, {f32, f32, f32, f32, f32, f32, f32, f32, &floats});
    auto fallback = slots(c, 8);
    assert(fallback.size() == (abi == DINV_ABI_RISCV64 ? 1 : 2));
    assert(fallback[0].where == DINV_LOC_GPR && fallback[0].reg == 0);
    dinv_cif_destroy(c);
  }
  // Large struct return uses hidden pointer on all ABIs.
  {
    static const dinv_type_t* bigElems[] = {
        &dinv_type_i64, &dinv_type_i64, &dinv_type_i64, nullptr};
    static const dinv_type_t big = {DINV_STRUCT, bigElems};
    for (auto abi :
         {DINV_ABI_SYSV_AMD64, DINV_ABI_WIN64, DINV_ABI_AAPCS64,
          DINV_ABI_RISCV64}) {
      dinv_cif_t* c = make(abi, &big, {i32});
      assert(dinv_cif_uses_hidden_ret(c) == 1);
      dinv_cif_destroy(c);
    }
    // Unsupported indirect arguments must not be misrepresented as by-value
    // stack copies (which would cause incorrect calls).
    for (auto abi : {DINV_ABI_WIN64, DINV_ABI_AAPCS64, DINV_ABI_DARWIN_ARM64,
                     DINV_ABI_RISCV64, DINV_ABI_RISCV32}) {
      dinv_cif_t* c = nullptr;
      const dinv_type_t* args[] = {&big};
      assert(dinv_cif_create(abi, i32, 1, args, &c) == DINV_UNSUPPORTED);
      assert(c == nullptr);
    }
  }
  // Sandstone must have the actual ABI carrier registers, not only nonzero
  // bank counts.
  for (auto abi :
       {DINV_ABI_SYSV_AMD64, DINV_ABI_WIN64, DINV_ABI_AAPCS64,
        DINV_ABI_DARWIN_ARM64, DINV_ABI_RISCV64, DINV_ABI_RISCV32}) {
    size_t ng = 0, nf = 0;
    assert(dinv_isa_regcounts(abi, nullptr, &ng, &nf) == DINV_OK);
    assert(ng >= 16 && nf >= 16);
  }
  {
    namespace fs = std::filesystem;
    const fs::path path("dyninvoke-isa-negative");
    fs::create_directories(path);
    std::ofstream(path / "x86.isa")
        << "arch amd64 { word_size = 64; addr_size = 64; endian = little; align = 1; }\n"
           "regclass GPR { rax(64)=0, }\nregclass FPR { xmm0(128)=0, }\n";
    size_t ng = 1, nf = 1;
    auto rc = dinv_isa_regcounts(DINV_ABI_SYSV_AMD64, path.string().c_str(), &ng, &nf);
    fs::remove(path / "x86.isa");
    fs::remove(path);
    assert(rc == DINV_LAYOUT_ERROR && ng == 0 && nf == 0);
  }
  std::cout << "dyninvoke layout tests passed\n";
  return 0;
}
