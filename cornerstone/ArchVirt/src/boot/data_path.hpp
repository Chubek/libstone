#pragma once

#include <dlfcn.h>
#include <filesystem>
#include <string>

namespace domvirt {
// dladdr also handles clients loading libdomvirt from outside the install tree.
static inline std::string data_path(const char* relative, const char* build_path) {
    Dl_info info{};
    if (dladdr(reinterpret_cast<const void*>(&data_path), &info) && info.dli_fname) {
        const auto directory = std::filesystem::absolute(info.dli_fname).parent_path();
        for (const char* base : {ARCHVIRT_BIN_TO_DATA, ARCHVIRT_LIB_TO_DATA}) {
            const auto path = directory / base / relative;
            if (std::filesystem::exists(path)) return path.string();
        }
    }
    return build_path;
}
} // namespace domvirt
