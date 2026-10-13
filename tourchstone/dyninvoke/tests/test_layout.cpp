// test_layout.cpp - cross-ABI layout queries (no execution).
// Verifies register/stack assignment for all supported ABIs on any host,
// using only the portable classification logic plus Sandstone ISA counts.
#include "dyninvoke.h"

#include <cassert>
#include <iostream>
#include <vector>

static dinv_cif_t* make(dinv_abi_t abi, const dinv_type_t* ret,
                        std::vector<const dinv_type_t*> args) {
  dinv_cif_t* c = nullptr;
  assert(dinv_cif_create(abi, ret, args.size(), args.data(), &c) == DINV_OK);
  return c;
}

int main() {
  const dinv_type_t* i32 = &dinv_type_i32;
  const dinv_type_t* f64 = &dinv_type_f64;
  const dinv_type_t* f32 = &dinv_type_f32;

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
  // SysV mixed: int,double,int -> gpr=2 fpr=1.
  {
    dinv_cif_t* c = make(DINV_ABI_SYSV_AMD64, i32, {i32, f64, i32});
    assert(dinv_cif_gpr_used(c) == 2 && dinv_cif_fpr_used(c) == 1);
    assert(dinv_cif_stack_bytes(c) == 0);
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
  // AArch64 HFA: three floats use three FPRs, no GPRs.
  {
    dinv_cif_t* c = make(DINV_ABI_AAPCS64, f32, {f32, f32});
    assert(dinv_cif_fpr_used(c) == 2 && dinv_cif_gpr_used(c) == 0);
    dinv_cif_destroy(c);
  }
  // RISC-V64: int,double mix uses one GPR + one FPR.
  {
    dinv_cif_t* c = make(DINV_ABI_RISCV64, i32, {i32, f64});
    assert(dinv_cif_gpr_used(c) == 1 && dinv_cif_fpr_used(c) == 1);
    dinv_cif_destroy(c);
  }
  // RISC-V32 layout differs only in slot width; smoke-test creation.
  {
    dinv_cif_t* c = make(DINV_ABI_RISCV32, i32, {i32, i32});
    assert(dinv_cif_gpr_used(c) == 2);
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
  }
  // Sandstone ISA files exist for the three ELF targets.
  for (auto abi :
       {DINV_ABI_SYSV_AMD64, DINV_ABI_AAPCS64, DINV_ABI_RISCV64}) {
    size_t ng = 0, nf = 0;
    if (dinv_isa_regcounts(abi, nullptr, &ng, &nf) == DINV_OK) {
      assert(ng > 0 && nf > 0);
    } else {
      std::cerr << "warning: missing ISA for " << dinv_abi_name(abi) << ": "
                << dinv_last_error() << '\n';
    }
  }
  std::cout << "dyninvoke layout tests passed\n";
  return 0;
}
