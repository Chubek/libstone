#pragma once

#include <TTyTk/TTyTk-Core.hpp>
#include <TTyTk/TTyTk-IPCTools.hpp>

#include <concepts>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ttytk::p2p {

/** A peer connection over an existing local IPC channel. */
class Connection {
public:
    explicit Connection(ipc::Channel channel) : channel_(std::move(channel)) {}

    [[nodiscard]] inline auto send(const std::string_view payload) -> Result<void> {
        return channel_.send(ipc::Message{std::string{payload}});
    }

    [[nodiscard]] inline auto receive() -> Result<ipc::Message> { return channel_.receive(); }

private:
    ipc::Channel channel_{};
};

class Discovery {
public:
    inline void add(std::string peer) { peers_.push_back(std::move(peer)); }
    [[nodiscard]] inline auto peers() const noexcept -> const std::vector<std::string>& { return peers_; }
private:
    std::vector<std::string> peers_{};
};

/** Send a caller-serialized frame (e.g. a snapshot preserving colors and attributes). */
template <typename Buffer>
[[nodiscard]] inline auto sync_buffer(const Buffer&, Connection& peer,
                                      const std::string_view serialized) -> Result<void> {
    return peer.send(serialized);
}

/** Send a plain-text snapshot, joining rows with LF and omitting wide-cell continuations. */
template <typename Buffer>
    requires requires(const Buffer& buffer, std::size_t row, std::size_t column) {
        { buffer.rows() } -> std::convertible_to<std::size_t>;
        { buffer.columns() } -> std::convertible_to<std::size_t>;
        { buffer.at(row, column).grapheme } -> std::convertible_to<std::string_view>;
        { buffer.at(row, column).continuation } -> std::convertible_to<bool>;
    }
[[nodiscard]] inline auto sync_buffer(const Buffer& buffer, Connection& peer) -> Result<void> {
    std::string snapshot;
    for (std::size_t row = 0; row < static_cast<std::size_t>(buffer.rows()); ++row) {
        if (row > 0) snapshot += '\n';
        for (std::size_t column = 0; column < static_cast<std::size_t>(buffer.columns()); ++column) {
            const auto& cell = buffer.at(row, column);
            if (!cell.continuation) snapshot += std::string_view{cell.grapheme};
        }
    }
    return peer.send(snapshot);
}

}  // namespace ttytk::p2p
