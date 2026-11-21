#ifndef CEPHYR_HOST_TARGET_HPP
#define CEPHYR_HOST_TARGET_HPP

#include <cephyr/target.hpp>

#include <span>
#include <string_view>

namespace cephyr {

struct Config;

struct NativeLoweringProvider {
    std::string_view architecture, abi, stdspec;
    bool plain_char_signed = false;
    bool wchar_signed = true;
};

// Cephyr's own C lowering support, not a list of Archon IRGen targets.
const NativeLoweringProvider *native_lowering_provider(std::string_view architecture,
                                                       std::string_view abi);

struct HostFacts {
    std::string_view machine, system; // the kernel's execution environment
    std::string_view process_machine, environment, process_abi; // Cephyr itself
    bool little_endian;
    bool rv64im;
};

struct HostPlatform {
    std::string machine, os, environment, abi;
    bool rv64im;
};

HostPlatform normalize_host(const HostFacts &facts);
std::optional<HostPlatform> detect_host(Diagnostics &diag);

struct HostTargetMapping {
    std::string_view machine, os, environment, host_abi;
    bool requires_rv64im;
    std::string_view triple;
    const NativeLoweringProvider &provider;
};

std::span<const HostTargetMapping> host_target_mappings();

// The explicit mapping is separate from the Lua bank. A matching Lua filename
// (or any classic/VM IRGen artifact) cannot opt a target into native selection.
std::optional<Target> load_host_target(const HostPlatform &host, const Config &config,
                                       std::span<const HostTargetMapping> mappings, Diagnostics &diag);

bool check_auto_tool(const Target &target, std::string_view role,
                     const std::vector<std::string> &command, Diagnostics &diag);

} // namespace cephyr

#endif
