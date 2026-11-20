#pragma once
#include <string>
#include <functional>
#include <thread>
#include <atomic>
#include <mutex>
#include "domvirt_types.hpp"

namespace domvirt::bindlet_bridge {

class FileTransferEngine;

using ControlCommandHandler = std::function<std::string(const std::string&)>;

class ControlSocket {
public:
    ControlSocket();
    ~ControlSocket();

    ControlSocket(const ControlSocket&) = delete;
    ControlSocket& operator=(const ControlSocket&) = delete;

    VmResult start(const std::string& socket_path);
    void stop();

    void register_handler(const std::string& command, ControlCommandHandler handler);
    void set_file_transfer_engine(FileTransferEngine* engine);

    bool is_running() const { return running_.load(); }

private:
    void server_loop();
    std::string process_request(const std::string& request);

    std::string socket_path_;
    int listen_fd_{-1};
    int event_fd_{-1};
    std::thread server_thread_;
    std::atomic<bool> running_{false};
    std::mutex handlers_mutex_;
    std::vector<std::pair<std::string, ControlCommandHandler>> handlers_;
    FileTransferEngine* ft_engine_{nullptr};
};

} // namespace domvirt::bindlet_bridge
