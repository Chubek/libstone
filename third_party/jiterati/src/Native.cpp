/** @file src/Native.cpp
 *  @brief Process-level native facilities: system assembler, runner, ELF reader.
 */
#include "Jiterati-Native.hpp"
#include "Elf.hpp"
#include "TemporaryDirectory.hpp"

#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace jiterati::native {
namespace {

namespace fs = std::filesystem;

std::string read_bytes(std::string const& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    std::ostringstream out;
    out << input.rdbuf();
    return out.str();
}

void write_bytes(std::string const& path, std::string const& data) {
    fs::path file(path);
    if (file.has_parent_path()) fs::create_directories(file.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error("native: cannot write " + path);
    output.write(data.data(), static_cast<std::streamsize>(data.size()));
    output.close();
    if (!output) throw std::runtime_error("native: write failed for " + path);
}

std::string shell_quote(std::string const& text) {
    std::string out = "'";
    for (char c : text) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    return out + "'";
}

} // namespace

bool have_system_toolchain() {
#if defined(_WIN32)
    return std::system("where cc >NUL 2>&1") == 0;
#else
    return std::system("command -v cc >/dev/null 2>&1") == 0;
#endif
}

AssembleReport assemble_text(std::string_view target, std::string_view assembly) {
    AssembleReport report;
    // Route 1: Keystone when the build enabled it.
    if (asmlink::have_keystone()) {
        asmlink::AssembleResult ks = asmlink::assemble(target, assembly);
        report.ok = ks.ok;
        report.object_bytes = std::move(ks.bytes);
        report.log = ks.error;
        return report;
    }
    // Route 2: system cc for host targets.
    if (have_system_toolchain() && target == host_target() &&
        (target == "amd64" || target == "aarch64" || target == "rv64")) {
        jiterati::detail::TemporaryDirectory temporary;
        std::string stem = temporary.file("assembly");
        write_bytes(stem + ".s", to_gas(assembly, target));
        std::string log_path = stem + ".log";
        std::string command = "cc -c -o " + shell_quote(stem + ".o") + " " +
                              shell_quote(stem + ".s") + " >" + shell_quote(log_path) +
                              " 2>&1";
        int rc = std::system(command.c_str());
        report.log = read_bytes(log_path);
        if (rc == 0) {
            std::string object = read_bytes(stem + ".o");
            report.object_bytes.assign(object.begin(), object.end());
            try {
                detail::ElfImage elf(report.object_bytes);
                auto expected = asmlink::detail::elf_target(target);
                if (elf.type != 1 || elf.is64 != expected.is64 || elf.little != expected.little || elf.machine != expected.machine)
                    throw std::invalid_argument("ELF: assembler target/type mismatch");
                report.ok = true;
                report.object_is_elf = true;
            } catch (std::invalid_argument const& error) {
                report.log += "\nassembler produced an invalid object: " + std::string(error.what());
                report.object_bytes.clear();
            }
        } else {
            report.log = "cc -c failed (exit " + std::to_string(rc) + "):\n" + report.log;
        }
        return report;
    }
    report.log = "no assembler for target " + std::string(target) +
                 " (keystone disabled, no host cc route)";
    return report;
}

std::vector<std::uint8_t> link_executable(std::string_view target,
                                          std::vector<std::uint8_t> const& object_or_text,
                                          bool object_is_elf) {
    if (!object_is_elf) return asmlink::link_elf(target, object_or_text);
    detail::ElfImage elf(object_or_text);
    auto expected = asmlink::detail::elf_target(target);
    if (elf.type != 1 || elf.is64 != expected.is64 || elf.little != expected.little || elf.machine != expected.machine)
        throw std::invalid_argument("native: expected target-matching ET_REL object");
    if (!elf.has_start()) throw std::invalid_argument("native: executable requires a defined global _start process entry");
#if defined(__linux__)
    if (target != host_target() || !have_system_toolchain())
        throw std::runtime_error("native: object linking requires the Linux host cc toolchain");
    jiterati::detail::TemporaryDirectory temporary;
    auto object = temporary.file("input.o"), output = temporary.file("output"), log = temporary.file("link.log");
    write_file(object, object_or_text);
    // A real linker must preserve data sections and apply relocations. Never
    // flatten an ET_REL .text section into an executable at an arbitrary base.
    auto command = "cc -nostdlib -static -no-pie -Wl,-e,_start -o " + shell_quote(output) +
                   " " + shell_quote(object) + " >" + shell_quote(log) + " 2>&1";
    if (std::system(command.c_str()) != 0) throw std::runtime_error("native: link failed:\n" + read_bytes(log));
    auto bytes = read_bytes(output);
    std::vector<std::uint8_t> image(bytes.begin(), bytes.end());
    detail::ElfImage linked(image);
    if (linked.type != 2 || linked.machine != elf.machine || linked.is64 != elf.is64 || linked.little != elf.little)
        throw std::runtime_error("native: linker did not produce the expected executable");
    return image;
#else
    throw std::runtime_error("native: ELF object linking requires the Linux host cc toolchain");
#endif
}

RunReport run_file(std::string const& path, std::vector<std::string> const& args) {
    RunReport report;
#if defined(_WIN32)
    std::string command = shell_quote(path);
    for (auto const& arg : args) command += " " + shell_quote(arg);
    command += " >NUL 2>&1";
    report.exit_code = std::system(command.c_str());
#else
    jiterati::detail::TemporaryDirectory temporary;
    std::string out_path = temporary.file("run.log");
    std::string command = shell_quote(path);
    for (auto const& arg : args) command += " " + shell_quote(arg);
    command += " >" + shell_quote(out_path) + " 2>&1";
    int rc = std::system(command.c_str());
    if (rc == -1) { report.error = "native: process creation failed"; return report; }
    if (WIFEXITED(rc)) report.exit_code = WEXITSTATUS(rc);
    else if (WIFSIGNALED(rc)) report.exit_code = 128 + WTERMSIG(rc);
    else report.exit_code = 128;
    report.output = read_bytes(out_path);
#endif
    return report;
}

void write_file(std::string const& path, std::vector<std::uint8_t> const& bytes,
                bool executable) {
    fs::path file(path);
    if (file.has_parent_path()) fs::create_directories(file.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error("native: cannot write " + path);
    output.write(reinterpret_cast<char const*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    output.close();
    if (!output) throw std::runtime_error("native: write failed for " + path);
#if !defined(_WIN32)
    if (executable) {
        if (::chmod(path.c_str(), 0755)) throw std::runtime_error("native: chmod failed for " + path);
    }
#endif
}

std::vector<std::uint8_t> extract_text_section(std::vector<std::uint8_t> const& elf) {
    try {
        detail::ElfImage image(elf);
        std::optional<detail::ElfImage::Section> text;
        for (std::uint64_t i = 0; i < image.count(); ++i) {
            auto s = image.section(i);
            if (image.name(s) == ".text") {
                if (text) return {};
                if (s.type != 1 || !(s.flags & 4) || (s.flags & 0x800) || !s.size || s.size > (1u << 26)) return {};
                text = s;
            }
        }
        if (!text) return {};
        // Unapplied REL/RELA relocations into .text make it unsafe to execute
        // as standalone bytes, even when the relocation symbol is local.
        for (std::uint64_t i = 0; i < image.count(); ++i) {
            auto s = image.section(i);
            if ((s.type == 4 || s.type == 9) && s.size) {
                if (s.info >= image.count()) return {};
                if (image.name(image.section(s.info)) == ".text") return {};
            }
        }
        auto begin = elf.begin() + static_cast<std::size_t>(text->offset);
        return {begin, begin + static_cast<std::size_t>(text->size)};
    } catch (std::invalid_argument const&) {
        return {};
    }
}

} // namespace jiterati::native
