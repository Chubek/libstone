#pragma once

#include <TTyUtils/TTyUtils-Multiplex.hpp>
#include <TTyUtils/TTyUtils-P2P.hpp>

#include <string>
#include <utility>

namespace ttyutils::share {

enum class Permissions { ReadOnly, ReadWrite };

struct Presence {
    std::string name{};
    bool connected{};
};

class Host {
public:
    explicit Host(p2p::Node&) {}
    /// Returns no token until an authenticated sharing transport is available.
    [[nodiscard]] auto invite(Permissions) const -> std::string { return {}; }
};

class Guest {
public:
    /// Unverified tokens never grant access, regardless of their spelling.
    [[nodiscard]] static auto join(const std::string&) -> Guest { return {}; }
    [[nodiscard]] auto connected() const noexcept -> bool { return false; }
    [[nodiscard]] auto permission() const noexcept -> Permissions { return Permissions::ReadOnly; }
};

}  // namespace ttyutils::share
