#undef NDEBUG
#include "dynload.h"

#include <cassert>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string_view>
#if defined(_WIN32)
#  define NOMINMAX
#  include <windows.h>
#endif
#if defined(TDYNLOAD_TEST_ELF_VERSIONS)
#  include <thread>
#endif

template <typename Fn>
Fn asFunction(void* raw) {
  Fn fn = nullptr;
  static_assert(sizeof(fn) == sizeof(raw));
  std::memcpy(&fn, &raw, sizeof(fn));
  return fn;
}

int testNative(const char* fixture) {
  tdyn_library_t* bad = reinterpret_cast<tdyn_library_t*>(1);
  assert(tdyn_library_open(fixture, TDYN_OPEN_NOW | TDYN_OPEN_LAZY, &bad) == TDYN_INVALID_ARGUMENT);
  assert(bad == nullptr);
  assert(tdyn_library_open(fixture, 1u << 31, &bad) == TDYN_INVALID_ARGUMENT);
  assert(bad == nullptr);
  // Inspect-only must fail, not return a useless successful handle.
  assert(tdyn_library_open(__FILE__, TDYN_OPEN_INSPECT_ONLY, &bad) == TDYN_INSPECT_ERROR);
  assert(bad == nullptr);
  tdyn_env_t* env = nullptr;
  assert(tdyn_env_create(&env) == TDYN_OK);

  tdyn_library_t* lib = nullptr;
  uint32_t flags = TDYN_OPEN_NOW | TDYN_OPEN_LOCAL | TDYN_OPEN_REQUIRE_INSPECT;
  tdyn_status_t rc = tdyn_env_open(env, fixture, flags, &lib);
  if (rc != TDYN_OK) {
    std::cerr << tdyn_last_error() << '\n';
    return 1;
  }
  assert(tdyn_library_is_native_loaded(lib));
  assert(tdyn_library_is_inspected(lib));
  assert(tdyn_library_symbol_count(lib, TDYN_SYMBOL_TABLE_ANY) > 0);
  assert(tdyn_library_address_bits(lib) == sizeof(void*) * 8);
  // Values unknown to a given build are not a native rejection.
  assert(tdyn_library_native_compatibility(lib) != TDYN_NATIVE_COMPAT_INCOMPATIBLE);

  const char* objectName =
#if defined(__APPLE__)
      "_tdynload_fixture_add";
#else
      "tdynload_fixture_add";
#endif
  tdyn_symbol_info_t info{};
  assert(tdyn_library_find_symbol_info(lib, objectName,
                                       TDYN_SYMBOL_TABLE_ANY, &info) == TDYN_OK);
  assert(info.name && std::strcmp(info.name, objectName) == 0);

  void* raw = nullptr;
  assert(tdyn_library_symbol(lib, "tdynload_fixture_add", &raw) == TDYN_OK);
  using AddFn = int (*)(int, int);
  assert(asFunction<AddFn>(raw)(2, 5) == 7);

  assert(tdyn_env_promote(env, lib, TDYN_SYMBOL_TABLE_DYNAMIC) == TDYN_OK);
  void* promoted = nullptr;
  tdyn_library_t* owner = nullptr;
  assert(tdyn_env_symbol(env, "tdynload_fixture_add", &promoted, &owner) == TDYN_OK);
  assert(promoted == raw);
  assert(owner == lib);

  size_t exports = 0;
  assert(tdyn_library_link_symbol_count(lib, TDYN_LINK_EXPORTS, &exports) == TDYN_OK);
  bool haveAdd = false;
  uint64_t linkedAdd = 0;
#if defined(_WIN32)
  bool haveOrdinal = false;
  bool haveForward = false;
#elif defined(TDYNLOAD_TEST_ELF_VERSIONS)
  bool haveOld = false;
  bool haveDefault = false;
#endif
  for (size_t i = 0; i < exports; ++i) {
    tdyn_link_symbol_info_t symbol{};
    assert(tdyn_library_link_symbol_info(lib, TDYN_LINK_EXPORTS, i, &symbol) == TDYN_OK);
    if (std::strcmp(symbol.name, objectName) == 0) {
      haveAdd = symbol.has_address;
      linkedAdd = symbol.address;
    }
#if defined(_WIN32)
    if (symbol.has_ordinal && symbol.ordinal == 17) {
      haveOrdinal = true;
      assert(!*symbol.name && symbol.has_address);
    }
    if (std::strcmp(symbol.name, "tdynload_fixture_forward") == 0) {
      haveForward = true;
      assert(symbol.forwarded && !symbol.has_address);
      assert(std::strcmp(symbol.target, "GetCurrentProcessId") == 0);
      assert(*symbol.library);
    }
#elif defined(TDYNLOAD_TEST_ELF_VERSIONS)
    if (std::strcmp(symbol.name, "tdynload_fixture_versioned") == 0) {
      haveOld |= std::strcmp(symbol.version, "TDYN_1") == 0 && !symbol.default_version;
      haveDefault |= std::strcmp(symbol.version, "TDYN_2") == 0 && symbol.default_version;
    }
#endif
  }
  assert(haveAdd);
  uint64_t offset = 0, linked = 0;
  assert(tdyn_library_file_offset_for_address(lib, linkedAdd, &offset) == TDYN_OK);
  assert(tdyn_library_address_for_file_offset(lib, offset, &linked) == TDYN_OK);
  assert(linked == linkedAdd);
  assert(linked >= tdyn_library_image_base(lib));
  assert(tdyn_library_file_offset_for_rva(lib, linked - tdyn_library_image_base(lib),
                                         &linked) == TDYN_OK);
  assert(linked == offset);
  uint64_t runtime = 0;
#if defined(_WIN32) || defined(TDYNLOAD_TEST_DL_PHDR)
  assert(tdyn_library_runtime_address(lib, linkedAdd, &runtime) == TDYN_OK);
  assert(runtime == reinterpret_cast<uintptr_t>(raw));
  assert(tdyn_library_runtime_address(lib, UINT64_MAX, &runtime) == TDYN_NOT_FOUND);
  assert(runtime == 0);
#else
  assert(tdyn_library_runtime_address(lib, linkedAdd, &runtime) == TDYN_UNSUPPORTED);
  assert(runtime == 0);
#endif

#if defined(_WIN32)
  assert(haveOrdinal && haveForward);
  void* selected = nullptr;
  assert(tdyn_library_symbol_ordinal(lib, 17, &selected) == TDYN_OK);
  assert(asFunction<int (*)(void)>(selected)() == 17);
  assert(tdyn_library_symbol_ordinal(lib, 30, &selected) == TDYN_NOT_FOUND);
  assert(selected == nullptr);
  assert(tdyn_library_symbol_ordinal(lib, 31, &selected) == TDYN_OK);
  assert(asFunction<DWORD (WINAPI*)(void)>(selected)() == GetCurrentProcessId());
  void* named = nullptr;
  assert(tdyn_library_symbol(lib, "tdynload_fixture_forward", &named) == TDYN_OK);
  assert(named == selected);
  assert(tdyn_library_symbol_version(lib, "tdynload_fixture_add", "TDYN_1", &named) ==
         TDYN_UNSUPPORTED);
  assert(named == nullptr);
#elif defined(TDYNLOAD_TEST_ELF_VERSIONS)
  assert(haveOld && haveDefault);
  void* versioned = nullptr;
  assert(tdyn_library_symbol_version(lib, "tdynload_fixture_versioned", "TDYN_1",
                                     &versioned) == TDYN_OK);
  assert(asFunction<int (*)(void)>(versioned)() == 1);
  assert(tdyn_library_symbol_version(lib, "tdynload_fixture_versioned", "TDYN_2",
                                     &versioned) == TDYN_OK);
  assert(asFunction<int (*)(void)>(versioned)() == 2);
  void* defaultVersion = nullptr;
  assert(tdyn_library_symbol(lib, "tdynload_fixture_versioned", &defaultVersion) == TDYN_OK);
  assert(defaultVersion == versioned);
  assert(tdyn_library_symbol_version(lib, "tdynload_fixture_versioned", "TDYN_X",
                                     &versioned) == TDYN_NOT_FOUND);
  assert(versioned == nullptr);
  assert(tdyn_library_symbol_ordinal(lib, 17, &versioned) == TDYN_UNSUPPORTED);
  assert(versioned == nullptr);
  void* mainTls = nullptr;
  assert(tdyn_env_symbol(env, "tdynload_fixture_tls", &mainTls, nullptr) == TDYN_OK);
  assert(*static_cast<int*>(mainTls) == 123);
  std::thread otherThread([&] {
    void* tls = nullptr;
    void* get = nullptr;
    assert(tdyn_env_symbol(env, "tdynload_fixture_tls", &tls, nullptr) == TDYN_OK);
    assert(tdyn_library_symbol(lib, "tdynload_fixture_tls_pointer", &get) == TDYN_OK);
    assert(tls == asFunction<int* (*)(void)>(get)());
    assert(tls != mainTls && *static_cast<int*>(tls) == 123);
  });
  otherThread.join();
#endif

  assert(tdyn_library_section_count(lib) > 0);
  (void)tdyn_library_needed_count(lib);

  size_t needed = 0;
  assert(tdyn_env_add_search_path(env, std::filesystem::path(fixture).parent_path().string().c_str()) == TDYN_OK);
  assert(tdyn_env_add_search_path(env, "a") == TDYN_OK);
  const char* path = tdyn_env_search_path(env, 1);
  for (int i = 0; i < 128; ++i) assert(tdyn_env_add_search_path(env, "b") == TDYN_OK);
  assert(std::strcmp(path, "a") == 0);
  const auto name = std::filesystem::path(fixture).filename().string();
  assert(tdyn_env_resolve_library(env, name.c_str(), nullptr, &needed) == TDYN_BUFFER_TOO_SMALL);
  assert(needed > 1);
  char tiny[1] = {'x'};
  size_t capacity = sizeof(tiny);
  assert(tdyn_env_resolve_library(env, name.c_str(), tiny, &capacity) == TDYN_BUFFER_TOO_SMALL);
  assert(capacity == needed && tiny[0] == 'x');

  raw = reinterpret_cast<void*>(1);
  assert(tdyn_library_symbol(lib, "no_such_symbol_12345", &raw) == TDYN_NOT_FOUND);
  assert(raw == nullptr);
  // An environment-owned library can be explicitly closed without dangling
  // promoted entries or a second delete during environment teardown.
  tdyn_library_close(lib);
  assert(tdyn_env_library_count(env) == 0);
  promoted = reinterpret_cast<void*>(1);
  owner = reinterpret_cast<tdyn_library_t*>(1);
  assert(tdyn_env_symbol(env, "tdynload_fixture_add", &promoted, &owner) == TDYN_NOT_FOUND);
  assert(promoted == nullptr && owner == nullptr);

  tdyn_env_destroy(env);
  env = tdyn_env_new();
  assert(env);
  tdyn_env_destroy(env);
  lib = tdyn_library_load(fixture, flags);
  assert(lib);
  env = tdyn_env_new();
  assert(env);
  assert(tdyn_env_promote(env, lib, TDYN_SYMBOL_TABLE_DYNAMIC) == TDYN_INVALID_ARGUMENT);
  tdyn_env_destroy(env);
  tdyn_library_close(lib);
  return 0;
}

int testOffline(const char* fixture) {
  std::unique_ptr<tdyn_env_t, decltype(&tdyn_env_destroy)> env(tdyn_env_new(), tdyn_env_destroy);
  assert(env);
  tdyn_library_t* lib = nullptr;
  assert(tdyn_env_open(env.get(), fixture, TDYN_OPEN_INSPECT_ONLY, &lib) == TDYN_OK);
  assert(tdyn_library_is_inspected(lib) && !tdyn_library_is_native_loaded(lib));
  assert(tdyn_env_promote(env.get(), lib, TDYN_SYMBOL_TABLE_ANY) == TDYN_UNSUPPORTED);
  void* address = reinterpret_cast<void*>(1);
  assert(tdyn_library_symbol(lib, "tdynload_fixture_add", &address) == TDYN_UNSUPPORTED);
  assert(address == nullptr);
  assert(tdyn_library_symbol_version(lib, "tdynload_fixture_versioned", "TDYN_1",
                                     &address) == TDYN_UNSUPPORTED);
  assert(tdyn_library_symbol_ordinal(lib, 17, &address) == TDYN_UNSUPPORTED);
  assert(tdyn_env_symbol(env.get(), "tdynload_fixture_add", &address, nullptr) == TDYN_NOT_FOUND);
  uint64_t runtimeAddress = UINT64_MAX;
  assert(tdyn_library_runtime_address(lib, 0, &runtimeAddress) == TDYN_UNSUPPORTED);
  assert(runtimeAddress == 0);
  tdyn_library_t* absent = nullptr;
  const auto noLoad = tdyn_library_open(fixture, TDYN_OPEN_NOLOAD, &absent);
  assert((noLoad == TDYN_NOT_FOUND || noLoad == TDYN_UNSUPPORTED) && !absent);

  // Tiny foreign shared-image headers are inspection fixtures, not executable
  // binaries. Pair ELF with Mach-O irrespective of the native processing ISA.
  const auto foreign = std::filesystem::path(fixture).string() + ".foreign-test";
#if defined(__APPLE__)
  const unsigned char header[64] = {
    0x7f, 'E', 'L', 'F', 2, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    3, 0, 62, 0, 1, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 64, 0
  };
#else
  const unsigned char header[32] = {
    0xcf, 0xfa, 0xed, 0xfe, 7, 0, 0, 1, 3, 0, 0, 0, 6
  };
#endif
  {
    std::ofstream out(foreign, std::ios::binary);
    out.write(reinterpret_cast<const char*>(header), sizeof(header));
    assert(out.good());
  }
  assert(tdyn_library_open(foreign.c_str(), TDYN_OPEN_INSPECT_ONLY, &lib) == TDYN_OK);
  assert(tdyn_library_is_inspected(lib) && !tdyn_library_is_native_loaded(lib));
  assert(tdyn_library_native_compatibility(lib) == TDYN_NATIVE_COMPAT_INCOMPATIBLE);
  tdyn_library_close(lib);
  assert(tdyn_library_open(foreign.c_str(), TDYN_OPEN_REQUIRE_INSPECT, &lib) == TDYN_UNSUPPORTED);
  assert(!lib && std::string_view(tdyn_last_error()).find("format") != std::string_view::npos);
  std::filesystem::remove(foreign);
  return 0;
}

int testMissingDependency(const char* fixture) {
  std::unique_ptr<tdyn_env_t, decltype(&tdyn_env_destroy)> env(tdyn_env_new(), tdyn_env_destroy);
  assert(env);
  assert(tdyn_env_add_search_path(env.get(),
         std::filesystem::path(fixture).parent_path().string().c_str()) == TDYN_OK);
  const auto name = std::filesystem::path(fixture).filename().string();
  tdyn_library_t* lib = nullptr;
  assert(tdyn_env_open(env.get(), name.c_str(), TDYN_OPEN_REQUIRE_INSPECT, &lib) == TDYN_NATIVE_ERROR);
  const std::string_view message(tdyn_last_error());
  assert(!lib && message.find("missing_fixture_dependency") != std::string_view::npos);
  assert(message.find("declared dependencies (not a search trace)") != std::string_view::npos);
  assert(tdyn_env_library_count(env.get()) == 0);
  return 0;
}

int main(int argc, char** argv) {
  assert(argc == 3);
  const std::string_view mode(argv[1]);
  if (mode == "offline") return testOffline(argv[2]);
  if (mode == "missing-dependency") return testMissingDependency(argv[2]);
  assert(mode == "native");
  return testNative(argv[2]);
}
