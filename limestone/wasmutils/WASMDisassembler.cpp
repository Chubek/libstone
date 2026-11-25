#include "CLI.hpp"
#include "WASM-Core.hpp"
#include "WAT-Print.hpp"

#include <iomanip>
#include <locale>
#include <sstream>
#include <type_traits>

namespace wasm = limestone::wasmutils;
namespace core = wasm::core;
namespace {

std::string_view type_name(core::Type type) {
    switch (type) {
    case core::Type::i32: return "i32";
    case core::Type::i64: return "i64";
    case core::Type::f32: return "f32";
    case core::Type::f64: return "f64";
    case core::Type::v128: return "v128";
    case core::Type::funcref: return "funcref";
    case core::Type::externref: return "externref";
    }
    throw std::logic_error("unrecognized Core 2.0 value type");
}

void memory(std::ostream& out, core::MemoryArg value) {
    out << " align_exp=" << value.align << " offset=" << value.offset;
}

void listing(std::ostream& out, const core::Program& program) {
    for (const auto& insn : program) {
        const auto& info = *core::lookup(insn.opcode);
        out << insn.offset << ": " << info.mnemonic;
        std::visit([&]<class T>(const T& operand) {
            using F = core::Format;
            if constexpr (std::is_same_v<T, core::BlockType>) {
                if (const auto* type = std::get_if<core::Type>(&operand.value))
                    out << " (result " << type_name(*type) << ')';
                else if (const auto* index = std::get_if<std::uint32_t>(&operand.value))
                    out << " (type " << *index << ')';
            } else if constexpr (std::is_same_v<T, core::IndexPair>) {
                out << ' ' << operand.first << ' ' << operand.second;
            } else if constexpr (std::is_same_v<T, core::BranchTable>) {
                for (auto label : operand.labels)
                    out << ' ' << label;
                out << " default=" << operand.otherwise;
            } else if constexpr (std::is_same_v<T, std::vector<core::Type>>) {
                out << " (result";
                for (auto type : operand)
                    out << ' ' << type_name(type);
                out << ')';
            } else if constexpr (std::is_same_v<T, core::Type>) {
                out << ' ' << type_name(operand);
            } else if constexpr (std::is_same_v<T, core::MemoryArg>) {
                memory(out, operand);
            } else if constexpr (std::is_same_v<T, core::MemoryLane>) {
                memory(out, operand.memory);
                out << " lane=" << unsigned(operand.lane);
            } else if constexpr (std::is_same_v<T, core::VectorBytes>) {
                // Display bytes in binary order, regardless of host byte order.
                out << " bytes=" << std::hex << std::setfill('0');
                for (auto byte : operand)
                    out << std::setw(2) << unsigned(byte);
                out << std::dec << std::setfill(' ');
            } else if constexpr (!std::is_same_v<T, std::monostate>) {
                switch (info.format) {
                case F::i32:
                case F::i64:
                case F::f32:
                case F::f64:
                    out << " bits=0x" << std::hex << +operand << std::dec;
                    break;
                default:
                    out << ' ' << +operand;
                    break;
                }
            }
        }, insn.immediate);
        out << '\n';
    }
}

} // namespace

int main(int argc, char** argv) {
    const auto mode = argc == 3 ? std::string_view(argv[1]) : std::string_view{};
    const bool help = argc == 2 && std::string_view(argv[1]) == "--help";
    if (help || (argc != 2 && mode != "--expression" && mode != "--module" && mode != "--wat")) {
        (help ? std::cout : std::cerr)
            << "usage: wasm-disassemble [--expression | --module | --wat] INPUT\n";
        return help ? 0 : 2;
    }
    try {
        const auto input = wasm::detail::read_input(argv[argc - 1]);
        const std::vector<std::uint8_t> bytes(input.begin(), input.end());
        if (mode == "--wat") {
            wasm::detail::write_output(core::print_module(core::decode_module(bytes)));
            return 0;
        }
        std::ostringstream out;
        out.imbue(std::locale::classic());
        if (mode == "--module") {
            const auto bodies = core::decode_code_section(bytes);
            for (std::size_t i = 0; i < bodies.size(); ++i) {
                const auto& body = bodies[i];
                out << "body " << i << " @" << body.offset << ":\n";
                for (const auto& local : body.locals)
                    out << "    local " << local.count << ' ' << type_name(local.type) << '\n';
                listing(out, body.instructions);
            }
        } else {
            const auto program = core::decode(bytes);
            if (mode == "--expression")
                core::check_expression(program);
            listing(out, program);
        }
        wasm::detail::write_output(out.str());
    } catch (const std::exception& error) {
        std::cerr << "wasm-disassemble: " << error.what() << '\n';
        return 1;
    }
}
