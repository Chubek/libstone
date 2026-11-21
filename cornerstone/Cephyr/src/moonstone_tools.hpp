#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace cephyr {

class Diagnostics;

// Owned by a single compiler invocation. Spans are lexical and thread-affine,
// and must end before report() or destruction of the profile.
class MoonstoneProfile {
    struct Impl;
    std::unique_ptr<Impl> impl_;

public:
    class Span {
    public:
        Span() = default;
        ~Span() noexcept;
        Span(const Span &) = delete;
        Span &operator=(const Span &) = delete;

    private:
        friend class MoonstoneProfile;
        Span(Impl *impl, std::uint64_t id) noexcept : impl_(impl), id_(id) {}
        [[maybe_unused]] Impl *impl_ = nullptr;
        [[maybe_unused]] std::uint64_t id_ = 0;
    };

    MoonstoneProfile();
    ~MoonstoneProfile();
    MoonstoneProfile(const MoonstoneProfile &) = delete;
    MoonstoneProfile &operator=(const MoonstoneProfile &) = delete;

    [[nodiscard]] Span measure(std::string_view stage, std::string_view input = {}) noexcept;
    // Serializes the snapshot with the compile outcome; distinct from
    // Prolific's own collection-completeness field. Call once per invocation.
    [[nodiscard]] std::string report(bool succeeded);
};

namespace moonstone_tools {

bool profile_available() noexcept;
bool is_tool(std::string_view command) noexcept;

// Args excludes the tool flag. The caller publishes output only on success.
std::optional<std::string> run_tool(std::string_view command, std::span<const char *const> args,
                                    Diagnostics &diag);

} // namespace moonstone_tools
} // namespace cephyr
