#include "sandslab_opt.hpp"
#include "sandslab_catalog.hpp"
#include <SandSLAB.hpp>

#include <bit>
#include <charconv>
#include <cstdint>
#include <string_view>

namespace cephyr {
void shorten_amd64_constants(std::string &assembly)
{
    constexpr std::string_view prefix = "\tmovabsq $0x", suffix = ", %rax\n";
    const std::string_view input = assembly;
    std::string output;
    std::size_t copied = 0;
    try {
        for (auto pos = input.find(prefix); pos != input.npos; pos = input.find(prefix, pos)) {
            const auto start = pos;
            pos += prefix.size();
            if (start && input[start - 1] != '\n')
                continue;
            std::uint64_t value;
            auto [end, ec] = std::from_chars(input.data() + pos, input.data() + input.size(), value, 16);
            if (ec != std::errc{} || !input.substr(end - input.data()).starts_with(suffix))
                continue;

            const bool zero_extend = value <= UINT32_MAX;
            if (!zero_extend && value < UINT64_C(0xffffffff80000000))
                continue;
            // The ISA encoder takes the signed spelling of the imm32 bit pattern.
            static const sandslab::Program model(sandslab::isa::parse(sandslab_x86_isa));
            auto program = model;
            program.emit(zero_extend ? "mov_eax_imm32" : "mov_rax_sext32",
                         {{"imm", std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(value))}});
            auto code = program.compile();
            if (code.bytes.size() != (zero_extend ? 5u : 7u))
                continue;
            if (!copied)
                output.reserve(input.size());
            output.append(input.substr(copied, start - copied));
            output += "\t.byte ";
            for (auto b : code.bytes) {
                output += std::to_string(b);
                output += ',';
            }
            output.back() = '\n';
            copied = pos = end - input.data() + suffix.size();
        }
    } catch (const sandslab::Error &) {
        return; // Transactional: keep the original native assembly.
    }
    if (copied) {
        output.append(input.substr(copied));
        assembly.swap(output);
    }
}
}
