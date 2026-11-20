#ifndef METAVECT_HPP
#define METAVECT_HPP
#include "MetaVect.h"
#include <stdexcept>
#include <string>
#include <string_view>

namespace metavect {
class error : public std::runtime_error {
public:
    explicit error(const std::string& message) : std::runtime_error(message) {}
};

class compile_error : public error {
public:
    compile_error(mv_status status, mv_diagnostic diagnostic)
        : error(diagnostic.message ? diagnostic.message : mv_status_string(status)),
          status_(status), diagnostic_(diagnostic) {}

    mv_status status() const noexcept { return status_; }
    const mv_diagnostic& diagnostic() const noexcept { return diagnostic_; }

private:
    mv_status status_;
    mv_diagnostic diagnostic_;
};

inline const char* version() noexcept { return mv_version_string(); }
inline void check(mv_status status) {
    if (status != MV_STATUS_OK) throw error(mv_status_string(status));
}

/// Compile a byte range into an owned string, or throw compile_error on failure.
inline std::string compile(std::string_view source, mv_backend backend = MV_BACKEND_SVG) {
    struct owned_output {
        mv_output output{};
        ~owned_output() { mv_output_destroy(&output); }
    } result;
    mv_diagnostic diagnostic{};
    const auto status = mv_compile(source.empty() ? "" : source.data(), source.size(),
                                   backend, &result.output, &diagnostic);
    if (status != MV_STATUS_OK)
        throw compile_error(status, diagnostic);
    return std::string(result.output.data, result.output.size);
}
}
#endif
