#include "mahak/coordination/Mailbox.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <mqueue.h>
#include <system_error>
#include <vector>

namespace moonstone::mahak::coordination {
namespace {

Error queue_error(std::string operation, int code) {
    Error error;
    if (code == EAGAIN || code == ETIMEDOUT) {
        error.category = ErrorCategory::BudgetExhausted;
        error.message = operation + ": queue operation timed out";
    } else if (code == EMSGSIZE || code == EINVAL) {
        error.category = ErrorCategory::InvalidInput;
        error.message = operation + ": invalid message or queue attribute";
    } else if (code == ENOENT) {
        error.category = ErrorCategory::NotFound;
        error.message = operation + ": mailbox does not exist";
    } else {
        error.category = ErrorCategory::Internal;
        error.message = operation + ": " + describe_errno(code);
    }
    return error;
}

struct Deadline {
    explicit Deadline(std::uint64_t timeout_ms) {
        clock_gettime(CLOCK_REALTIME, &value);
        const std::uint64_t extra_ns = timeout_ms * 1000000ULL;
        value.tv_sec += static_cast<time_t>(extra_ns / 1000000000ULL);
        value.tv_nsec += static_cast<long>(extra_ns % 1000000000ULL);
        if (value.tv_nsec >= 1000000000L) {
            value.tv_sec += 1;
            value.tv_nsec -= 1000000000L;
        }
    }
    timespec value{};
};

} // namespace

std::string describe_errno(int error) {
    return std::error_code(error, std::generic_category()).message();
}

Result<Mailbox> Mailbox::open(std::string name, MailboxOptions options, MailboxOpen mode) {
    if (name.empty() || name[0] != '/')
        return queue_error("mq_open", EINVAL);
    if (options.depth == 0)
        options.depth = 1;
    if (options.depth > 64)
        options.depth = 64;
    if (options.message_bytes == 0)
        options.message_bytes = static_cast<std::uint32_t>(max_envelope_bytes + 16);
    if (options.message_bytes > 65536)
        options.message_bytes = 65536;
    // Some Linux configurations account mq_maxmsg*mq_msgsize against a small
    // per-queue budget (observed EMFILE above ~16 KiB here). Keep the queue
    // usable by reducing depth rather than failing to create the queue.
    constexpr std::size_t queue_budget = 8192;
    if (static_cast<std::size_t>(options.depth) * options.message_bytes > queue_budget) {
        const std::size_t allowed = queue_budget / options.message_bytes;
        options.depth = static_cast<std::uint32_t>(std::max<std::size_t>(1, allowed));
    }

    mq_attr attributes{};
    attributes.mq_maxmsg = static_cast<long>(options.depth);
    attributes.mq_msgsize = static_cast<long>(options.message_bytes);
    attributes.mq_flags = 0;
    int flags = O_RDWR | O_CLOEXEC;
    if (mode == MailboxOpen::Create)
        flags |= O_CREAT | O_EXCL;
    const int fd = mq_open(name.c_str(), flags, static_cast<mode_t>(options.mode), &attributes);
    if (fd == -1)
        return queue_error("mq_open(" + name + ")", errno);
    return Mailbox(std::move(name), fd);
}

Result<void> Mailbox::unlink(std::string_view name) {
    if (name.empty() || name[0] != '/')
        return queue_error("mq_unlink", EINVAL);
    const std::string owned(name);
    if (mq_unlink(owned.c_str()) == -1 && errno != ENOENT)
        return queue_error("mq_unlink(" + owned + ")", errno);
    return {};
}

Mailbox::Mailbox(std::string name, int fd) : name_(std::move(name)), fd_(fd) {}

Mailbox::~Mailbox() { close(); }

Mailbox::Mailbox(Mailbox &&other) noexcept : name_(std::move(other.name_)), fd_(other.fd_) {
    other.fd_ = -1;
}

Mailbox &Mailbox::operator=(Mailbox &&other) noexcept {
    if (this != &other) {
        close();
        name_ = std::move(other.name_);
        fd_ = other.fd_;
        other.fd_ = -1;
    }
    return *this;
}

const std::string &Mailbox::name() const noexcept { return name_; }

Result<void> Mailbox::send(const Envelope &envelope, std::uint64_t timeout_ms) {
    if (fd_ < 0)
        return queue_error("mq_timedsend", EBADF);
    auto encoded = encode_envelope(envelope);
    if (!encoded)
        return encoded.error();
    const std::string &text = encoded.value();
    const Deadline deadline(timeout_ms);
    for (;;) {
        const int priority = envelope.priority < 0 ? 0 : (envelope.priority > 31 ? 31 : envelope.priority);
        const int result = mq_timedsend(fd_, text.data(), text.size(),
                                        static_cast<unsigned>(priority), &deadline.value);
        if (result == 0)
            return {};
        const int code = errno;
        if (code == EINTR)
            continue;
        return queue_error("mq_timedsend(" + name_ + ")", code);
    }
}

Result<Envelope> Mailbox::receive(std::uint64_t timeout_ms) {
    if (fd_ < 0)
        return queue_error("mq_timedreceive", EBADF);
    mq_attr attributes{};
    if (mq_getattr(fd_, &attributes) == -1)
        return queue_error("mq_getattr(" + name_ + ")", errno);
    std::vector<char> buffer(static_cast<std::size_t>(attributes.mq_msgsize) + 1);
    const Deadline deadline(timeout_ms);
    for (;;) {
        const ssize_t count = mq_timedreceive(fd_, buffer.data(), buffer.size(), nullptr,
                                              &deadline.value);
        if (count >= 0) {
            auto envelope = decode_envelope(std::string_view(buffer.data(),
                                                            static_cast<std::size_t>(count)));
            if (!envelope)
                return envelope.error();
            return std::move(envelope.value());
        }
        const int code = errno;
        if (code == EINTR)
            continue;
        return queue_error("mq_timedreceive(" + name_ + ")", code);
    }
}

Result<bool> Mailbox::try_send(const Envelope &envelope) {
    auto result = send(envelope, 0);
    if (result)
        return true;
    if (result.error().category == ErrorCategory::BudgetExhausted)
        return false;
    return result.error();
}

Result<std::optional<Envelope>> Mailbox::try_receive() {
    auto result = receive(0);
    if (result)
        return std::optional<Envelope>(std::move(result.value()));
    if (result.error().category == ErrorCategory::BudgetExhausted)
        return std::optional<Envelope>{};
    return result.error();
}

Result<std::uint32_t> Mailbox::pending() const {
    if (fd_ < 0)
        return queue_error("mq_getattr", EBADF);
    mq_attr attributes{};
    if (mq_getattr(fd_, &attributes) == -1)
        return queue_error("mq_getattr(" + name_ + ")", errno);
    return static_cast<std::uint32_t>(attributes.mq_curmsgs);
}

Result<void> Mailbox::close() {
    if (fd_ >= 0) {
        const int result = mq_close(fd_);
        fd_ = -1;
        if (result == -1)
            return queue_error("mq_close(" + name_ + ")", errno);
    }
    return {};
}

} // namespace moonstone::mahak::coordination
