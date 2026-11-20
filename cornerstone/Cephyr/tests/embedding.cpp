#include <cephyr/Cephyr.hpp>

#include <atomic>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <type_traits>

static_assert(!std::is_copy_constructible_v<cephyr::CompileResult>);
static_assert(std::is_nothrow_move_constructible_v<cephyr::CompileResult>);

int main()
{
    const auto cwd = std::filesystem::current_path();
    auto bad = cephyr::compile({"--invalid-cephyr-option"});
    if (bad.status() != CEPHYR_COMPILE_ERROR ||
        bad.diagnostics().find("unrecognized option") == std::string_view::npos)
        return 1;

    std::atomic<int> failures{0};
    std::atomic<bool> start{false};
    const auto compile = [&](std::string symbol, std::string value) {
        while (!start.load())
            std::this_thread::yield();
        // Macros and frontend/IR state from concurrent requests must not
        // leak. Both requests also enter CPython's native backend for the
        // first time at roughly the same moment.
        const auto source = "#define NUMBER " + value + "\nint " + symbol +
                            "(void) { return NUMBER; }\n";
        auto preprocessed = cephyr::compile({"-E", "-P", "-"}, source);
        if (!preprocessed || preprocessed.output().find(value) == std::string_view::npos ||
            preprocessed.output().find("NUMBER") != std::string_view::npos)
            ++failures;
        auto assembly = cephyr::compile({"--no-cache", "-S", "-o", "-", "-"}, source);
        if (!assembly || assembly.output().find(symbol + ":") == std::string_view::npos) {
            std::cerr << assembly.diagnostics();
            ++failures;
        }
    };
    std::thread first(compile, "cephyr_cpp_first", "123");
    std::thread second(compile, "cephyr_cpp_second", "456");
    start = true;
    first.join();
    second.join();

    auto recovered = cephyr::compile(
        {"--no-cache", "-S", "-o", "-", "-"}, "int cephyr_cpp_later(void) { return 8; }\n");
    if (!recovered || recovered.output().find("cephyr_cpp_later:") == std::string_view::npos) {
        std::cerr << recovered.diagnostics();
        ++failures;
    }
    if (std::filesystem::current_path() != cwd)
        ++failures;
    return failures ? 1 : 0;
}
