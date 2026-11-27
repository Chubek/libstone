#pragma once

#include <TTyUtils/TTyUtils-NetLink.hpp>

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ttyutils::ssh {

/// SSH-facing types. No SSH handshake or authentication backend is provided.
struct Auth {
    std::string user{};
    std::string password{};
    [[nodiscard]] static auto password_auth(std::string user, std::string password) -> Auth {
        return {std::move(user), std::move(password)};
    }
};

class Channel {
public:
    /// Raw stream adoption is not a substitute for a negotiated SSH channel.
    explicit Channel(net::Link = {}) {}
    [[nodiscard]] auto write(std::string_view) const noexcept -> bool { return false; }
    [[nodiscard]] auto link() noexcept -> net::Link& { return link_; }

private:
    net::Link link_;
};

class KnownHosts {
public:
    /// Record an exact name; this does not verify a host key.
    void add(std::string host) { if (!host.empty() && !contains(host)) hosts_.push_back(std::move(host)); }
    [[nodiscard]] auto contains(const std::string_view host) const -> bool {
        for (const auto& entry : hosts_) if (entry == host) return true;
        return false;
    }

private:
    std::vector<std::string> hosts_;
};

class Sftp {
public:
    explicit Sftp(Channel channel = Channel{}) : channel_(std::move(channel)) {}
    [[nodiscard]] auto channel() noexcept -> Channel& { return channel_; }

private:
    Channel channel_;
};

class AgentClient {
public:
    [[nodiscard]] auto available() const noexcept -> bool { return false; }
};

class Session {
public:
    explicit Session(net::Link link = {}) : link_(std::move(link)) {}
    /// Fail closed rather than sending credentials over the raw transport.
    [[nodiscard]] auto auth(const Auth&) noexcept -> bool { return false; }
    [[nodiscard]] auto open_channel() -> Channel { return Channel{}; }
    [[nodiscard]] auto authenticated() const noexcept -> bool { return false; }

private:
    net::Link link_;
};

}  // namespace ttyutils::ssh
