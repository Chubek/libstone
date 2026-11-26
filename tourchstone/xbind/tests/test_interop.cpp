#include "dyninvoke/dyninvoke.h"
#include "dynload/dynload.h"
#include "examples/hello/hello.h"
#include "xbind_discovery.hpp"

#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <type_traits>

namespace {
void require(bool ok, const char* message) {
  if (!ok) throw std::runtime_error(message);
}
}

int main(int argc, char** argv) {
  try {
    require(argc == 3, "expected a hello image and its manifest");
    const auto spec = xbind::qamrpp::load_spec_file(argv[2]);
    const auto ir = xbind::discovery::bind(argv[1], spec);
    require(ir.find("\"declaration\":\"hello_add\",\"export\":") != std::string::npos,
            "hello_add did not match the manifest");

    using Library = std::unique_ptr<tdyn_library_t, decltype(&tdyn_library_close)>;
    Library offline(tdyn_library_load(argv[1], TDYN_OPEN_INSPECT_ONLY), tdyn_library_close);
    require(offline != nullptr, "cannot inspect the hello image");
    require(tdyn_library_is_inspected(offline.get()) && !tdyn_library_is_native_loaded(offline.get()),
            "offline inspection unexpectedly native-loaded the image");
    void* address = nullptr;
    require(tdyn_library_symbol(offline.get(), "hello_add", &address) == TDYN_UNSUPPORTED &&
                address == nullptr,
            "offline inspection returned a callable address");

    const auto host = dinv_abi_host();
    if (!dinv_abi_can_execute(host)) {
      std::cerr << "Offline checks passed; dyninvoke has no host trampoline\n";
      return 77;
    }
    dinv_abi_t abi = DINV_ABI_AUTO;
    require(dinv_abi_for_object(argv[1], &abi) == DINV_OK && abi == host,
            "dyninvoke ABI selection disagrees with the running host");
    Library native(tdyn_library_load(argv[1], TDYN_OPEN_NOW | TDYN_OPEN_REQUIRE_INSPECT),
                   tdyn_library_close);
    if (!native) throw std::runtime_error(tdyn_last_error());
    require(tdyn_library_is_native_loaded(native.get()), "native loader returned an offline handle");
    require(tdyn_library_symbol(native.get(), "hello_add", &address) == TDYN_OK && address,
            "native resolver did not find hello_add");

    // Types come from the C declaration, not from the image or its symbols.
    static_assert(std::is_same_v<decltype(&hello_add), int (*)(int, int)> && sizeof(int) == 4);
    const dinv_type_t* types[]{&dinv_type_i32, &dinv_type_i32};
    int32_t a = 17, b = 25, result = 0;
    void* arguments[]{&a, &b};
    const auto status = dinv_call_simple(abi, &dinv_type_i32, address, &result, 2, types, arguments);
    if (status != DINV_OK) throw std::runtime_error(dinv_last_error());
    require(result == 42, "wrong result from dyninvoke");
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
