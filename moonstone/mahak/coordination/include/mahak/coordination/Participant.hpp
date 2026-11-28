#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "Blotter.hpp"
#include "Mailbox.hpp"
#include "Protocol.hpp"

namespace moonstone::mahak::coordination {

struct ParticipantOptions {
    std::string session;
    std::string worker_id;
    std::string role;
    std::string blotter_root; // empty selects /dev/shm/mahak-<session>
    std::uint32_t queue_depth = 1;
    std::uint64_t send_timeout_ms = 5000;
    std::uint64_t request_timeout_ms = 15000;
    std::uint32_t registration_attempts = 100;
};

// Worker-side endpoint. It owns its inbound mailbox, sends every outbound
// message to the broker, deduplicates inbound (sender,sequence) pairs, and
// acknowledges delivery. It does not talk to another worker directly.
class Participant {
  public:
    static Result<Participant> join(const ParticipantOptions &options);
    ~Participant();
    Participant(Participant &&) noexcept;
    Participant &operator=(Participant &&) noexcept;
    Participant(const Participant &) = delete;
    Participant &operator=(const Participant &) = delete;

    const std::string &worker_id() const noexcept;
    const std::string &session() const noexcept;
    Blotter &blotter() noexcept;
    const Blotter &blotter() const noexcept;

    Result<void> send(std::string_view recipient, MessageType type, Json payload,
                      std::string correlation = {}, int priority = 0,
                      std::uint64_t timeout_ms = 0);
    Result<Envelope> receive(std::uint64_t timeout_ms);
    Result<Envelope> request(std::string_view recipient, Json payload,
                             std::uint64_t timeout_ms = 0);
    Result<void> acknowledge(const Envelope &envelope, std::string stage);
    Result<Json> inspect_broker();
    Result<void> heartbeat();
    Result<void> publish_status(std::string_view state, Json detail);
    Result<void> leave(std::string reason);

    // Optional background heartbeat thread. stop() joins it; calling stop()
    // twice is safe. The thread sends a STATUS heartbeat at the interval.
    Result<void> start_heartbeat(std::uint32_t interval_ms);
    void stop_heartbeat() noexcept;

  private:
    Participant(ParticipantOptions options, Mailbox inbox, Mailbox broker, Blotter blotter);
    Result<void> register_self();
    Result<void> prepare_outbound(Envelope &envelope);
    Result<void> resolve_inbound(Envelope &envelope);
    Result<Envelope> receive_fresh(std::uint64_t timeout_ms);
    bool duplicate(const Envelope &envelope);
    std::uint64_t next_sequence();
    ParticipantOptions options_;
    Mailbox inbox_;
    Mailbox broker_;
    Blotter blotter_;
    std::mutex send_mutex_;
    std::uint64_t sequence_ = 0;
    std::size_t seen_limit_ = 8192;
    std::deque<std::pair<std::string, std::uint64_t>> seen_order_;
    std::map<std::pair<std::string, std::uint64_t>, bool> seen_;
    std::deque<Envelope> deferred_;
    std::atomic<bool> heartbeat_stop_{false};
    std::unique_ptr<std::thread> heartbeat_thread_;
};

} // namespace moonstone::mahak::coordination
