#include <cephyr/host_target.hpp>

#include <cephyr/config.hpp>
#include <cephyr/diagnostics.hpp>
#include <cephyr/process.hpp>

#include <bit>
#include <cerrno>
#include <sys/auxv.h>
#include <sys/utsname.h>
#include <system_error>
#include <utility>

namespace cephyr {
namespace {

constexpr NativeLoweringProvider rv64im{"RISCV64IM", "LP64", "stdspec/riscv/rv64im.arch"};
constexpr NativeLoweringProvider amd64{"AMD64", "SysVAMD64", "stdspec/amd64/root.arch", true};
constexpr NativeLoweringProvider aarch64{"AArch64", "AAPCS64", "stdspec/aarch64/root.arch",
                                         /* plain_char_signed */ false, /* wchar_signed */ false};
constexpr HostTargetMapping native_hosts[] = {
    {"riscv64", "linux", "gnu", "lp64", true, "riscv64-linux-gnu", rv64im},
    {"x86_64", "linux", "gnu", "lp64", false, "x86_64-linux-gnu", amd64},
    {"aarch64", "linux", "gnu", "lp64", false, "aarch64-linux-gnu", aarch64},
};

std::string lowercase(std::string_view name)
{
    std::string result(name);
    for (char &c : result)
        if (c >= 'A' && c <= 'Z')
            c += 'a' - 'A';
    return result;
}

std::string normalize_machine(std::string_view machine)
{
    auto name = lowercase(machine);
    if (name == "amd64") return "x86_64";
    if (name == "arm64") return "aarch64";
    return name;
}

void auto_error(Diagnostics &diag, std::string reason)
{
    diag.error("cephyr.target.auto", std::move(reason) + "; choose an explicit --target=TRIPLE");
}

} // namespace

const NativeLoweringProvider *native_lowering_provider(std::string_view architecture,
                                                       std::string_view abi)
{
    for (const auto *provider : {&rv64im, &amd64, &aarch64})
        if (architecture == provider->architecture && abi == provider->abi)
            return provider;
    return nullptr;
}

HostPlatform normalize_host(const HostFacts &facts)
{
    auto machine = normalize_machine(facts.machine);
    auto os = lowercase(facts.system);
    auto environment = lowercase(facts.environment);
    auto abi = lowercase(facts.process_abi);
    // A compat personality or mismatched process architecture is not evidence
    // of the kernel machine's ABI. Never round a different width/endian up.
    if (machine != normalize_machine(facts.process_machine) || !facts.little_endian)
        abi = "unknown";
    if (environment == "android" && os == "linux")
        os = "android";
    return {std::move(machine), std::move(os), std::move(environment), std::move(abi), facts.rv64im};
}

std::optional<HostPlatform> detect_host(Diagnostics &diag)
{
    utsname system{};
    if (uname(&system) != 0) {
        const int failure = errno;
        auto_error(diag, "cannot identify the host with uname: " + std::generic_category().message(failure));
        return std::nullopt;
    }

    // These properties describe the running Cephyr binary, not the configured
    // target assembler, linker, runtime compiler, or backend build compiler.
#if defined(__ANDROID__)
    constexpr auto environment = "android";
#elif defined(__GLIBC__)
    constexpr auto environment = "gnu";
#else
    constexpr auto environment = "unknown";
#endif
    [[maybe_unused]] constexpr bool lp64 = sizeof(void *) == 8 && sizeof(long) == 8;
    constexpr auto little_endian = std::endian::native == std::endian::little;
#if defined(__riscv) && __riscv_xlen == 64
    constexpr auto process_machine = "riscv64";
#if defined(__riscv_float_abi_soft)
    constexpr auto abi = lp64 ? "lp64" : "unknown";
#elif defined(__riscv_float_abi_double)
    constexpr auto abi = lp64 ? "lp64d" : "unknown";
#elif defined(__riscv_float_abi_single)
    constexpr auto abi = lp64 ? "lp64f" : "unknown";
#else
    constexpr auto abi = "unknown";
#endif
    // Linux's RISC-V ELF HWCAP bits for single-letter ISA extensions.
    constexpr unsigned long required = (1ul << ('I' - 'A')) | (1ul << ('M' - 'A'));
    const bool has_rv64im = (getauxval(AT_HWCAP) & required) == required;
#elif defined(__x86_64__)
    constexpr auto process_machine = "x86_64";
    constexpr auto abi = lp64 ? "lp64" : "x32";
    constexpr bool has_rv64im = false;
#elif defined(__aarch64__)
    constexpr auto process_machine = "aarch64";
    constexpr auto abi = lp64 ? "lp64" : "ilp32";
    constexpr bool has_rv64im = false;
#else
    constexpr auto process_machine = "";
    constexpr auto abi = "unknown";
    constexpr bool has_rv64im = false;
#endif
    return normalize_host({system.machine, system.sysname, process_machine, environment,
                           abi, little_endian, has_rv64im});
}

std::span<const HostTargetMapping> host_target_mappings()
{
    return native_hosts;
}

std::optional<Target> load_host_target(const HostPlatform &host, const Config &config,
                                       std::span<const HostTargetMapping> mappings, Diagnostics &diag)
{
    const auto label = host.machine + "-" + host.os + "-" + host.environment + " (" + host.abi + ")";
    const HostTargetMapping *selected = nullptr;
    for (const auto &mapping : mappings) {
        if (mapping.machine != host.machine || mapping.os != host.os ||
            mapping.environment != host.environment || mapping.host_abi != host.abi ||
            (mapping.requires_rv64im && !host.rv64im))
            continue;
        const auto &provider = mapping.provider;
        if (native_lowering_provider(provider.architecture, provider.abi) != &provider)
            continue;
        if (selected) {
            auto_error(diag, "ambiguous Cephyr native targets for host " + label + ": " +
                       std::string(selected->triple) + " and " + std::string(mapping.triple));
            return std::nullopt;
        }
        selected = &mapping;
    }
    if (!selected) {
        auto_error(diag, "no Cephyr native C lowering target for host " + label);
        return std::nullopt;
    }

    auto target = load_target(std::string(selected->triple), config.triple_dir, {}, config.archon_root, diag);
    if (!target)
        return std::nullopt;
    const auto &provider = selected->provider;
    const auto spec = (config.archon_root / provider.stdspec).lexically_normal();
    if (target->architecture != provider.architecture || target->abi != provider.abi ||
        target->is_linux != (selected->os == "linux") || target->specification != spec) {
        auto_error(diag, "target-bank entry '" + target->triple +
                   "' does not match the reviewed native lowering provider (expected " +
                   std::string(provider.architecture) + "/" + std::string(provider.abi) +
                   ", OS " + std::string(selected->os) + ", specification " + spec.string() + ")");
        return std::nullopt;
    }
    return target;
}

bool check_auto_tool(const Target &target, std::string_view role,
                     const std::vector<std::string> &command, Diagnostics &diag)
{
    if (!command.empty() && executable_available(command.front()))
        return true;
    const auto tool = command.empty() ? "not configured" : "'" + command.front() + "' not executable or not on PATH";
    diag.error("cephyr.target.auto.tool", "auto target " + target.triple + " requires " +
               std::string(role) + ": " + tool);
    return false;
}

} // namespace cephyr
