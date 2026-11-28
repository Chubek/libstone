#include "mahak/coordination/Broker.hpp"

#include "mahak/coordination/Roster.hpp"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <optional>
#include <sys/stat.h>
#include <set>
#include <thread>
#include <unistd.h>

namespace moonstone::mahak::coordination {
namespace {

std::atomic<bool> signal_stop{false};

void on_signal(int) { signal_stop.store(true, std::memory_order_relaxed); }

std::uint64_t now_ns() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

Error broker_error(std::string message, ErrorCategory category = ErrorCategory::Internal) {
    Error error;
    error.category = category;
    error.message = std::move(message);
    return error;
}

struct PendingDelivery {
    std::string sender;
    std::string recipient;
    std::uint64_t sent_ns = 0;
};

struct RequestTrack {
    std::string sender;
    std::string recipient;
    std::uint64_t sent_ns = 0;
    std::string correlation;
};

class BrokerImpl {
  public:
    explicit BrokerImpl(BrokerOptions options)
        : options_(std::move(options)) {}

    Result<BrokerStats> run(const std::atomic<bool> *external_stop) {
        auto ingress_name = mailbox_name(options_.session, "broker");
        if (!ingress_name)
            return ingress_name.error();
        for (const auto &worker : options_.workers) {
            if (!valid_worker_id(worker))
                return broker_error("invalid worker id: " + worker, ErrorCategory::InvalidInput);
        }
        auto root = options_.blotter_root.empty()
                        ? blotter_root(options_.session).value()
                        : options_.blotter_root;
        auto attached = Blotter::attach(root);
        if (!attached) {
            auto created = Blotter::create(options_.session, options_.workers);
            if (!created)
                return created.error();
            attached = std::move(created);
        }
        blotter_ = std::make_unique<Blotter>(std::move(attached.value()));

        auto ingress = Mailbox::open(ingress_name.value(),
                                     MailboxOptions{options_.queue_depth, 0, 0600},
                                     MailboxOpen::Create);
        if (!ingress) {
            (void)Mailbox::unlink(ingress_name.value());
            ingress = Mailbox::open(ingress_name.value(),
                                    MailboxOptions{options_.queue_depth, 0, 0600},
                                    MailboxOpen::Create);
        }
        if (!ingress)
            return ingress.error();
        ingress_ = std::make_unique<Mailbox>(std::move(ingress.value()));

        // Worker inbound queues are created by the workers themselves. The
        // broker attaches lazily when routing; this avoids accounting every
        // queue's capacity to one process on hosts with a tiny mqueue budget.
        if (!options_.roster_root.empty()) {
            auto loaded = load_roster(options_.roster_root);
            if (!loaded)
                return loaded.error();
            if (loaded.value().session != options_.session)
                return broker_error("roster session '" + loaded.value().session +
                                        "' does not match broker session '" + options_.session +
                                        "'",
                                    ErrorCategory::InvalidInput);
            roster_ = std::move(loaded.value());
            Json data = Json::object();
            data.set("revision", static_cast<std::int64_t>(roster_->revision));
            data.set("root", roster_->root);
            data.set("workers", static_cast<std::int64_t>(roster_->entries.size()));
            blink_("roster-loaded", std::move(data));
        }
        blink_("broker-start", Json::object());
        Json broker_status = Json::object();
        broker_status.set("state", "running");
        broker_status.set("role", "broker");
        broker_status.set("pid", static_cast<long long>(::getpid()));
        (void)blotter_->update_worker("2", broker_status);

        struct sigaction action {};
        action.sa_handler = on_signal;
        sigemptyset(&action.sa_mask);
        sigaction(SIGINT, &action, nullptr);
        sigaction(SIGTERM, &action, nullptr);
        signal_stop.store(false, std::memory_order_relaxed);

        std::uint64_t last_housekeeping = 0;
        while (!shutdown_ && !(external_stop && external_stop->load()) &&
               !signal_stop.load(std::memory_order_relaxed)) {
            auto envelope = ingress_->receive(100);
            if (envelope) {
                ++stats_.received;
                if (options_.verbose && stats_.received % 5000 == 0) {
                    const Json *op = envelope.value().payload.find("op");
                    std::fprintf(stderr, "broker received=%llu sender=%s recipient=%s type=%s op=%s\n",
                                 static_cast<unsigned long long>(stats_.received),
                                 envelope.value().sender.c_str(),
                                 envelope.value().recipient.c_str(),
                                 name(envelope.value().type),
                                 op && op->as_string() ? op->as_string()->c_str() : "");
                }
                handle(envelope.value());
            } else if (envelope.error().category != ErrorCategory::BudgetExhausted) {
                journal_error("broker-receive", envelope.error().message);
            }
            const std::uint64_t now = now_ns();
            if (now - last_housekeeping > 100'000'000ULL) {
                (void)reload_roster_if_changed();
                housekeeping(now);
                last_housekeeping = now;
            }
        }

        orderly_shutdown();
        return stats_;
    }

  private:
    BrokerOptions options_;
    std::unique_ptr<Blotter> blotter_;
    std::unique_ptr<Mailbox> ingress_;
    std::optional<Roster> roster_;
    std::uint64_t roster_mtime_ns_ = 0;
    std::map<std::string, Mailbox> mailboxes_;
    std::set<std::string> registered_;
    std::set<std::string> unavailable_;
    std::map<std::string, std::uint64_t> last_seen_;
    std::map<std::string, bool> blocked_;
    std::map<std::string, std::string> blocked_reason_;
    std::map<std::string, PendingDelivery> pending_;
    std::map<std::string, RequestTrack> requests_;
    std::set<std::pair<std::string, std::uint64_t>> seen_;
    std::deque<std::pair<std::string, std::uint64_t>> seen_order_;
    std::uint64_t broker_sequence_ = 0;
    std::uint64_t last_deadlock_report_ = 0;
    BrokerStats stats_;
    bool shutdown_ = false;

    void journal_error(std::string_view kind, std::string_view message) {
        if (!blotter_)
            return;
        Json data = Json::object();
        data.set("message", std::string(message));
        (void)blotter_->record_event("broker", kind, data);
    }

    void blink_(std::string_view kind, Json data) {
        if (blotter_)
            (void)blotter_->record_event("broker", kind, data);
    }

    // Comrade-name routing. The roster is re-read when the file changes so a
    // revised roster (revision + 1) takes effect without restarting the broker.
    bool reload_roster_if_changed() {
        if (options_.roster_root.empty())
            return false;
        struct stat status {};
        const std::string path = roster_path(options_.roster_root);
        if (::stat(path.c_str(), &status) != 0)
            return false;
        const auto mtime = static_cast<std::uint64_t>(status.st_mtime) * 1000000000ull +
                           static_cast<std::uint64_t>(status.st_mtim.tv_nsec);
        if (mtime == roster_mtime_ns_)
            return false;
        auto loaded = try_load_roster(options_.roster_root);
        if (!loaded || !loaded.value())
            return false;
        if (loaded.value()->session != options_.session) {
            journal_error("roster-session-mismatch", loaded.value()->session);
            return false;
        }
        roster_mtime_ns_ = mtime;
        if (roster_ && roster_->revision == loaded.value()->revision)
            return false;
        roster_ = std::move(loaded.value());
        Json data = Json::object();
        data.set("revision", static_cast<std::int64_t>(roster_->revision));
        data.set("workers", static_cast<std::int64_t>(roster_->entries.size()));
        blink_("roster-revision", std::move(data));
        return true;
    }

    // Rewrites a comrade name to its numeric protocol id. Returns false only
    // when a roster is published and the name is not in it.
    bool resolve_comrade(Envelope &envelope) {
        if (options_.roster_root.empty())
            return true;
        if (envelope.recipient == "broker" || envelope.recipient == "*")
            return true;
        if (roster_ && roster_->lookup(envelope.recipient) != nullptr) {
            const RosterEntry *entry = roster_->lookup(envelope.recipient);
            if (entry->name != envelope.recipient)
                return true; // already a numeric id known to the roster
            const std::string id = std::to_string(entry->protocol_id);
            Json data = Json::object();
            data.set("message_id", envelope.id);
            data.set("from", envelope.sender);
            data.set("comrade", entry->name);
            data.set("protocol_id", static_cast<std::int64_t>(entry->protocol_id));
            blink_("named-route", std::move(data));
            envelope.recipient = id;
            return true;
        }
        if (reload_roster_if_changed() && roster_ &&
            roster_->lookup(envelope.recipient) != nullptr)
            return resolve_comrade(envelope);
        if (roster_)
            return false;
        return true;
    }

    std::string next_id() {
        return "broker-" + std::to_string(++broker_sequence_);
    }

    bool route(const std::string &recipient, Envelope envelope, std::string *why) {
        auto found = mailboxes_.find(recipient);
        if (found == mailboxes_.end()) {
            auto name = mailbox_name(options_.session, "w" + recipient);
            if (!name) {
                if (why)
                    *why = name.error().message;
                return false;
            }
            std::optional<Mailbox> attached;
            for (int attempt = 0; attempt < 100; ++attempt) {
                auto opened = Mailbox::open(name.value(), MailboxOptions{1, 0, 0600},
                                            MailboxOpen::Attach);
                if (opened) {
                    attached = std::move(opened.value());
                    break;
                }
                if (opened.error().category == ErrorCategory::NotFound)
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                else
                    break;
            }
            if (!attached) {
                if (why)
                    *why = "unknown recipient: " + recipient;
                return false;
            }
            found = mailboxes_.emplace(recipient, std::move(attached.value())).first;
        }
        auto prepared = prepare_outbound(envelope);
        if (!prepared) {
            if (why)
                *why = prepared.error().message;
            return false;
        }
        if (unavailable_.count(recipient) != 0) {
            if (why)
                *why = "recipient unavailable: " + recipient;
            return false;
        }
        for (std::uint32_t attempt = 0; attempt <= options_.delivery_retries; ++attempt) {
            auto sent = found->second.try_send(envelope);
            if (sent && sent.value())
                return true;
            if (sent && !sent.value()) {
                ++stats_.backpressure;
                blocked_[recipient] = true;
                blocked_reason_[recipient] = "mailbox full";
                std::this_thread::sleep_for(std::chrono::milliseconds(2 + attempt * 2));
                continue;
            }
            if (why)
                *why = sent.error().message;
            return false;
        }
        if (why)
            *why = "recipient mailbox remained full after bounded retries";
        return false;
    }

    Result<void> prepare_outbound(Envelope &envelope) {
        auto probe = encode_envelope(envelope);
        if (!probe)
            return probe.error();
        if (probe.value().size() <= max_envelope_bytes - 32)
            return {};
        auto reference = blotter_->spool_payload(envelope.payload);
        if (!reference)
            return reference.error();
        Json pointer = Json::object();
        pointer.set("$spool", reference.value());
        envelope.payload = std::move(pointer);
        return {};
    }

    Result<void> resolve_inbound(Envelope &envelope) {
        const Json *reference = envelope.payload.find("$spool");
        if (!reference)
            return {};
        const auto *path = reference->as_string();
        if (!path)
            return broker_error("spool reference is not a string",
                                ErrorCategory::InvalidInput);
        auto payload = blotter_->resolve_spool(*path);
        if (!payload)
            return payload.error();
        envelope.payload = std::move(payload.value());
        return {};
    }

    void send_to(const std::string &recipient, MessageType type, std::string correlation,
                 Json payload, int priority = 0) {
        Envelope envelope;
        envelope.id = next_id();
        envelope.sender = "broker";
        envelope.recipient = recipient;
        envelope.sequence = broker_sequence_;
        envelope.correlation_id = std::move(correlation);
        envelope.type = type;
        envelope.priority = priority;
        envelope.timestamp_ns = now_ns();
        envelope.payload = std::move(payload);
        std::string why;
        if (!route(recipient, std::move(envelope), &why)) {
            ++stats_.errors_sent;
            journal_error("broker-delivery-failure", recipient + ": " + why);
        }
    }

    void send_error(const std::string &recipient, const std::string &message,
                    std::string correlation = {}) {
        Json payload = Json::object();
        payload.set("message", message);
        payload.set("error", true);
        send_to(recipient, MessageType::Error, std::move(correlation), std::move(payload), 30);
        ++stats_.errors_sent;
    }

    void send_ack(const std::string &recipient, const std::string &message_id,
                  std::string stage) {
        Json payload = Json::object();
        payload.set("stage", std::move(stage));
        payload.set("ack_for", message_id);
        // Delivery acknowledgements are lower priority than responses: a
        // response must not be starved by a queue full of ACKs.
        send_to(recipient, MessageType::Ack, message_id, std::move(payload), 1);
    }

    void remember(const Envelope &envelope) {
        seen_.emplace(envelope.sender, envelope.sequence);
        seen_order_.emplace_back(envelope.sender, envelope.sequence);
        while (seen_order_.size() > 8192) {
            seen_.erase(seen_order_.front());
            seen_order_.pop_front();
        }
    }

    void update_seen(const std::string &worker, std::uint64_t now) {
        last_seen_[worker] = now;
        unavailable_.erase(worker);
        blocked_.erase(worker);
        blocked_reason_.erase(worker);
    }

    Json inspect_snapshot() const {
        Json result = Json::object();
        Json workers = Json::array();
        for (const auto &worker : registered_) {
            Json item = Json::object();
            item.set("id", worker);
            item.set("available", unavailable_.count(worker) == 0);
            item.set("blocked", blocked_.count(worker) != 0);
            if (const auto found = blocked_reason_.find(worker); found != blocked_reason_.end())
                item.set("blocked_reason", found->second);
            auto seen = last_seen_.find(worker);
            item.set("last_seen_ns", static_cast<unsigned long long>(
                                         seen == last_seen_.end() ? 0 : seen->second));
            workers.push(std::move(item));
        }
        result.set("workers", std::move(workers));
        result.set("pending_deliveries", static_cast<unsigned long long>(pending_.size()));
        result.set("pending_requests", static_cast<unsigned long long>(requests_.size()));
        Json stats = Json::object();
        stats.set("received", static_cast<unsigned long long>(stats_.received));
        stats.set("routed", static_cast<unsigned long long>(stats_.routed));
        stats.set("duplicates", static_cast<unsigned long long>(stats_.duplicates));
        stats.set("rejected", static_cast<unsigned long long>(stats_.rejected));
        stats.set("backpressure", static_cast<unsigned long long>(stats_.backpressure));
        stats.set("timeouts", static_cast<unsigned long long>(stats_.timeouts));
        stats.set("heartbeat_failures",
                  static_cast<unsigned long long>(stats_.heartbeat_failures));
        result.set("stats", std::move(stats));
        return result;
    }

    bool local_operation(const Envelope &envelope) {
        const Json *operation = envelope.payload.find("op");
        if (!operation || !operation->as_string())
            return false;
        const std::string &op = *operation->as_string();
        if (op == "register") {
            const Json *worker = envelope.payload.find("worker");
            const Json *role = envelope.payload.find("role");
            std::string id = worker && worker->as_string() ? *worker->as_string()
                                                           : envelope.sender;
            if (id != envelope.sender) {
                send_error(envelope.sender, "register worker id does not match envelope sender",
                           envelope.id);
                return true;
            }
            registered_.insert(id);
            update_seen(id, now_ns());
            Json status = Json::object();
            status.set("state", "registered");
            status.set("role", role && role->as_string() ? *role->as_string() : "");
            status.set("pid", static_cast<long long>(::getpid()));
            if (blotter_)
                (void)blotter_->update_worker(id, status);
            Json response = Json::object();
            response.set("ok", true);
            response.set("worker", id);
            response.set("registered", static_cast<unsigned long long>(registered_.size()));
            send_to(envelope.sender, MessageType::Response,
                    envelope.correlation_id.empty() ? envelope.id : envelope.correlation_id,
                    std::move(response), 10);
            blink_("worker-registered", status);
            return true;
        }
        if (op == "heartbeat") {
            update_seen(envelope.sender, now_ns());
            if (blotter_) {
                Json status = Json::object();
                status.set("state", "running");
                status.set("heartbeat", true);
                (void)blotter_->update_worker(envelope.sender, status);
            }
            // Heartbeats are liveness evidence, not a routed work item. An ACK
            // per heartbeat would fill a one/two-slot inbox on this host.
            return true;
        }
        if (op == "inspect") {
            send_to(envelope.sender, MessageType::Response,
                    envelope.correlation_id.empty() ? envelope.id : envelope.correlation_id,
                    inspect_snapshot(), 10);
            return true;
        }
        if (op == "shutdown") {
            Json response = Json::object();
            response.set("ok", true);
            response.set("message", "orderly shutdown requested");
            send_to(envelope.sender, MessageType::Response,
                    envelope.correlation_id.empty() ? envelope.id : envelope.correlation_id,
                    std::move(response), 10);
            shutdown_ = true;
            blink_("shutdown-requested", envelope.payload);
            return true;
        }
        if (op == "verdict") {
            if (blotter_) {
                const Json *id = envelope.payload.find("id");
                (void)blotter_->record_verdict(
                    id && id->as_string() ? *id->as_string() : envelope.id,
                    envelope.payload);
            }
            send_ack(envelope.sender, envelope.id, "verdict-recorded");
            return true;
        }
        return false;
    }

    void handle(Envelope envelope) {
        update_seen(envelope.sender, now_ns());
        if (seen_.count({envelope.sender, envelope.sequence}) != 0) {
            ++stats_.duplicates;
            send_ack(envelope.sender, envelope.id, "broker-duplicate");
            return;
        }
        remember(envelope);

        if (!resolve_comrade(envelope)) {
            ++stats_.rejected;
            send_error(envelope.sender, "unknown comrade '" + envelope.recipient + "'",
                       envelope.id);
            return;
        }

        Envelope effective = envelope;
        const bool resolved = static_cast<bool>(resolve_inbound(effective));
        const Json *operation = effective.payload.find("op");
        const std::string op = operation && operation->as_string() ? *operation->as_string() : "";

        if (envelope.recipient == "broker") {
            if (op == "ack") {
                handle_ack(effective);
                return;
            }
            if (resolved && local_operation(effective))
                return;
            // Forwarding a message addressed to the broker is not a valid
            // generic operation. Report it instead of silently dropping it.
            ++stats_.rejected;
            send_error(envelope.sender, "unsupported broker operation: " + op, envelope.id);
            return;
        }

        if (envelope.recipient == "*") {
            std::uint64_t delivered = 0;
            for (const auto &worker : registered_) {
                if (worker == envelope.sender)
                    continue;
                Envelope copy = envelope;
                copy.recipient = worker;
                std::string why;
                if (route(worker, copy, &why)) {
                    ++delivered;
                    ++stats_.routed;
                } else {
                    ++stats_.errors_sent;
                    journal_error("broadcast-failure", worker + ": " + why);
                }
            }
            Json payload = Json::object();
            payload.set("stage", "broker-broadcast");
            payload.set("delivered", static_cast<unsigned long long>(delivered));
            send_to(envelope.sender, MessageType::Ack, envelope.id, std::move(payload), 25);
            return;
        }

        if (envelope.type == MessageType::Request) {
            const std::string key = envelope.correlation_id.empty() ? envelope.id
                                                                   : envelope.correlation_id;
            requests_[key] = RequestTrack{envelope.sender, envelope.recipient, now_ns(), key};
        }
        if (envelope.type == MessageType::Response) {
            const auto found = requests_.find(envelope.correlation_id);
            if (found != requests_.end())
                requests_.erase(found);
        }

        std::string why;
        Envelope forwarded = envelope;
        forwarded.id = envelope.id;
        if (!route(envelope.recipient, std::move(forwarded), &why)) {
            ++stats_.rejected;
            send_error(envelope.sender, "delivery failed: " + why, envelope.id);
            if (envelope.type == MessageType::Request) {
                Json blocked = Json::object();
                blocked.set("op", "blocked");
                blocked.set("reason", why);
                blocked.set("target", envelope.recipient);
                // Notify the orchestrator through the broker's own error path.
                send_to("1", MessageType::Blocked, envelope.id, std::move(blocked), 20);
            }
            return;
        }
        ++stats_.routed;
        pending_[envelope.id] = PendingDelivery{envelope.sender, envelope.recipient, now_ns()};
        send_ack(envelope.sender, envelope.id, "broker-accepted");
        if (envelope.type == MessageType::Status && blotter_) {
            Json data = envelope.payload;
            data.set("from", envelope.sender);
            data.set("to", envelope.recipient);
            (void)blotter_->record_event(envelope.sender, "status-routed", data);
        }
    }

    void handle_ack(const Envelope &envelope) {
        const Json *stage = envelope.payload.find("stage");
        const Json *ack_for = envelope.payload.find("ack_for");
        const std::string message_id = ack_for && ack_for->as_string()
                                           ? *ack_for->as_string()
                                           : envelope.correlation_id;
        (void)stage;
        const auto found = pending_.find(message_id);
        if (options_.verbose && stats_.received < 30)
            std::fprintf(stderr, "ack sender=%s ack_for=%s found=%d pending=%zu\n",
                         envelope.sender.c_str(), message_id.c_str(),
                         found != pending_.end() ? 1 : 0, pending_.size());
        if (found == pending_.end())
            return;
        PendingDelivery delivery = found->second;
        pending_.erase(found);
        if (delivery.sender == "broker" || delivery.sender.empty())
            return;
        // Forward the delivery acknowledgement to the original sender. The
        // receiver's ACK is itself an application-level fact, not a new request.
        Json payload = Json::object();
        payload.set("stage", "delivery");
        payload.set("ack_for", message_id);
        payload.set("from", envelope.sender);
        send_to(delivery.sender, MessageType::Ack, message_id, std::move(payload), 2);
    }

    void housekeeping(std::uint64_t now) {
        for (const auto &worker : registered_) {
            if (worker == "2")
                continue;
            const auto seen = last_seen_.find(worker);
            const std::uint64_t last = seen == last_seen_.end() ? 0 : seen->second;
            if (last != 0 && now - last > static_cast<std::uint64_t>(options_.heartbeat_timeout_ms) * 1'000'000ULL &&
                unavailable_.count(worker) == 0) {
                unavailable_.insert(worker);
                ++stats_.heartbeat_failures;
                Json status = Json::object();
                status.set("state", "unresponsive");
                status.set("last_seen_ns", static_cast<unsigned long long>(last));
                if (blotter_)
                    (void)blotter_->update_worker(worker, status);
                Json notice = Json::object();
                notice.set("op", "worker-unavailable");
                notice.set("worker", worker);
                notice.set("last_seen_ns", static_cast<unsigned long long>(last));
                send_to("1", MessageType::Error, "", std::move(notice), 28);
                blink_("worker-unavailable", status);
            }
        }
        for (auto it = pending_.begin(); it != pending_.end();) {
            if (now - it->second.sent_ns >
                static_cast<std::uint64_t>(options_.request_timeout_ms) * 1'000'000ULL) {
                ++stats_.timeouts;
                send_error(it->second.sender,
                           "delivery timeout for " + it->second.recipient, it->first);
                it = pending_.erase(it);
            } else {
                ++it;
            }
        }
        if (!requests_.empty() && registered_.size() > 1 && now - last_deadlock_report_ > 5'000'000'000ULL) {
            std::size_t stalled = 0;
            for (const auto &[id, request] : requests_) {
                (void)id;
                if (unavailable_.count(request.recipient) != 0 ||
                    blocked_.count(request.recipient) != 0)
                    ++stalled;
            }
            if (stalled == requests_.size()) {
                last_deadlock_report_ = now;
                Json diagnostic = Json::object();
                diagnostic.set("op", "deadlock-suspected");
                diagnostic.set("pending_requests",
                               static_cast<unsigned long long>(requests_.size()));
                diagnostic.set("note", "all outstanding recipients are blocked or unavailable");
                send_to("1", MessageType::Error, "", std::move(diagnostic), 30);
                blink_("deadlock-diagnostic", diagnostic);
            }
        }
    }

    void orderly_shutdown() {
        Json payload = Json::object();
        payload.set("op", "shutdown");
        payload.set("reason", "broker shutting down");
        for (const auto &worker : registered_) {
            if (worker == "2")
                continue;
            send_to(worker, MessageType::Complete, "", payload, 31);
        }
        const std::uint64_t deadline =
            now_ns() + static_cast<std::uint64_t>(options_.shutdown_grace_ms) * 1'000'000ULL;
        while (now_ns() < deadline) {
            auto envelope = ingress_->receive(20);
            if (envelope) {
                ++stats_.received;
                // Drain acknowledgements without starting new work.
                if (envelope.value().payload.find("op") &&
                    envelope.value().payload.find("op")->as_string() &&
                    *envelope.value().payload.find("op")->as_string() == "ack")
                    handle_ack(envelope.value());
            } else if (envelope.error().category != ErrorCategory::BudgetExhausted) {
                break;
            }
        }
        Json final = Json::object();
        final.set("received", static_cast<unsigned long long>(stats_.received));
        final.set("routed", static_cast<unsigned long long>(stats_.routed));
        final.set("duplicates", static_cast<unsigned long long>(stats_.duplicates));
        final.set("rejected", static_cast<unsigned long long>(stats_.rejected));
        final.set("backpressure", static_cast<unsigned long long>(stats_.backpressure));
        final.set("timeouts", static_cast<unsigned long long>(stats_.timeouts));
        final.set("heartbeat_failures", static_cast<unsigned long long>(stats_.heartbeat_failures));
        if (blotter_) {
            (void)blotter_->record_event("broker", "broker-stop", final);
            Json status = Json::object();
            status.set("state", "stopped");
            status.set("role", "broker");
            (void)blotter_->update_worker("2", status);
        }
        for (const auto &worker : registered_)
            blocked_[worker] = false;
        for (const auto &worker : options_.workers) {
            if (auto name = mailbox_name(options_.session, "w" + worker))
                (void)Mailbox::unlink(name.value());
        }
        if (ingress_) {
            (void)Mailbox::unlink(ingress_->name());
            ingress_.reset();
        }
    }
};

} // namespace

Result<BrokerStats> run_broker(const BrokerOptions &options, const std::atomic<bool> *stop) {
    BrokerImpl broker(options);
    return broker.run(stop);
}

} // namespace moonstone::mahak::coordination
