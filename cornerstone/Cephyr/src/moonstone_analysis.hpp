#pragma once

#include <cstddef>
#include <string_view>

namespace cephyr {

class Diagnostics;
struct Location;

namespace detail {

struct Function;

// Invocation-local, advisory inspection of completed, typed frontend MIR.
class MoonstoneAnalysis {
public:
    explicit MoonstoneAnalysis(Diagnostics &diagnostics) : diagnostics_(diagnostics) {}
    void inspect(const Function &);

private:
    void report(bool warning, std::string_view code, std::string_view message, Location);

    Diagnostics &diagnostics_;
    std::size_t functions_ = 0;
    std::size_t instructions_ = 0;
    std::size_t diagnostics_count_ = 0;
    bool exhausted_ = false;
};

} // namespace detail
} // namespace cephyr
