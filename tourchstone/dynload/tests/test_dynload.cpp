#include "dynload.h"

#include <cassert>
#include <cstring>
#include <iostream>

int main(int argc, char** argv) {
  assert(argc == 2);
  tdyn_env_t* env = nullptr;
  assert(tdyn_env_create(&env) == TDYN_OK);

  tdyn_library_t* lib = nullptr;
  uint32_t flags = TDYN_OPEN_NOW | TDYN_OPEN_LOCAL | TDYN_OPEN_REQUIRE_INSPECT;
  tdyn_status_t rc = tdyn_env_open(env, argv[1], flags, &lib);
  if (rc != TDYN_OK) {
    std::cerr << tdyn_last_error() << '\n';
    return 1;
  }
  assert(tdyn_library_is_native_loaded(lib));
  assert(tdyn_library_is_inspected(lib));
  assert(tdyn_library_symbol_count(lib, TDYN_SYMBOL_TABLE_ANY) > 0);

  tdyn_symbol_info_t info{};
  assert(tdyn_library_find_symbol_info(lib, "tdynload_fixture_add",
                                       TDYN_SYMBOL_TABLE_ANY, &info) == TDYN_OK);
  assert(info.name && std::strcmp(info.name, "tdynload_fixture_add") == 0);

  void* raw = nullptr;
  assert(tdyn_library_symbol(lib, "tdynload_fixture_add", &raw) == TDYN_OK);
  using AddFn = int (*)(int, int);
  AddFn add = nullptr;
  static_assert(sizeof(add) == sizeof(raw));
  std::memcpy(&add, &raw, sizeof(add));
  assert(add(2, 5) == 7);

  assert(tdyn_env_promote(env, lib, TDYN_SYMBOL_TABLE_DYNAMIC) == TDYN_OK);
  void* promoted = nullptr;
  tdyn_library_t* owner = nullptr;
  assert(tdyn_env_symbol(env, "tdynload_fixture_add", &promoted, &owner) == TDYN_OK);
  assert(promoted == raw);
  assert(owner == lib);

  assert(tdyn_library_section_count(lib) > 0);
  (void)tdyn_library_needed_count(lib);

  tdyn_env_destroy(env);
  return 0;
}
