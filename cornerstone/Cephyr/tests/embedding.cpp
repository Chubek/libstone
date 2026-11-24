#include <cephyr/Cephyr.hpp>
#include <cephyr/CephyrSDK.h>

#include <atomic>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <memory>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>

static_assert(!std::is_copy_constructible_v<cephyr::CompileResult>);
static_assert(std::is_nothrow_move_constructible_v<cephyr::CompileResult>);
static_assert(std::is_nothrow_move_assignable_v<cephyr::CompileResult>);

namespace {

struct CallbackState {
    bool wrong_thread_accepted = true;
    cephyr_sdk_output saved{};
};

uint32_t desugar(const cephyr_sdk_input *, const cephyr_sdk_output *output, void *user)
{
    auto &state = *static_cast<CallbackState *>(user);
    state.saved = *output;
    constexpr std::string_view source = "int cephyr_cpp_sdk;\n";
    const cephyr_sdk_text replacement{source.data(), source.size()};
    std::thread other([&] {
        state.wrong_thread_accepted = output->set_text(output->context, replacement) != 0;
    });
    other.join();
    return output->set_text(output->context, replacement) ? CEPHYR_SDK_OK : CEPHYR_SDK_REJECT;
}

uint32_t throwing_callback(const cephyr_sdk_input *, const cephyr_sdk_output *, void *)
{
    throw 42;
}

} // namespace

int main()
{
    const auto cwd = std::filesystem::current_path();
    auto bad = cephyr::compile({"--invalid-cephyr-option"});
    if (bad.status() != CEPHYR_COMPILE_ERROR ||
        bad.diagnostics().find("unrecognized option") == std::string_view::npos)
        return 1;

    auto version = cephyr::compile({"--version"});
    const auto view = version.output();
    auto moved = std::move(version);
    if (version || version.status() != CEPHYR_INVALID_ARGUMENT || !version.output().empty() ||
        !moved || moved.output() != view || view.empty())
        return 1;
    bad = std::move(moved);
    if (moved || !bad || bad.output().data() != view.data() || !bad.diagnostics().empty())
        return 1;

    CallbackState state;
    cephyr_sdk_hook hook{CEPHYR_SDK_DESUGAR, {"cpp-hook", 8}, {}, desugar, &state};
    const cephyr_sdk sdk{sizeof(sdk), CEPHYR_SDK_ABI_VERSION, &hook, 1};
    const cephyr::CompileOptions options{nullptr, nullptr, &sdk};
    auto transformed = cephyr::compile({"-E", "-P", "-"}, "must be replaced", options);
    if (!transformed || transformed.output().find("cephyr_cpp_sdk") == std::string_view::npos ||
        state.wrong_thread_accepted)
        return 1;
    // Call a copied sink outside its callback. It must not touch expired state.
    if (state.saved.set_text(state.saved.context, {}))
        return 1;
    // Parse-only still runs source hooks but must not invoke a MIR analysis hook.
    const cephyr_sdk_hook ast_hooks[] = {
        hook, {CEPHYR_SDK_ANALYZE, {"no-mir", 6}, {}, throwing_callback, nullptr}
    };
    const cephyr_sdk ast_sdk{sizeof(ast_sdk), CEPHYR_SDK_ABI_VERSION, ast_hooks, 2};
    auto ast = cephyr::parse_c11({"-"}, CEPHYR_AST_YAML, "must be replaced",
                                 {nullptr, nullptr, &ast_sdk});
    if (!ast || !ast.output().starts_with("schema_version: 1\n") ||
        ast.output().find("cephyr_cpp_sdk") == std::string_view::npos)
        return 1;
    if (!cephyr::compile({"-fsyntax-only", "-x", "c11-ast-yaml", "-"}, ast.output(), options))
        return 1; // importing must not run desugar again
    hook.callback = throwing_callback;
    auto thrown = cephyr::compile({"-E", "-"}, "int untouched;\n", options);
    if (thrown.status() != CEPHYR_COMPILE_ERROR ||
        thrown.diagnostics().find("callback threw an exception") == std::string_view::npos)
        return 1;
    hook.callback = desugar;
    if (!cephyr::compile({"-E", "-"}, "int untouched;\n", options))
        return 1;

    const char *const preprocess_args[] = {"-E", "-P", "-"};
    const char *const assembly_args[] = {"--no-cache", "-S", "-o", "-", "-"};
    using Context = std::unique_ptr<cephyr_context, decltype(&cephyr_context_destroy)>;
    const auto make_context = [](std::size_t argc, const char *const *argv) {
        const cephyr_context_options config{sizeof(config), argc, argv, nullptr, nullptr};
        cephyr_context *context = nullptr;
        cephyr_context_create(&config, &context);
        return Context(context, cephyr_context_destroy);
    };
    const auto preprocess = make_context(std::size(preprocess_args), preprocess_args);
    const auto assemble = make_context(std::size(assembly_args), assembly_args);
    if (!preprocess || !assemble)
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
        cephyr_result *result = nullptr;
        int status = cephyr_context_compile(preprocess.get(), source.data(), source.size(), nullptr, &result);
        const std::unique_ptr<cephyr_result, decltype(&cephyr_result_destroy)>
            preprocessed(result, cephyr_result_destroy);
        const char *text = cephyr_result_output(result, nullptr);
        if (status != CEPHYR_OK || !text || std::string_view(text).find(value) == std::string_view::npos ||
            std::string_view(text).find("NUMBER") != std::string_view::npos)
            ++failures;
        status = cephyr_context_compile(assemble.get(), source.data(), source.size(), nullptr, &result);
        const std::unique_ptr<cephyr_result, decltype(&cephyr_result_destroy)>
            assembly(result, cephyr_result_destroy);
        text = cephyr_result_output(result, nullptr);
        if (status != CEPHYR_OK || !text || std::string_view(text).find(symbol + ":") == std::string_view::npos) {
            if (const char *diagnostics = cephyr_result_diagnostics(result, nullptr))
                std::cerr << diagnostics;
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
