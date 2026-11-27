#pragma once

#include <TTyUtils/TTyUtils-NetLink.hpp>

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace ttyutils::p2p {

struct PeerId {
    std::string value{};
    [[nodiscard]] friend auto operator==(const PeerId&, const PeerId&) -> bool = default;
};

class Channel {
public:
    /// Buffers local frames only. No encrypted peer connection is available.
    void send(std::string frame) { frames_.push_back(std::move(frame)); }
    [[nodiscard]] auto frames() const noexcept -> const std::vector<std::string>& { return frames_; }
    [[nodiscard]] auto connected() const noexcept -> bool { return false; }

private:
    std::vector<std::string> frames_;
};

class Discovery {
public:
    /// Record a locally supplied peer identity once; no network discovery.
    void announce(PeerId peer) {
        if (peer.value.empty()) return;
        for (const auto& existing : peers_) if (peer == existing) return;
        peers_.push_back(std::move(peer));
    }
    [[nodiscard]] auto peers() const noexcept -> const std::vector<PeerId>& { return peers_; }

private:
    std::vector<PeerId> peers_;
};

class NatTraversal {
public:
    [[nodiscard]] auto hole_punch(const net::Addr&) const noexcept -> bool { return false; }
};

class Node {
public:
    explicit Node(PeerId identity = {}) : identity_(std::move(identity)) {}
    [[nodiscard]] auto identity() const noexcept -> const PeerId& { return identity_; }
    [[nodiscard]] auto dial(const PeerId&) -> Channel { return {}; }

private:
    PeerId identity_;
};

}  // namespace ttyutils::p2p
