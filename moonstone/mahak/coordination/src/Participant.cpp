#include "mahak/coordination/Participant.hpp"

#include <chrono>
#include <optional>
#include <thread>
#include <unistd.h>

namespace moonstone::mahak::coordination {
namespace {

Error participant_error(std::string message, ErrorCategory category = ErrorCategory::Internal) {
    Error error;
    error.category = category;
    error.message = std::move(message);
    return error;
}

std::uint64_t now_ns() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

} // namespace

Participant::Participant(ParticipantOptions options, Mailbox inbox, Mailbox broker,
                         Blotter blotter)
    : options_(std::move(options)), inbox_(std::move(inbox)), broker_(std::move(broker)),
      blotter_(std::move(blotter)) {}

Participant::~Participant() { stop_heartbeat(); }

Participant::Participant(Participant &&other) noexcept
    : options_(std::move(other.options_)), inbox_(std::move(other.inbox_)),
      broker_(std::move(other.broker_)), blotter_(std::move(other.blotter_)),
      sequence_(other.sequence_), seen_limit_(other.seen_limit_),
      seen_order_(std::move(other.seen_order_)), seen_(std::move(other.seen_)),
      deferred_(std::move(other.deferred_)) {
    other.heartbeat_stop_.store(true);
    // The heartbeat thread owns a reference to `other`; moving a live
    // participant is therefore forbidden by construction in practice. We stop
    // it here to avoid a dangling thread. This is a defensive check, not a
    // supported concurrent-move feature.
    other.stop_heartbeat();
}

Participant &Participant::operator=(Participant &&other) noexcept {
    if (this != &other) {
        stop_heartbeat();
        options_ = std::move(other.options_);
        inbox_ = std::move(other.inbox_);
        broker_ = std::move(other.broker_);
        blotter_ = std::move(other.blotter_);
        sequence_ = other.sequence_;
        seen_limit_ = other.seen_limit_;
        seen_order_ = std::move(other.seen_order_);
        seen_ = std::move(other.seen_);
        deferred_ = std::move(other.deferred_);
        other.heartbeat_stop_.store(true);
        other.stop_heartbeat();
    }
    return *this;
}

const std::string &Participant::worker_id() const noexcept { return options_.worker_id; }
const std::string &Participant::session() const noexcept { return options_.session; }
Blotter &Participant::blotter() noexcept { return blotter_; }
const Blotter &Participant::blotter() const noexcept { return blotter_; }

Result<Participant> Participant::join(const ParticipantOptions &options) {
    if (!valid_session_id(options.session))
        return participant_error("invalid session id", ErrorCategory::InvalidInput);
    if (!valid_worker_id(options.worker_id))
        return participant_error("invalid worker id", ErrorCategory::InvalidInput);
    const std::string root = options.blotter_root.empty()
                                 ? blotter_root(options.session).value()
                                 : options.blotter_root;
    auto attached = Blotter::attach(root);
    if (!attached)
        return attached.error();

    auto inbox_name = mailbox_name(options.session, "w" + options.worker_id);
    if (!inbox_name)
        return inbox_name.error();
    std::optional<Mailbox> inbox;
    (void)Mailbox::unlink(inbox_name.value()); // stale queue from a failed session
    for (std::uint32_t attempt = 0; attempt < options.registration_attempts; ++attempt) {
        // Each worker owns (creates) its inbound queue. This keeps the
        // per-process accounting for the first slot local to the worker and
        // avoids one process exhausting the host's small mqueue budget.
        auto opened = Mailbox::open(inbox_name.value(),
                                    MailboxOptions{options.queue_depth, 0, 0600},
                                    MailboxOpen::Create);
        if (opened) {
            inbox = std::move(opened.value());
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!inbox)
        return participant_error("broker did not create worker mailbox " + inbox_name.value(),
                                 ErrorCategory::NotFound);

    auto broker_name = mailbox_name(options.session, "broker");
    if (!broker_name)
        return broker_name.error();
    std::optional<Mailbox> broker_queue;
    for (std::uint32_t attempt = 0; attempt < options.registration_attempts; ++attempt) {
        auto opened = Mailbox::open(broker_name.value(),
                                    MailboxOptions{options.queue_depth, 0, 0600},
                                    MailboxOpen::Attach);
        if (opened) {
            broker_queue = std::move(opened.value());
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!broker_queue)
        return participant_error("broker ingress mailbox is unavailable " + broker_name.value(),
                                 ErrorCategory::NotFound);

    Participant participant(options, std::move(inbox.value()), std::move(broker_queue.value()),
                            std::move(attached.value()));
    auto registered = participant.register_self();
    if (!registered)
        return registered.error();
    return participant;
}

Result<void> Participant::register_self() {
    Json payload = Json::object();
    payload.set("op", "register");
    payload.set("worker", options_.worker_id);
    payload.set("role", options_.role);
    payload.set("pid", static_cast<long long>(::getpid()));
    const std::string correlation = options_.worker_id + "-register";
    auto sent = send("broker", MessageType::Request, std::move(payload), correlation);
    if (!sent)
        return sent.error();
    std::deque<Envelope> stash;
    const std::uint64_t deadline = now_ns() + options_.request_timeout_ms * 1'000'000ULL;
    while (now_ns() < deadline) {
        const std::uint64_t remaining = (deadline - now_ns()) / 1'000'000ULL;
        auto envelope = receive_fresh(remaining == 0 ? 1 : remaining);
        if (!envelope) {
            if (envelope.error().category == ErrorCategory::BudgetExhausted)
                continue;
            for (auto &item : stash)
                deferred_.push_back(std::move(item));
            return envelope.error();
        }
        if (envelope.value().correlation_id == correlation ||
            envelope.value().type == MessageType::Error) {
            if (envelope.value().type == MessageType::Error) {
                const Json *message = envelope.value().payload.find("message");
                for (auto &item : stash)
                    deferred_.push_back(std::move(item));
                return participant_error(
                    message && message->as_string() ? *message->as_string()
                                                    : "registration rejected",
                    ErrorCategory::InvalidInput);
            }
            Json status = Json::object();
            status.set("worker", options_.worker_id);
            status.set("role", options_.role);
            status.set("state", "registered");
            status.set("pid", static_cast<long long>(::getpid()));
            (void)blotter_.update_worker(options_.worker_id, status);
            for (auto &item : stash)
                deferred_.push_back(std::move(item));
            return {};
        }
        if (envelope.value().type != MessageType::Ack &&
            envelope.value().type != MessageType::Status)
            (void)acknowledge(envelope.value(), "deferred-during-registration");
        stash.push_back(std::move(envelope.value()));
    }
    for (auto &item : stash)
        deferred_.push_back(std::move(item));
    return participant_error("registration timed out", ErrorCategory::BudgetExhausted);
}

std::uint64_t Participant::next_sequence() {
    return ++sequence_;
}

bool Participant::duplicate(const Envelope &envelope) {
    const auto key = std::make_pair(envelope.sender, envelope.sequence);
    if (seen_.count(key) != 0)
        return true;
    seen_[key] = true;
    seen_order_.push_back(key);
    while (seen_order_.size() > seen_limit_) {
        seen_.erase(seen_order_.front());
        seen_order_.pop_front();
    }
    return false;
}

Result<void> Participant::send(std::string_view recipient, MessageType type, Json payload,
                               std::string correlation, int priority,
                               std::uint64_t timeout_ms) {
    if (!payload.is_object())
        return participant_error("message payload must be a JSON object",
                                 ErrorCategory::InvalidInput);
    std::lock_guard<std::mutex> guard(send_mutex_);
    Envelope envelope;
    envelope.sequence = next_sequence();
    envelope.id = options_.worker_id + "-" + std::to_string(envelope.sequence);
    envelope.sender = options_.worker_id;
    envelope.recipient = std::string(recipient);
    envelope.correlation_id = std::move(correlation);
    envelope.type = type;
    envelope.priority = priority;
    envelope.timestamp_ns = now_ns();
    envelope.payload = std::move(payload);
    auto prepared = prepare_outbound(envelope);
    if (!prepared)
        return prepared.error();
    return broker_.send(envelope,
                        timeout_ms == 0 ? options_.send_timeout_ms : timeout_ms);
}

Result<void> Participant::prepare_outbound(Envelope &envelope) {
    auto probe = encode_envelope(envelope);
    if (!probe)
        return probe.error();
    if (probe.value().size() <= max_envelope_bytes - 32)
        return {};
    auto reference = blotter_.spool_payload(envelope.payload);
    if (!reference)
        return reference.error();
    Json pointer = Json::object();
    pointer.set("$spool", reference.value());
    envelope.payload = std::move(pointer);
    return {};
}

Result<void> Participant::resolve_inbound(Envelope &envelope) {
    const Json *reference = envelope.payload.find("$spool");
    if (!reference)
        return {};
    const auto *path = reference->as_string();
    if (!path)
        return participant_error("spool reference is not a string",
                                 ErrorCategory::InvalidInput);
    auto payload = blotter_.resolve_spool(*path);
    if (!payload)
        return payload.error();
    envelope.payload = std::move(payload.value());
    return {};
}

Result<Envelope> Participant::receive_fresh(std::uint64_t timeout_ms) {
    const std::uint64_t deadline = now_ns() + timeout_ms * 1'000'000ULL;
    for (;;) {
        const std::uint64_t now = now_ns();
        const std::uint64_t remaining = now < deadline ? (deadline - now) / 1'000'000ULL : 0;
        auto envelope = inbox_.receive(remaining);
        if (!envelope) {
            if (envelope.error().category == ErrorCategory::BudgetExhausted)
                return participant_error("receive timed out", ErrorCategory::BudgetExhausted);
            return envelope.error();
        }
        if (duplicate(envelope.value())) {
            if (envelope.value().type != MessageType::Ack &&
                envelope.value().type != MessageType::Status)
                (void)acknowledge(envelope.value(), "duplicate");
            if (now >= deadline)
                return participant_error("receive timed out", ErrorCategory::BudgetExhausted);
            continue;
        }
        auto resolved = resolve_inbound(envelope.value());
        if (!resolved)
            return resolved.error();
        return std::move(envelope.value());
    }
}

Result<Envelope> Participant::receive(std::uint64_t timeout_ms) {
    if (!deferred_.empty()) {
        Envelope envelope = std::move(deferred_.front());
        deferred_.pop_front();
        return envelope;
    }
    return receive_fresh(timeout_ms);
}

Result<Envelope> Participant::request(std::string_view recipient, Json payload,
                                      std::uint64_t timeout_ms) {
    const std::string correlation = options_.worker_id + "-req-" + std::to_string(now_ns());
    // Messages stashed by an earlier request (or by registration) are checked
    // once, then moved aside so this wait cannot rediscover the same envelope.
    std::deque<Envelope> stash;
    while (!deferred_.empty()) {
        Envelope current = std::move(deferred_.front());
        deferred_.pop_front();
        if (current.correlation_id == correlation)
            return current;
        stash.push_back(std::move(current));
    }
    auto sent = send(recipient, MessageType::Request, std::move(payload), correlation);
    if (!sent) {
        for (auto &item : stash)
            deferred_.push_back(std::move(item));
        return sent.error();
    }
    const std::uint64_t budget = timeout_ms == 0 ? options_.request_timeout_ms : timeout_ms;
    const std::uint64_t deadline = now_ns() + budget * 1'000'000ULL;
    for (;;) {
        const std::uint64_t now = now_ns();
        if (now >= deadline) {
            for (auto &item : stash)
                deferred_.push_back(std::move(item));
            return participant_error("request timed out", ErrorCategory::BudgetExhausted);
        }
        const std::uint64_t remaining = (deadline - now) / 1'000'000ULL;
        auto envelope = receive_fresh(remaining == 0 ? 1 : remaining);
        if (!envelope) {
            if (envelope.error().category == ErrorCategory::BudgetExhausted)
                continue;
            for (auto &item : stash)
                deferred_.push_back(std::move(item));
            return envelope.error();
        }
        if (envelope.value().correlation_id == correlation &&
            (envelope.value().type == MessageType::Response ||
             envelope.value().type == MessageType::Error)) {
            for (auto &item : stash)
                deferred_.push_back(std::move(item));
            return std::move(envelope.value());
        }
        if (envelope.value().type != MessageType::Ack &&
            envelope.value().type != MessageType::Status)
            (void)acknowledge(envelope.value(), "deferred-during-request");
        stash.push_back(std::move(envelope.value()));
    }
}

Result<void> Participant::acknowledge(const Envelope &envelope, std::string stage) {
    Json payload = Json::object();
    payload.set("op", "ack");
    payload.set("stage", std::move(stage));
    payload.set("ack_for", envelope.id);
    return send("broker", MessageType::Ack, std::move(payload), envelope.id, 25);
}

Result<Json> Participant::inspect_broker() {
    Json query = Json::object();
    query.set("op", "inspect");
    auto response = request("broker", std::move(query), 2000);
    if (!response)
        return response.error();
    return response.value().payload;
}

Result<void> Participant::heartbeat() {
    Json payload = Json::object();
    payload.set("op", "heartbeat");
    payload.set("state", "running");
    return send("broker", MessageType::Status, std::move(payload), "", 10, 1000);
}

Result<void> Participant::publish_status(std::string_view state, Json detail) {
    Json status = detail.is_object() ? detail : Json::object();
    status.set("worker", options_.worker_id);
    status.set("role", options_.role);
    status.set("state", std::string(state));
    status.set("updated_unix_ns", static_cast<unsigned long long>(
                                     std::chrono::duration_cast<std::chrono::nanoseconds>(
                                         std::chrono::system_clock::now().time_since_epoch())
                                         .count()));
    auto written = blotter_.update_worker(options_.worker_id, status);
    if (!written)
        return written.error();
    const std::string recipient = options_.worker_id == "1" ? "broker" : "1";
    Json payload = Json::object();
    payload.set("op", "status");
    payload.set("state", std::string(state));
    payload.set("detail", status);
    return send(recipient, MessageType::Status, std::move(payload), "", 5);
}

Result<void> Participant::leave(std::string reason) {
    stop_heartbeat();
    Json payload = Json::object();
    payload.set("op", "complete");
    payload.set("reason", std::move(reason));
    auto status = publish_status("complete", payload);
    (void)status; // A completed worker may leave after the broker is gone.
    return send("broker", MessageType::Complete, std::move(payload), "", 20);
}

Result<void> Participant::start_heartbeat(std::uint32_t interval_ms) {
    stop_heartbeat();
    heartbeat_stop_.store(false);
    heartbeat_thread_ = std::make_unique<std::thread>([this, interval_ms] {
        while (!heartbeat_stop_.load(std::memory_order_relaxed)) {
            (void)heartbeat();
            const std::uint32_t slices = interval_ms / 10 == 0 ? 1 : interval_ms / 10;
            for (std::uint32_t i = 0; i < slices && !heartbeat_stop_.load(); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    });
    return {};
}

void Participant::stop_heartbeat() noexcept {
    heartbeat_stop_.store(true);
    if (heartbeat_thread_ && heartbeat_thread_->joinable())
        heartbeat_thread_->join();
    heartbeat_thread_.reset();
}

} // namespace moonstone::mahak::coordination
