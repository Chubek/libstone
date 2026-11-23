#include <cephyr/config.hpp>
#include <cephyr/diagnostics.hpp>
#include <cephyr/host_target.hpp>

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

using namespace cephyr;
namespace fs = std::filesystem;

namespace {

struct Scratch {
    fs::path path;

    Scratch()
    {
        auto name = (fs::temp_directory_path() / "cephyr-host-target-test-XXXXXX").string();
        if (!mkdtemp(name.data()))
            throw std::runtime_error("could not create host-target test directory");
        path = name;
    }

    ~Scratch()
    {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }
};

std::string text(const Diagnostics &diag)
{
    std::ostringstream out;
    diag.render(out);
    return out.str();
}

} // namespace

int main(int argc, char **argv)
{
    assert(argc == 3); // Cephyr resources, Archon resources
    auto config = default_config(argv[1], argv[2]);
    const auto mappings = host_target_mappings();
    const auto host = normalize_host({"RISCV64", "Linux", "riscv64", "GNU", "LP64", true, true});
    assert(host.machine == "riscv64" && host.os == "linux" &&
           host.environment == "gnu" && host.abi == "lp64" && host.rv64im);
    Diagnostics success;
    auto target = load_host_target(host, config, mappings, success);
    assert(target && !success.error_count());
    assert(target->triple == "riscv64-linux-gnu" && target->architecture == "RISCV64IM" &&
           target->abi == "LP64" && target->is_linux);
    assert(target->specification == config.archon_root / "stdspec/riscv/rv64im.arch");

    // Use fixed host facts, not the test machine or a configured tool's default triple.
    const auto x64 = normalize_host({"AMD64", "Linux", "x86_64", "GNU", "LP64", true, false});
    const auto arm = normalize_host({"arm64", "Linux", "aarch64", "gnu", "lp64", true, false});
    assert(x64.machine == "x86_64" && x64.abi == "lp64" && arm.machine == "aarch64");
    const auto native_x64 = load_host_target(x64, config, mappings, success);
    assert(native_x64 && !success.error_count());
    assert(native_x64->triple == "x86_64-linux-gnu" && native_x64->architecture == "AMD64" &&
           native_x64->abi == "SysVAMD64" && native_x64->is_linux);
    assert(native_x64->specification == config.archon_root / "stdspec/amd64/root.arch");
    const auto native_arm = load_host_target(arm, config, mappings, success);
    assert(native_arm && !success.error_count());
    assert(native_arm->triple == "aarch64-linux-gnu" && native_arm->architecture == "AArch64" &&
           native_arm->abi == "AAPCS64" && native_arm->is_linux);
    assert(native_arm->specification == config.archon_root / "stdspec/aarch64/root.arch");
    const HostPlatform unsupported[] = {
        normalize_host({"aarch64", "Linux", "aarch64", "gnu", "ilp32", true, false}),
        normalize_host({"aarch64", "Linux", "aarch64", "gnu", "lp64", false, false}),
        normalize_host({"aarch64", "Linux", "aarch64", "android", "lp64", true, false}),
        normalize_host({"x86_64", "Linux", "x86_64", "gnu", "x32", true, false}),
        normalize_host({"x86_64", "Linux", "x86_64", "musl", "lp64", true, false}),
        normalize_host({"riscv64", "FreeBSD", "riscv64", "gnu", "lp64", true, true}),
        normalize_host({"riscv64", "Linux", "riscv64", "android", "lp64", true, true}),
        normalize_host({"riscv64", "Linux", "riscv64", "musl", "lp64", true, true}),
        normalize_host({"riscv64", "Linux", "riscv64", "gnu", "lp64d", true, true}),
        normalize_host({"riscv64", "Linux", "riscv64", "gnu", "lp64", true, false}),
        normalize_host({"riscv64", "Linux", "riscv64", "gnu", "lp64", false, true}),
        normalize_host({"riscv64", "Linux", "riscv32", "gnu", "ilp32", true, true}),
        normalize_host({"wasm32", "Linux", "wasm32", "gnu", "ilp32", true, false}),
    };
    for (const auto &facts : unsupported) {
        Diagnostics diag;
        assert(!load_host_target(facts, config, mappings, diag));
        assert(text(diag).find("no Cephyr native C lowering target") != std::string::npos);
    }

    // Real bundled stdspecs, including classic/VM IRGen targets, are not C providers.
    for (const auto *arch : {"RISCV64", "AMD64", "AArch64", "Wasm32", "JVM", "CIL"})
        assert(!native_lowering_provider(arch, "LP64"));
    const NativeLoweringProvider classic{"AMD64", "LP64", "stdspec/amd64/root.arch"};
    const HostTargetMapping unimplemented[] = {
        {"x86_64", "linux", "gnu", "lp64", false, "x86_64-linux-gnu", classic},
    };
    Diagnostics no_provider;
    assert(!load_host_target(x64, config, unimplemented, no_provider));
    assert(text(no_provider).find("no Cephyr native C lowering target") != std::string::npos);

    auto other = mappings.front();
    other.triple = "riscv64-unknown-linux-gnu";
    const HostTargetMapping ambiguous[] = {mappings.front(), other};
    Diagnostics conflict;
    assert(!load_host_target(host, config, ambiguous, conflict));
    const auto message = text(conflict);
    assert(message.find("ambiguous") != std::string::npos &&
           message.find(target->triple) != std::string::npos &&
           message.find(other.triple) != std::string::npos);

    Scratch scratch;
    config.triple_dir = scratch.path;
    Diagnostics absent;
    assert(!load_host_target(host, config, mappings, absent));
    assert(text(absent).find("no matching Lua target in the bank") != std::string::npos);

    const auto script = scratch.path / "riscv64-linux-gnu.lua";
    const auto write_bank = [&](std::string_view arch, std::string_view abi, std::string_view spec) {
        std::ofstream(script)
            << "return {triple = 'riscv64-linux-gnu', architecture = '" << arch
            << "', abi = '" << abi << "', os = 'linux', specification = '@archon/" << spec
            << "', assembler = {'as'}, linker = {'ld'}, archiver = {'ar'}, "
            << "assemble_flags = {}, link_flags = {}}\n";
        Diagnostics diag;
        assert(!load_host_target(host, config, mappings, diag));
        assert(text(diag).find("does not match the reviewed native lowering provider") != std::string::npos);
    };
    write_bank("RISCV64", "LP64", "stdspec/riscv/rv64i.arch");
    write_bank("RISCV64IM", "LP64D", "stdspec/riscv/rv64im.arch");
    write_bank("RISCV64IM", "LP64", "stdspec/riscv/rv64i.arch"); // claimed name alone does not qualify

    assert(setenv("PATH", scratch.path.c_str(), 1) == 0);
    target->assembler = {"test-assembler"}; // a PATH-resolved custom tool
    Diagnostics missing_tool;
    assert(!check_auto_tool(*target, "a target assembler", target->assembler, missing_tool));
    assert(text(missing_tool).find("cephyr.target.auto.tool") != std::string::npos);
    assert(text(missing_tool).find(target->assembler.front()) != std::string::npos);
    const auto assembler = scratch.path / target->assembler.front();
    std::ofstream(assembler) << "#!/bin/sh\nexit 99\n"; // must not run it to infer the host
    fs::permissions(assembler, fs::perms::owner_read | fs::perms::owner_write);
    Diagnostics non_executable;
    assert(!check_auto_tool(*target, "a target assembler", {assembler.string()}, non_executable));
    fs::permissions(assembler, fs::perms::owner_exec, fs::perm_options::add);
    Diagnostics available;
    assert(check_auto_tool(*target, "a target assembler", target->assembler, available));
    assert(check_auto_tool(*target, "a target assembler", {assembler.string(), "--fixed-arg"}, available));
}
