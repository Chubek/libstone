#include "CLI.hpp"
#include "WASM-Instance.hpp"
#include "WASM-Module.hpp"

#include <charconv>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>

namespace wasm = limestone::wasmutils;
namespace core = wasm::core;
namespace {

std::uint64_t number(std::string_view text, unsigned width, bool allow_signed = false) {
    const auto original = text;
    const bool negative = allow_signed && text.starts_with('-');
    if (negative || (allow_signed && text.starts_with('+')))
        text.remove_prefix(1);
    const bool hex = text.starts_with("0x") || text.starts_with("0X");
    if (hex)
        text.remove_prefix(2);
    std::uint64_t value{};
    const auto [end, error] = text.empty() ? std::from_chars_result{} :
        std::from_chars(text.data(), text.data() + text.size(), value, hex ? 16 : 10);
    const auto limit = width == 32 ? std::uint64_t{0xffffffff} :
                                     std::numeric_limits<std::uint64_t>::max();
    if (text.empty() || error != std::errc{} || end != text.data() + text.size() ||
        value > (negative ? (std::uint64_t{1} << (width - 1)) : limit))
        throw std::runtime_error("invalid " + std::to_string(width) + "-bit number: " +
                                 std::string(original));
    return negative ? (std::uint64_t{0} - value) & limit : value;
}

core::Value argument(std::string_view text) {
    const auto colon = text.find(':');
    if (colon != text.npos) {
        const auto key = text.substr(0, colon);
        const auto value = text.substr(colon + 1);
        if (key == "i32") return {core::Type::i32, number(value, 32, true)};
        if (key == "i64") return {core::Type::i64, number(value, 64, true)};
        if (key == "f32") return {core::Type::f32, number(value, 32)};
        if (key == "f64") return {core::Type::f64, number(value, 64)};
        if (key == "v128" && value.size() == 34 &&
            (value.starts_with("0x") || value.starts_with("0X")))
            return {core::Type::v128, number("0x" + std::string(value.substr(18)), 64),
                    number(value.substr(0, 18), 64)};
        if (key == "funcref" && value == "null") return {core::Type::funcref, 0};
        if (key == "externref" && value == "null") return {core::Type::externref, 0};
    }
    throw std::runtime_error("expected TYPE:BITS argument, got: " + std::string(text));
}

void result(std::ostream& out, const core::Value& value) {
    unsigned digits = 8;
    switch (value.type) {
    case core::Type::i32: out << "i32:"; break;
    case core::Type::i64: out << "i64:"; digits = 16; break;
    case core::Type::f32: out << "f32:"; break;
    case core::Type::f64: out << "f64:"; digits = 16; break;
    case core::Type::funcref:
    case core::Type::externref:
        out << (value.type == core::Type::funcref ? "funcref:" : "externref:");
        if (value.bits == 0)
            out << "null";
        else
            out << "non-null";
        out << '\n';
        return;
    case core::Type::v128:
        out << "v128:0x" << std::hex << std::setfill('0') << std::setw(16) << value.high
            << std::setw(16) << value.bits << std::dec << '\n';
        return;
    }
    out << "0x" << std::hex << std::setw(static_cast<int>(digits)) << std::setfill('0')
        << value.bits << std::dec << '\n';
}

void usage(std::ostream& out) {
    out << "usage: wasm-run [--fuel N] INPUT EXPORT [TYPE:BITS ...]\n"
           "  i32/i64 accept decimal or hex, f32/f64 accept raw IEEE bits,\n"
           "  v128 accepts 32 hex digits after 0x, and references accept null.\n";
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--help") {
        usage(std::cout);
        return 0;
    }
    try {
        int pos = 1;
        auto fuel = core::default_fuel;
        if (pos < argc && std::string_view(argv[pos]) == "--fuel") {
            if (++pos >= argc) {
                usage(std::cerr);
                return 2;
            }
            fuel = number(argv[pos++], 64);
        }
        if (argc - pos < 2) {
            usage(std::cerr);
            return 2;
        }
        const auto input = wasm::detail::read_input(argv[pos++]);
        const std::string_view name = argv[pos++];
        std::vector<core::Value> args;
        for (; pos < argc; ++pos)
            args.push_back(argument(argv[pos]));
        const std::vector<std::uint8_t> bytes(input.begin(), input.end());
        core::Instance instance(core::decode_module(bytes), {}, {.start_fuel = fuel});
        std::ostringstream out;
        out.imbue(std::locale::classic());
        for (const auto& value : instance.invoke(name, args, fuel))
            result(out, value);
        wasm::detail::write_output(out.str());
    } catch (const core::Trap& error) {
        std::cerr << "wasm-run: trap " << error.what() << '\n';
        return 1;
    } catch (const std::exception& error) {
        std::cerr << "wasm-run: " << error.what() << '\n';
        return 1;
    }
}
