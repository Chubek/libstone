#pragma once

#include <cstdint>
#include <string>

#include "domvirt_types.hpp"

namespace domvirt {
class BootDisplay;

namespace virtualization {

struct VirtualBootOptions {
    std::string machine{"x86_64"};
    std::string kernel_path;
    std::string initrd_path;
    std::string firmware_path;
    std::string cmdline;
    std::uint64_t memory_mb{512};
    bool use_domix{true};
    bool use_domios{true};
};

VmResult run_virtual_backend(const VirtualBootOptions& options,
                             BootDisplay& display,
                             const volatile bool& running);

} // namespace virtualization
} // namespace domvirt
