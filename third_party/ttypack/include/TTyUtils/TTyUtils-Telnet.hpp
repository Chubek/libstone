#pragma once

#include <TTyUtils/TTyUtils-NetLink.hpp>

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <utility>

namespace ttyutils::telnet {

enum class Option : std::uint8_t {
    Echo = 1, SuppressGoAhead = 3, Binary = 0, Naws = 31, Ttype = 24, NAWS = Naws, TTYPE = Ttype
};

class Negotiator {
public:
    struct Input { std::string data; std::string reply; };

    [[nodiscard]] static auto escape(const std::string_view data) -> std::string {
        std::string result;
        for (const auto value : data) {
            result.push_back(value);
            if (static_cast<unsigned char>(value) == 255) result.push_back(value);
        }
        return result;
    }

    void enable(const Option option) { enabled_[static_cast<unsigned>(option)] = true; }
    [[nodiscard]] auto enabled(const Option option) const noexcept -> bool {
        return enabled_[static_cast<unsigned>(option)];
    }
    [[nodiscard]] auto local_active(const Option option) const noexcept -> bool {
        return local_[static_cast<unsigned>(option)] == State::active;
    }
    void terminal_type(std::string type) { terminal_type_ = std::move(type); }
    /// Override option acceptance (option number, true for an option we perform).
    void on_option(std::function<bool(std::uint8_t, bool)> handler) { option_handler_ = std::move(handler); }

    /// Decode a stream chunk without losing incomplete IAC commands at read boundaries.
    [[nodiscard]] auto feed(const std::string_view bytes) -> Input {
        Input input;
        for (const unsigned char byte : bytes) {
            switch (mode_) {
            case Mode::data:
                if (byte == 255) mode_ = Mode::iac;
                else input.data += static_cast<char>(byte);
                break;
            case Mode::iac:
                mode_ = Mode::data;
                if (byte == 255) input.data += static_cast<char>(byte);
                else if (byte >= 251 && byte <= 254) { verb_ = byte; mode_ = Mode::option; }
                else if (byte == 250) mode_ = Mode::sub_option;
                break;
            case Mode::option:
                negotiate(verb_, byte, input.reply);
                mode_ = Mode::data;
                break;
            case Mode::sub_option:
                sub_option_ = byte;
                sub_payload_.clear();
                truncated_ = false;
                mode_ = Mode::sub_data;
                break;
            case Mode::sub_data:
                if (byte == 255) mode_ = Mode::sub_iac;
                else append_sub(byte);
                break;
            case Mode::sub_iac:
                if (byte == 255) { append_sub(byte); mode_ = Mode::sub_data; }
                else if (byte == 240) {
                    if (!truncated_ && sub_option_ == 24 && sub_payload_ == "\1" &&
                        local_active(Option::Ttype))
                        input.reply += subnegotiation(Option::Ttype, std::string(1, '\0') + terminal_type_);
                    mode_ = Mode::data;
                } else if (byte >= 251 && byte <= 254) {
                    verb_ = byte;
                    mode_ = Mode::option;
                } else mode_ = byte == 250 ? Mode::sub_option : Mode::data;
                break;
            }
        }
        return input;
    }
    [[nodiscard]] static auto subnegotiation(const Option option, const std::string_view payload) -> std::string {
        return "\xff\xfa" + std::string(1, static_cast<char>(option)) + escape(payload) + "\xff\xf0";
    }

private:
    enum class Mode { data, iac, option, sub_option, sub_data, sub_iac };
    enum class State { inactive, active, rejected };

    void append_sub(const unsigned char byte) {
        if (sub_payload_.size() < 4096) sub_payload_ += static_cast<char>(byte);
        else truncated_ = true;
    }
    void negotiate(const unsigned char verb, const unsigned char option, std::string& reply) {
        const bool local = verb >= 253; // DO/DONT concern local behavior; WILL/WONT concern the peer.
        auto& state = (local ? local_ : remote_)[option];
        unsigned char response = 0;
        if (verb == 251 || verb == 253) {
            if (state == State::active) return;
            const bool supported = option == 0 || option == 3 || (local ? option == 24 || option == 31 : option == 1);
            const bool accept = option_handler_ ? option_handler_(option, local) : supported && enabled_[option];
            if (!accept && state == State::rejected) return;
            state = accept ? State::active : State::rejected;
            response = local ? (accept ? 251 : 252) : (accept ? 253 : 254);
        } else {
            if (state == State::active) response = local ? 252 : 254;
            state = State::inactive;
        }
        if (response != 0) {
            reply += '\xff';
            reply += static_cast<char>(response);
            reply += static_cast<char>(option);
        }
    }

    Mode mode_{Mode::data};
    unsigned char verb_{}, sub_option_{};
    bool truncated_{};
    std::string sub_payload_;
    std::string terminal_type_{"VT100"};
    std::array<bool, 256> enabled_{};
    std::array<State, 256> local_{}, remote_{};
    std::function<bool(std::uint8_t, bool)> option_handler_;
};

class Session {
public:
    explicit Session(net::Link link = {}) : link_(std::move(link)) {}
    void enable(const Option option) { negotiator_.enable(option); }
    [[nodiscard]] auto enabled(const Option option) const noexcept -> bool { return negotiator_.enabled(option); }
    [[nodiscard]] auto send(const std::string_view data) -> bool { return link_.write(Negotiator::escape(data)); }
    [[nodiscard]] auto send_window_size(const std::uint16_t columns, const std::uint16_t rows) -> bool {
        if (!negotiator_.local_active(Option::Naws)) return false;
        const std::string payload{static_cast<char>(columns >> 8U), static_cast<char>(columns),
                                  static_cast<char>(rows >> 8U), static_cast<char>(rows)};
        return link_.write(Negotiator::subnegotiation(Option::Naws, payload));
    }
    /// Decode network input. If a response cannot be queued, close rather than desynchronize.
    [[nodiscard]] auto receive(const std::string_view bytes) -> std::string {
        auto decoded = negotiator_.feed(bytes);
        if (!decoded.reply.empty() && !link_.write(decoded.reply)) link_.close();
        return std::move(decoded.data);
    }
    [[nodiscard]] auto negotiator() noexcept -> Negotiator& { return negotiator_; }
    [[nodiscard]] auto link() noexcept -> net::Link& { return link_; }

private:
    net::Link link_;
    Negotiator negotiator_;
};

}  // namespace ttyutils::telnet
