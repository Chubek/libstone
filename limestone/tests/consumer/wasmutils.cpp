#include <wasmutils/WASM-Core2.hpp>
#include <wasmutils/WASM-Runtime.hpp>
#include <wasmutils/WATCompiler.hpp>

int main() {
    namespace core = limestone::wasmutils::core2;
    const std::uint8_t expression[]{0xfd, 0xff, 0x01, 0x0b};
    const auto program = core::decode(expression);
    core::check_expression(program);
    const auto result = limestone::wasmutils::execute(
        limestone::wasmutils::compile_wat("(i32.add (i32.const 20) (i32.const 22))"));
    return result.trapped || result.stack.size() != 1 || result.stack[0].bits != 42 ||
           program[0].opcode != core::Opcode{0xfd, 255};
}
