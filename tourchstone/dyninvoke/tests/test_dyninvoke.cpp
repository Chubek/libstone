// test_dyninvoke.cpp - execute calls through dyninvoke on the host ABI.
#include "dyninvoke.h"

#include <cassert>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#define LIB_NAME "dyninvoke_fixture.dll"
#else
#include <dlfcn.h>
#define LIB_NAME "libdyninvoke_fixture.so"
#endif

static void* openLib(const char* path) {
#if defined(_WIN32)
  return (void*)LoadLibraryA(path);
#else
  return dlopen(path, RTLD_NOW | RTLD_LOCAL);
#endif
}
static void* sym(void* h, const char* n) {
#if defined(_WIN32)
  return (void*)GetProcAddress((HMODULE)h, n);
#else
  return dlsym(h, n);
#endif
}

static void checkCall(dinv_abi_t abi, const dinv_type_t* ret, void* fn,
                      void* retOut, std::vector<const dinv_type_t*> argT,
                      std::vector<void*> argv) {
  dinv_cif_t* cif = nullptr;
  assert(dinv_cif_create(abi, ret, argT.size(), argT.data(), &cif) == DINV_OK);
  assert(dinv_call(cif, fn, retOut, argv.data()) == DINV_OK);
  dinv_cif_destroy(cif);
}

int main(int argc, char** argv) {
  assert(argc == 2);
  std::string path = argv[1];
  // argv[1] is the full fixture path from CTest.
  void* handle = openLib(path.c_str());
  if (!handle) {
    std::cerr << "cannot open fixture\n";
    return 1;
  }
  dinv_abi_t host = dinv_abi_host();
  assert(host == DINV_ABI_SYSV_AMD64 || host == DINV_ABI_WIN64 ||
         host == DINV_ABI_AAPCS64 || host == DINV_ABI_RISCV64 ||
         host == DINV_ABI_RISCV32);

  // qBFD inspect: fixture must map to the host ABI on this machine.
  {
    dinv_abi_t fa = DINV_ABI_AUTO;
    int found = 0;
    assert(dinv_inspect_object(path.c_str(), "dinvk_add2", &fa, &found) ==
           DINV_OK);
    assert(found == 1);
    // On the build host the file ABI should equal the execution ABI.
    assert(fa == host);
  }
  // ISA cross-check (needs sandstone/isa present).
  {
    size_t ng = 0, nf = 0;
    if (dinv_isa_regcounts(host, nullptr, &ng, &nf) == DINV_OK) {
      assert(ng >= 4 && nf >= 4);
    } else {
      std::cerr << "warning: isa check skipped: " << dinv_last_error()
                << '\n';
    }
  }

  // i32 add.
  {
    void* fn = sym(handle, "dinvk_add2");
    assert(fn);
    int32_t a = 2, b = 5, r = 0;
    checkCall(host, &dinv_type_i32, fn, &r, {&dinv_type_i32, &dinv_type_i32},
              {(void*)&a, (void*)&b});
    assert(r == 7);
  }
  // f64 add.
  {
    void* fn = sym(handle, "dinvk_addd");
    assert(fn);
    double a = 1.5, b = 2.25, r = 0;
    checkCall(host, &dinv_type_f64, fn, &r, {&dinv_type_f64, &dinv_type_f64},
              {(void*)&a, (void*)&b});
    assert(r == 3.75);
  }
  // f32 add.
  {
    void* fn = sym(handle, "dinvk_addf");
    assert(fn);
    float a = 1.5f, b = 2.0f, r = 0;
    checkCall(host, &dinv_type_f32, fn, &r, {&dinv_type_f32, &dinv_type_f32},
              {(void*)&a, (void*)&b});
    assert(r == 3.5f);
  }
  // mixed int/double/int.
  {
    void* fn = sym(handle, "dinvk_mixed");
    assert(fn);
    int32_t a = 1, c = 3;
    double b = 2.5;
    int32_t r = 0;
    checkCall(host, &dinv_type_i32, fn, &r,
              {&dinv_type_i32, &dinv_type_f64, &dinv_type_i32},
              {(void*)&a, (void*)&b, (void*)&c});
    assert(r == 6);
  }
  // 8 ints (forces two stack slots on SysV/Win64).
  {
    void* fn = sym(handle, "dinvk_sum8");
    assert(fn);
    int32_t v[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    int32_t r = 0;
    std::vector<const dinv_type_t*> ts(8, &dinv_type_i32);
    std::vector<void*> av;
    for (auto& x : v) av.push_back(&x);
    checkCall(host, &dinv_type_i32, fn, &r, ts, av);
    assert(r == 36);
  }
  // struct Pair add (small struct by value + return).
  {
    void* fn = sym(handle, "dinvk_pair_add");
    assert(fn);
    static const dinv_type_t* elems[] = {&dinv_type_i32, &dinv_type_i32,
                                         nullptr};
    static const dinv_type_t pairT = {DINV_STRUCT, elems};
    struct Pair {
      int x, y;
    };
    Pair a{1, 2}, b{10, 20}, r{0, 0};
    const dinv_type_t* at[] = {&pairT, &pairT};
    void* av[] = {&a, &b};
    dinv_cif_t* cif = nullptr;
    assert(dinv_cif_create(host, &pairT, 2, at, &cif) == DINV_OK);
    // Win64 returns small structs in rax; SysV in rax; both covered.
    assert(dinv_call(cif, fn, &r, av) == DINV_OK);
    dinv_cif_destroy(cif);
    assert(r.x == 11 && r.y == 22);
  }
  // closure: build (i32,i32)->i32 adder, pass to call_cb.
  if (host == DINV_ABI_SYSV_AMD64) {
    struct AddCtx {
      int base = 0;
    };
    AddCtx ctx{100};
    auto cb = [](dinv_cif_t*, void* ret, void** args, void* user) {
      int a = *(int*)args[0], b = *(int*)args[1];
      *(int*)ret = a + b + ((AddCtx*)user)->base;
    };
    dinv_cif_t* cbCif = nullptr;
    const dinv_type_t* at[] = {&dinv_type_i32, &dinv_type_i32};
    assert(dinv_cif_create(host, &dinv_type_i32, 2, at, &cbCif) == DINV_OK);
    dinv_closure_t* cl = nullptr;
    void* fnptr = nullptr;
    assert(dinv_closure_create(cbCif, +cb, &ctx, &cl, &fnptr) == DINV_OK);
    void* callCb = sym(handle, "dinvk_call_cb");
    assert(callCb);
    int32_t a = 2, b = 5, r = 0;
    void* cbArg = fnptr;
    const dinv_type_t* ct[] = {&dinv_type_ptr, &dinv_type_i32, &dinv_type_i32};
    void* cv[] = {&cbArg, &a, &b};
    dinv_cif_t* callCif = nullptr;
    assert(dinv_cif_create(host, &dinv_type_i32, 3, ct, &callCif) == DINV_OK);
    assert(dinv_call(callCif, callCb, &r, cv) == DINV_OK);
    assert(r == 107);
    dinv_cif_destroy(callCif);
    dinv_closure_destroy(cl);
    dinv_cif_destroy(cbCif);
  }

  std::cout << "dyninvoke execute tests passed\n";
  return 0;
}
