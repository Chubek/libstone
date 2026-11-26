// test_dyninvoke.cpp - execute calls through dyninvoke on the host ABI.
#undef NDEBUG
#include "dyninvoke.h"

#include <cassert>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#ifdef DINV_TEST_SYSV_AMD64
extern "C" int dinv_test_preserve(dinv_cif_t*, void*, void*, void**);
#endif

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
  dinv_abi_t host = dinv_abi_host();
  if (!dinv_abi_can_execute(host)) {
    std::cout << "native execution not supported on this build\n";
    return 77;
  }
  std::string path = argv[1];
  // argv[1] is the full fixture path from CTest.
  void* handle = openLib(path.c_str());
  if (!handle) {
    std::cerr << "cannot open fixture\n";
    return 1;
  }
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
  {
    const dinv_type_t* at[] = {&dinv_type_i8, &dinv_type_i16};
    int8_t a = -41, result = 0;
    int16_t b = 10;
    void* values[] = {&a, &b};
    assert(dinv_call_simple(host, &dinv_type_i8, sym(handle, "dinvk_narrow"),
                            &result, 2, at, values) == DINV_OK);
    assert(result == -31);
    assert(dinv_call_simple(DINV_ABI_WIN64, &dinv_type_i8, sym(handle, "dinvk_narrow"),
                            &result, 2, at, values) == DINV_UNSUPPORTED);
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
    // Mixed return register banks each start at zero, not at the eightbyte index.
    struct Mixed { int64_t integer; double real; } mixed{};
    struct Reverse { double real; int64_t integer; } reverse{};
    const dinv_type_t* me[] = {&dinv_type_i64, &dinv_type_f64, nullptr};
    const dinv_type_t mt{DINV_STRUCT, me};
    const dinv_type_t* re[] = {&dinv_type_f64, &dinv_type_i64, nullptr};
    const dinv_type_t rt{DINV_STRUCT, re};
    int64_t integer = 40;
    double real = 1.25;
    checkCall(host, &mt, sym(handle, "dinvk_mixed_pair"), &mixed,
              {&dinv_type_i64, &dinv_type_f64}, {&integer, &real});
    assert(mixed.integer == 41 && mixed.real == 2.5);
    checkCall(host, &rt, sym(handle, "dinvk_reverse_pair"), &reverse,
              {&dinv_type_f64, &dinv_type_i64}, {&real, &integer});
    assert(reverse.real == 3.75 && reverse.integer == 42);

    // Snapshot nested struct element arrays, not just their top-level pointer.
    dinv_cif_t* nested_cif = nullptr;
    {
      dinv_type_t inner = mt;
      const dinv_type_t* fields[] = {&inner, nullptr};
      dinv_type_t outer{DINV_STRUCT, fields};
      const dinv_type_t* args[] = {&dinv_type_i64, &dinv_type_f64};
      assert(dinv_cif_create(host, &outer, 2, args, &nested_cif) == DINV_OK);
      inner.kind = DINV_VOID;
      fields[0] = nullptr;
    }
    void* mixed_args[] = {&integer, &real};
    assert(dinv_call(nested_cif, sym(handle, "dinvk_mixed_pair"), &mixed, mixed_args) == DINV_OK);
    assert(mixed.integer == 41 && mixed.real == 2.5);
    dinv_cif_destroy(nested_cif);

    // Invoke a generated mixed-return closure through a real typed native call.
    auto mixed_cb = [](dinv_cif_t* cif, void* ret, void** args, void*) {
      assert(dinv_cif_nargs(cif) == 2);
      *static_cast<Mixed*>(ret) = {*static_cast<int64_t*>(args[0]) + 3,
                                  *static_cast<double*>(args[1]) * 4};
    };
    const dinv_type_t* mixed_types[] = {&dinv_type_i64, &dinv_type_f64};
    dinv_cif_t* mixed_cif = nullptr;
    assert(dinv_cif_create(host, &mt, 2, mixed_types, &mixed_cif) == DINV_OK);
    dinv_closure_t* mixed_closure = nullptr;
    void* mixed_code = nullptr;
    assert(dinv_closure_create(mixed_cif, +mixed_cb, nullptr, &mixed_closure, &mixed_code) == DINV_OK);
    dinv_cif_destroy(mixed_cif);
    Mixed (*mixed_function)(int64_t, double) = nullptr;
    static_assert(sizeof(mixed_function) == sizeof(mixed_code));
    std::memcpy(&mixed_function, &mixed_code, sizeof(mixed_code));
    mixed = mixed_function(integer, real);
    assert(mixed.integer == 43 && mixed.real == 5);
    dinv_closure_destroy(mixed_closure);

    struct Big { int64_t x, y, z; } big{};
    const dinv_type_t* be[] = {&dinv_type_i64, &dinv_type_i64, &dinv_type_i64, nullptr};
    const dinv_type_t bt{DINV_STRUCT, be};
    checkCall(host, &bt, sym(handle, "dinvk_big"), &big, {&dinv_type_i64}, {&integer});
    assert(big.x == 40 && big.y == 41 && big.z == 42);
    const dinv_type_t* spillTypes[] = {
        &dinv_type_i32, &dinv_type_i32, &dinv_type_i32, &dinv_type_i32,
        &dinv_type_i32, &dinv_type_i32, &mt, &bt, &dinv_type_f64};
    int32_t arg = 2;
    double scale = 4;
    mixed = {5, 1.25};
    void* spillArgs[] = {&arg, &arg, &arg, &arg, &arg, &arg, &mixed, &big, &scale};
    dinv_cif_t* spillCif = nullptr;
    assert(dinv_cif_create(host, &dinv_type_i64, 9, spillTypes, &spillCif) == DINV_OK);
    int64_t spillResult = 0;
    void* spillFn = sym(handle, "dinvk_spill");
#ifdef DINV_TEST_SYSV_AMD64
    assert(dinv_test_preserve(spillCif, spillFn, &spillResult, spillArgs));
#else
    assert(dinv_call(spillCif, spillFn, &spillResult, spillArgs) == DINV_OK);
#endif
    assert(spillResult == 104);
    dinv_cif_destroy(spillCif);

    auto bigCallback = +[](dinv_cif_t*, void* ret, void** args, void*) {
      auto n = *static_cast<int64_t*>(args[0]);
      *static_cast<Big*>(ret) = {n, 2 * n, 3 * n};
    };
    dinv_cif_t* bigCif = nullptr;
    const dinv_type_t* bigTypes[] = {&dinv_type_i64};
    assert(dinv_cif_create(host, &bt, 1, bigTypes, &bigCif) == DINV_OK);
    dinv_closure_t* bigClosure = nullptr;
    void* bigCode = nullptr;
    assert(dinv_abi_can_close_over(host));
    assert(dinv_closure_create(bigCif, bigCallback, nullptr, &bigClosure, &bigCode) == DINV_OK);
    dinv_cif_destroy(bigCif);
    Big (*bigFn)(int64_t) = nullptr;
    memcpy(&bigFn, &bigCode, sizeof(bigCode));
    big = bigFn(13);
    assert(big.x == 13 && big.y == 26 && big.z == 39);
    dinv_closure_destroy(bigClosure);

    struct AddCtx {
      int base = 0;
    };
    AddCtx ctx{100};
    auto cb = [](dinv_cif_t* cif, void* ret, void** args, void* user) {
      assert(dinv_cif_nargs(cif) == 2);
      assert(dinv_cif_abi(cif) == DINV_ABI_SYSV_AMD64);
      int a = *(int*)args[0], b = *(int*)args[1];
      *(int*)ret = a + b + ((AddCtx*)user)->base;
    };
    dinv_cif_t* cbCif = nullptr;
    const dinv_type_t* at[] = {&dinv_type_i32, &dinv_type_i32};
    assert(dinv_cif_create(host, &dinv_type_i32, 2, at, &cbCif) == DINV_OK);
    dinv_closure_t* cl = nullptr;
    void* fnptr = nullptr;
    assert(dinv_closure_create(cbCif, +cb, &ctx, &cl, &fnptr) == DINV_OK);
    dinv_cif_destroy(cbCif); // Closure owns the complete signature snapshot.
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
  }

  // The cif must not retain temporary user descriptors.
  dinv_cif_t* persistent = nullptr;
  {
    dinv_type_t temporary{DINV_I32, nullptr};
    const dinv_type_t* types[] = {&temporary, &temporary};
    assert(dinv_cif_create(host, &temporary, 2, types, &persistent) == DINV_OK);
    temporary.kind = DINV_VOID;
  }
  int32_t a = 3, b = 4, result = 0;
  void* values[] = {&a, &b};
  assert(dinv_call(persistent, sym(handle, "dinvk_add2"), &result, values) == DINV_OK);
  assert(result == 7);
  values[0] = nullptr;
  assert(dinv_call(persistent, sym(handle, "dinvk_add2"), &result, values) == DINV_INVALID_ARGUMENT);
  dinv_cif_destroy(persistent);

#if defined(_WIN32)
  FreeLibrary((HMODULE)handle);
#else
  dlclose(handle);
#endif

  std::cout << "dyninvoke execute tests passed\n";
  return 0;
}
