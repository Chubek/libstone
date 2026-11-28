#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "Protocol.hpp"

namespace moonstone::mahak::coordination {

enum class MailboxOpen { Create, Attach };

struct MailboxOptions {
    // The deployment host may account all message queues against a small
    // global budget. Defaults are deliberately one slot of 512 bytes; callers
    // that need more can only request what the kernel permits.
    std::uint32_t depth = 1;        // bounded; clamped to 1..64 and system msg_max
    std::uint32_t message_bytes = 0; // 0 selects max_envelope_bytes+16
    std::uint32_t mode = 0600;
};

// Thin, owning POSIX message queue. send/receive use mq_timedsend and
// mq_timedreceive (CLOCK_REALTIME) with an absolute deadline. A full/empty
// queue is backpressure, not a fatal error: send returns BudgetExhausted after
// the deadline. The class is movable and closes exactly once.
class Mailbox {
  public:
    static Result<Mailbox> open(std::string name, MailboxOptions options, MailboxOpen mode);
    static Result<void> unlink(std::string_view name);
    ~Mailbox();
    Mailbox(Mailbox &&) noexcept;
    Mailbox &operator=(Mailbox &&) noexcept;
    Mailbox(const Mailbox &) = delete;
    Mailbox &operator=(const Mailbox &) = delete;

    const std::string &name() const noexcept;
    Result<void> send(const Envelope &envelope, std::uint64_t timeout_ms);
    Result<Envelope> receive(std::uint64_t timeout_ms);
    Result<bool> try_send(const Envelope &envelope);
    Result<std::optional<Envelope>> try_receive();
    Result<std::uint32_t> pending() const;
    Result<void> close();

  private:
    Mailbox(std::string name, int fd);
    std::string name_;
    int fd_ = -1;
};

std::string describe_errno(int error);

} // namespace moonstone::mahak::coordination
