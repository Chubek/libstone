#ifndef CEPHYR_CEPHYR_HPP
#define CEPHYR_CEPHYR_HPP

#include <cephyr/Cephyr.h>

#include <initializer_list>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cephyr {

struct CompileOptions {
    const char *resource_root = nullptr;
    const char *archon_root = nullptr;
    const cephyr_sdk *sdk = nullptr;
    const cephyr_lua_options *lua = nullptr;
};

class CompileResult {
public:
    CompileResult(CompileResult &&other) noexcept
        : status_(std::exchange(other.status_, CEPHYR_INVALID_ARGUMENT)),
          result_(std::move(other.result_)) {}
    CompileResult &operator=(CompileResult &&other) noexcept {
        if (this != &other) {
            status_ = std::exchange(other.status_, CEPHYR_INVALID_ARGUMENT);
            result_ = std::move(other.result_);
        }
        return *this;
    }

    cephyr_status status() const noexcept { return status_; }
    explicit operator bool() const noexcept { return status_ == CEPHYR_OK; }

    // Views borrow the native storage, whose ownership moves with this result.
    // A moved-from result has no storage and reports CEPHYR_INVALID_ARGUMENT.
    std::string_view output() const noexcept { return view(cephyr_result_output); }
    std::string_view diagnostics() const noexcept { return view(cephyr_result_diagnostics); }

private:
    friend CompileResult compile(const cephyr_request &) noexcept;
    friend CompileResult compile(const std::vector<std::string> &, std::string_view, CompileOptions);
    friend CompileResult parse_c11(const cephyr_request &, cephyr_ast_format) noexcept;
    friend CompileResult parse_c11(const std::vector<std::string> &, cephyr_ast_format,
                                   std::string_view, CompileOptions);
    friend CompileResult emit_interface(const cephyr_request &) noexcept;
    friend CompileResult emit_interface(const std::vector<std::string> &, std::string_view,
                                        CompileOptions);
    friend CompileResult emit_mir(const cephyr_request &) noexcept;
    friend CompileResult emit_mir(const std::vector<std::string> &, std::string_view, CompileOptions);

    CompileResult(int status, cephyr_result *result) noexcept
        : status_(static_cast<cephyr_status>(status)), result_(result, cephyr_result_destroy) {}

    template<class Invoke>
    static CompileResult from_args(const std::vector<std::string> &args, std::string_view stdin_data,
                                   CompileOptions options, Invoke invoke) {
        std::vector<const char *> argv;
        argv.reserve(args.size());
        for (const auto &arg : args) {
            if (arg.find('\0') != std::string::npos)
                return CompileResult(CEPHYR_INVALID_ARGUMENT, nullptr);
            argv.push_back(arg.c_str());
        }
        return invoke(cephyr_request{sizeof(cephyr_request), argv.size(), argv.data(),
                                    stdin_data.data(), stdin_data.size(), options.resource_root,
                                    options.archon_root, options.sdk, options.lua});
    }

    std::string_view view(const char *(*get)(const cephyr_result *, size_t *)) const noexcept {
        size_t size = 0;
        const char *bytes = get(result_.get(), &size);
        return bytes ? std::string_view(bytes, size) : std::string_view{};
    }

    cephyr_status status_;
    std::unique_ptr<cephyr_result, decltype(&cephyr_result_destroy)> result_;
};

inline CompileResult compile(const cephyr_request &request) noexcept {
    cephyr_result *result = nullptr;
    const int status = cephyr_compile(&request, &result);
    return CompileResult(status, result);
}

inline CompileResult compile(const std::vector<std::string> &args, std::string_view stdin_data = {},
                              CompileOptions options = {}) {
    return CompileResult::from_args(args, stdin_data, options,
                                    [](const cephyr_request &request) { return compile(request); });
}

inline CompileResult compile(std::initializer_list<std::string> args, std::string_view stdin_data = {},
                             CompileOptions options = {}) {
    return compile(std::vector<std::string>(args), stdin_data, options);
}

inline CompileResult parse_c11(const cephyr_request &request, cephyr_ast_format format) noexcept {
    cephyr_result *result = nullptr;
    const int status = cephyr_parse_c11(&request, format, &result);
    return CompileResult(status, result);
}

inline CompileResult parse_c11(const std::vector<std::string> &args, cephyr_ast_format format,
                                std::string_view stdin_data = {}, CompileOptions options = {}) {
    return CompileResult::from_args(args, stdin_data, options, [format](const cephyr_request &request) {
        return parse_c11(request, format);
    });
}

inline CompileResult parse_c11(std::initializer_list<std::string> args, cephyr_ast_format format,
                                std::string_view stdin_data = {}, CompileOptions options = {}) {
    return parse_c11(std::vector<std::string>(args), format, stdin_data, options);
}

inline CompileResult emit_interface(const cephyr_request &request) noexcept {
    cephyr_result *result = nullptr;
    const int status = cephyr_emit_interface(&request, &result);
    return CompileResult(status, result);
}

inline CompileResult emit_interface(const std::vector<std::string> &args,
                                    std::string_view stdin_data = {},
                                    CompileOptions options = {}) {
    return CompileResult::from_args(args, stdin_data, options, [](const cephyr_request &request) {
        return emit_interface(request);
    });
}

inline CompileResult emit_interface(std::initializer_list<std::string> args,
                                    std::string_view stdin_data = {},
                                    CompileOptions options = {}) {
    return emit_interface(std::vector<std::string>(args), stdin_data, options);
}

inline CompileResult emit_mir(const cephyr_request &request) noexcept {
    cephyr_result *result = nullptr;
    const int status = cephyr_emit_mir(&request, &result);
    return CompileResult(status, result);
}

inline CompileResult emit_mir(const std::vector<std::string> &args,
                              std::string_view stdin_data = {},
                              CompileOptions options = {}) {
    return CompileResult::from_args(args, stdin_data, options,
                                    [](const cephyr_request &request) { return emit_mir(request); });
}

inline CompileResult emit_mir(std::initializer_list<std::string> args,
                              std::string_view stdin_data = {},
                              CompileOptions options = {}) {
    return emit_mir(std::vector<std::string>(args), stdin_data, options);
}

} // namespace cephyr

#endif
