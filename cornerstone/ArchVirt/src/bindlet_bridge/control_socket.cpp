#include "control_socket.hpp"
#include "file_transfer.hpp"
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <poll.h>
#include <chrono>
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <iostream>
#include <nlohmann/json.hpp>

namespace domvirt::bindlet_bridge {

ControlSocket::ControlSocket() = default;

ControlSocket::~ControlSocket() {
    stop();
}

VmResult ControlSocket::start(const std::string& socket_path) {
    if (running_.load()) {
        return {VmErrorCode::SUCCESS, "Already running"};
    }

    listen_fd_ = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (listen_fd_ < 0) {
        return {VmErrorCode::BRIDGE_ERROR, "Failed to create socket"};
    }

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (socket_path.empty() || socket_path.size() >= sizeof(addr.sun_path)) {
        close(listen_fd_); listen_fd_ = -1; socket_path_.clear();
        return {VmErrorCode::BRIDGE_ERROR, "Control socket path is empty or too long"};
    }
    std::strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);

    // Binding must not delete another test runner's socket or a regular file.
    if (bind(listen_fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(listen_fd_);
        listen_fd_ = -1;
        return {VmErrorCode::BRIDGE_ERROR, "Failed to bind socket"};
    }
    socket_path_ = socket_path;
    if (chmod(socket_path.c_str(), 0600) < 0) {
        stop();
        return {VmErrorCode::BRIDGE_ERROR, "Failed to set control socket permissions"};
    }

    if (listen(listen_fd_, 8) < 0) {
        stop();
        return {VmErrorCode::BRIDGE_ERROR, "Failed to listen on socket"};
    }

    event_fd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (event_fd_ < 0) {
        stop();
        return {VmErrorCode::BRIDGE_ERROR, "Failed to create eventfd"};
    }

    running_.store(true);
    try { server_thread_ = std::thread(&ControlSocket::server_loop, this); }
    catch (const std::exception& error) { stop(); return {VmErrorCode::BRIDGE_ERROR, error.what()}; }

    return {VmErrorCode::SUCCESS, "Control socket started"};
}

void ControlSocket::stop() {
    running_.store(false);

    if (event_fd_ >= 0) {
        uint64_t val = 1;
        write(event_fd_, &val, sizeof(val));
    }

    if (server_thread_.joinable()) {
        server_thread_.join();
    }

    if (listen_fd_ >= 0) {
        close(listen_fd_);
        listen_fd_ = -1;
    }

    if (event_fd_ >= 0) {
        close(event_fd_);
        event_fd_ = -1;
    }

    if (!socket_path_.empty()) {
        unlink(socket_path_.c_str());
        socket_path_.clear();
    }
}

void ControlSocket::register_handler(const std::string& command, ControlCommandHandler handler) {
    std::lock_guard<std::mutex> lock(handlers_mutex_);
    handlers_.emplace_back(command, std::move(handler));
}

void ControlSocket::set_file_transfer_engine(FileTransferEngine* engine) {
    ft_engine_ = engine;
}

void ControlSocket::server_loop() {
    using Clock = std::chrono::steady_clock;
    constexpr std::size_t max_request = 256 * 1024;
    struct Client {
        int fd;
        std::string input, output;
        std::size_t sent{0};
        Clock::time_point deadline{Clock::now() + std::chrono::seconds(5)};
    };
    std::vector<Client> clients;
    while (running_.load()) {
        std::vector<pollfd> polls{{listen_fd_, POLLIN, 0}, {event_fd_, POLLIN, 0}};
        for (const auto& client : clients)
            polls.push_back({client.fd, static_cast<short>(client.output.empty() ? POLLIN : POLLOUT), 0});
        if (poll(polls.data(), polls.size(), 100) < 0) { if (errno == EINTR) continue; break; }
        if (!running_.load()) break;
        const auto existing = clients.size();
        for (std::size_t i = 0; i < existing; ++i) {
            auto& client = clients[i];
            const auto events = polls[i + 2].revents;
            auto close_client = [&] { close(client.fd); client.fd = -1; };
            if (Clock::now() >= client.deadline || (events & (POLLERR | POLLNVAL))) { close_client(); continue; }
            if (client.output.empty() && (events & (POLLIN | POLLHUP))) {
                char buffer[8192];
                bool eof = false;
                for (;;) {
                    auto n = recv(client.fd, buffer, sizeof(buffer), 0);
                    if (n > 0) client.input.append(buffer, static_cast<std::size_t>(n));
                    else { eof = n == 0; if (n < 0 && errno == EINTR) continue; break; }
                    if (client.input.size() > max_request) break;
                }
                if (client.input.size() > max_request) {
                    client.output = R"({"error":{"code":"request-too-large","message":"Request exceeds 256 KiB"}})";
                } else {
                    const auto newline = client.input.find('\n');
                    if (newline != std::string::npos || eof || nlohmann::json::accept(client.input)) {
                        client.output = process_request(client.input.substr(0, newline));
                    }
                }
                if (!client.output.empty()) {
                    client.output += '\n';
                    client.deadline = Clock::now() + std::chrono::seconds(5);
                }
            }
            if (!client.output.empty()) {
                const auto n = send(client.fd, client.output.data() + client.sent,
                                    client.output.size() - client.sent, MSG_NOSIGNAL);
                if (n > 0) client.sent += static_cast<std::size_t>(n);
                else if (n < 0 && errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) { close_client(); continue; }
                if (client.sent == client.output.size()) close_client();
            }
        }
        std::erase_if(clients, [](const Client& client) { return client.fd < 0; });
        if (polls[0].revents & POLLIN) {
            for (;;) {
                const int fd = accept4(listen_fd_, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
                if (fd < 0) break;
                if (clients.size() >= 32) close(fd);
                else clients.push_back(Client{fd});
            }
        }
    }
    for (const auto& client : clients) close(client.fd);
    running_.store(false);
}

std::string ControlSocket::process_request(const std::string& request) {
    nlohmann::json response;
    nlohmann::json req;
    auto reply = [&](nlohmann::json value) {
        if (req.is_object() && req.contains("id")) value["id"] = req["id"];
        // Parser diagnostics can quote malformed UTF-8 from a bad request.
        return value.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    };

    try {
        req = nlohmann::json::parse(request);
        if (!req.is_object()) throw std::invalid_argument("Request must be a JSON object");
        std::string execute = req.value("execute", req.value("command", ""));

        if (execute.empty()) {
            throw std::invalid_argument("Missing execute field");
        }

        ControlCommandHandler selected;
        {
            std::lock_guard lock(handlers_mutex_);
            for (const auto& [command, handler] : handlers_)
                if (execute == command) { selected = handler; break; }
        }
        if (selected) return reply(nlohmann::json::parse(selected(request)));

        if (execute == "status") {
            response["return"]["running"] = true;
            return reply(response);
        }

        if (execute == "file_push" && ft_engine_) {
            std::string host_path = req.value("host_path", "");
            std::string guest_path = req.value("guest_path", "");
            if (host_path.empty() || guest_path.empty()) {
                response["error"] = "file_push requires host_path and guest_path";
                return reply(response);
            }
            auto result = ft_engine_->push_file(host_path.c_str(), guest_path.c_str());
            if (result) {
                auto& stats = ft_engine_->last_stats();
                response["return"]["bytes_transferred"] = stats.bytes_transferred;
                response["return"]["chunks_sent"] = stats.chunks_sent;
                response["return"]["status"] = "ok";
            } else {
                response["error"] = result.message;
            }
            return reply(response);
        }

        if (execute == "file_pull" && ft_engine_) {
            std::string guest_path = req.value("guest_path", "");
            std::string host_path = req.value("host_path", "");
            if (host_path.empty() || guest_path.empty()) {
                response["error"] = "file_pull requires guest_path and host_path";
                return reply(response);
            }
            auto result = ft_engine_->pull_file(guest_path.c_str(), host_path.c_str());
            if (result) {
                auto& stats = ft_engine_->last_stats();
                response["return"]["bytes_transferred"] = stats.bytes_transferred;
                response["return"]["chunks_sent"] = stats.chunks_sent;
                response["return"]["status"] = "ok";
            } else {
                response["error"] = result.message;
            }
            return reply(response);
        }

        return reply({{"error", {{"code", "unsupported-command"}, {"message", "Unsupported command: " + execute}}}});

    } catch (const std::exception& e) {
        return reply({{"error", {{"code", "invalid-request"}, {"message", e.what()}}}});
    }
}

} // namespace domvirt::bindlet_bridge
