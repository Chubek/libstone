#include "mahak/coordination/Blotter.hpp"
#include "mahak/coordination/Broker.hpp"
#include "mahak/coordination/Participant.hpp"

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
#include <sys/wait.h>
#include <unistd.h>

using namespace moonstone::mahak::coordination;
using moonstone::mahak::Json;

namespace {

std::uint64_t now_ns() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

void sleep_ms(unsigned value) { std::this_thread::sleep_for(std::chrono::milliseconds(value)); }

bool queue_exists(const std::string &session) {
    auto name = mailbox_name(session, "broker");
    if (!name)
        return false;
    auto opened = Mailbox::open(name.value(), MailboxOptions{8, 0, 0600}, MailboxOpen::Attach);
    if (!opened)
        return false;
    (void)opened.value().close();
    return true;
}

Json contract_document() {
    Json contract = Json::object();
    contract.set("name", "mahak.api.v1");
    contract.set("version", 1);
    contract.set("owner", "worker-1");
    contract.set("interfaces", Json::array());
    Json interface = Json::object();
    interface.set("name", "BenchmarkManifest");
    interface.set("stability", "frozen");
    interface.set("fields", Json::array());
    interface.find("fields")->push("schema");
    interface.find("fields")->push("targets");
    interface.find("fields")->push("benchmarks");
    contract.find("interfaces")->push(std::move(interface));
    return contract;
}

int run_implementer(const std::string &session, const std::string &root,
                    const std::string &id) {
    ParticipantOptions options;
    options.session = session;
    options.worker_id = id;
    options.role = "implementation";
    options.blotter_root = root;
    auto joined = Participant::join(options);
    if (!joined) {
        std::cerr << "worker " << id << " join failed: " << joined.error().message << "\n";
        return 1;
    }
    Participant participant = std::move(joined.value());
    (void)participant.start_heartbeat(300);
    Json detail = Json::object();
    detail.set("phase", "starting");
    (void)participant.publish_status("working", detail);

    const std::uint64_t contract_deadline = now_ns() + 8'000'000'000ULL;
    bool contracted = false;
    while (now_ns() < contract_deadline && !contracted) {
        auto envelope = participant.receive(300);
        if (!envelope)
            continue;
        if (envelope.value().type != MessageType::Ack &&
            envelope.value().type != MessageType::Status)
            (void)participant.acknowledge(envelope.value(), "implementer-contract");
        if (envelope.value().type == MessageType::Contract) {
            contracted = true;
        } else if (envelope.value().type == MessageType::Complete) {
            (void)participant.leave("complete-before-contract");
            return 0;
        }
    }
    if (!contracted)
        return 1;

    Json working = Json::object();
    working.set("phase", "implementing");
    (void)participant.publish_status("implementing", working);
    Json status = Json::object();
    status.set("op", "status");
    status.set("state", "implementing");
    status.set("progress", 0.5);
    (void)participant.send("1", MessageType::Status, status, "", 5);

    auto task = participant.request("1", Json::object(), 5000);
    if (task) {
        (void)participant.acknowledge(task.value(), "implementer-task");
    }

    if (id == "5") {
        Json blocked = Json::object();
        blocked.set("op", "blocked");
        blocked.set("reason", "contract ambiguity: BenchmarkManifest.timeout");
        blocked.set("target", "1");
        (void)participant.send("1", MessageType::Blocked, blocked, id + "-block", 20);
        const std::uint64_t deadline = now_ns() + 5'000'000'000ULL;
        while (now_ns() < deadline) {
            auto resolution = participant.receive(300);
            if (!resolution)
                continue;
            if (resolution.value().type != MessageType::Ack &&
                resolution.value().type != MessageType::Status)
                (void)participant.acknowledge(resolution.value(), "implementer-resolution");
            if (resolution.value().correlation_id == id + "-block")
                break;
        }
    }
    if (id == "8") {
        for (int index = 1; index <= 20; ++index) {
            Json burst = Json::object();
            burst.set("op", "status");
            burst.set("state", "burst");
            burst.set("index", index);
            auto sent = participant.send("1", MessageType::Status, std::move(burst), "", 3, 5000);
            if (!sent) {
                (void)participant.publish_status("backpressure-observed",
                                                 Json::object());
                break;
            }
        }
    }
    if (id == "6") {
        // Deliberately stop heartbeating and block: the broker must diagnose an
        // unresponsive worker rather than silently waiting forever.
        participant.stop_heartbeat();
        sleep_ms(2500);
        (void)participant.leave("simulated-unresponsive-exit");
        return 0;
    }

    Json review = Json::object();
    review.set("op", "review");
    review.set("subject", "subsystem-" + id);
    review.set("verdict", "apply");
    (void)participant.send("1", MessageType::Review, review, "", 15);

    const std::uint64_t complete_deadline = now_ns() + 8'000'000'000ULL;
    while (now_ns() < complete_deadline) {
        auto envelope = participant.receive(300);
        if (!envelope)
            continue;
        if (envelope.value().type != MessageType::Ack &&
            envelope.value().type != MessageType::Status)
            (void)participant.acknowledge(envelope.value(), "implementer-complete");
        if (envelope.value().type == MessageType::Complete)
            break;
    }
    (void)participant.leave("done");
    return 0;
}

int run_orchestrator(const std::string &session, const std::string &root,
                     const std::string &snapshot_dir) {
    ParticipantOptions options;
    options.session = session;
    options.worker_id = "1";
    options.role = "orchestrator";
    options.blotter_root = root;
    options.queue_depth = 2;
    auto joined = Participant::join(options);
    if (!joined) {
        std::cerr << "orchestrator join failed: " << joined.error().message << "\n";
        return 1;
    }
    Participant participant = std::move(joined.value());
    (void)participant.start_heartbeat(300);

    // Wait until all six implementers have registered with the broker.
    const std::uint64_t registration_deadline = now_ns() + 6'000'000'000ULL;
    std::size_t registered = 0;
    while (now_ns() < registration_deadline) {
        auto inspection = participant.inspect_broker();
        if (inspection) {
            const Json *workers = inspection.value().find("workers");
            if (workers && workers->as_array())
                registered = workers->as_array()->size();
            if (registered >= 7)
                break;
        }
        sleep_ms(50);
    }

    auto contract = contract_document();
    (void)participant.blotter().publish_contract("mahak.api.v1", contract);
    Json contract_message = Json::object();
    contract_message.set("op", "contract");
    contract_message.set("contract", contract);
    (void)participant.send("*", MessageType::Contract, contract_message, "", 15);

    // Duplicate delivery probe: the broker must acknowledge but route only once.
    auto broker_name = mailbox_name(session, "broker");
    if (broker_name) {
        auto queue = Mailbox::open(broker_name.value(), MailboxOptions{8, 0, 0600},
                                   MailboxOpen::Attach);
        if (queue) {
            Envelope duplicate;
            duplicate.id = "7-999";
            duplicate.sender = "7";
            duplicate.recipient = "1";
            duplicate.sequence = 999;
            duplicate.type = MessageType::Status;
            duplicate.timestamp_ns = now_ns();
            duplicate.payload = Json::object();
            duplicate.payload.set("op", "status");
            duplicate.payload.set("state", "duplicate-probe");
            (void)queue.value().send(duplicate, 1000);
            (void)queue.value().send(duplicate, 1000);
        }
    }

    std::uint32_t statuses = 0, requests = 0, blocked = 0, reviews = 0, errors = 0;
    bool unresponsive_notice = false;
    const std::uint64_t deadline = now_ns() + 10'000'000'000ULL;
    while (now_ns() < deadline && (reviews < 5 || statuses < 6)) {
        auto envelope = participant.receive(200);
        if (!envelope)
            continue;
        if (envelope.value().type != MessageType::Ack &&
            envelope.value().type != MessageType::Status)
            (void)participant.acknowledge(envelope.value(), "orchestrator-consumed");
        switch (envelope.value().type) {
        case MessageType::Status: {
            ++statuses;
            const Json *state = envelope.value().payload.find("state");
            if (state && state->as_string() && *state->as_string() == "duplicate-probe")
                --statuses; // the routed duplicate probe is not a worker status
            break;
        }
        case MessageType::Request: {
            ++requests;
            Json response = Json::object();
            response.set("ok", true);
            response.set("task", "implement your assigned subsystem");
            (void)participant.send(envelope.value().sender, MessageType::Response,
                                   response,
                                   envelope.value().correlation_id.empty()
                                       ? envelope.value().id
                                       : envelope.value().correlation_id,
                                   20);
            break;
        }
        case MessageType::Blocked: {
            ++blocked;
            Json response = Json::object();
            response.set("ok", true);
            response.set("resolution", "freeze timeout at 30000 ms from contract revision 1");
            (void)participant.send(envelope.value().sender, MessageType::Response,
                                   response,
                                   envelope.value().correlation_id.empty()
                                       ? envelope.value().id
                                       : envelope.value().correlation_id,
                                   20);
            Json decision = Json::object();
            decision.set("decision", "timeout-default");
            decision.set("value", 30000);
            decision.set("source", envelope.value().sender);
            (void)participant.blotter().record_decision(decision);
            break;
        }
        case MessageType::Review:
            ++reviews;
            break;
        case MessageType::Error: {
            ++errors;
            const Json *operation = envelope.value().payload.find("op");
            if (operation && operation->as_string() &&
                *operation->as_string() == "worker-unavailable")
                unresponsive_notice = true;
            break;
        }
        default:
            break;
        }
    }

    auto inspection = participant.inspect_broker();
    if (inspection) {
        std::ofstream output(root + "/orchestrator-inspect.json",
                             std::ios::binary | std::ios::trunc);
        output << inspection.value().dump(true) << "\n";
    }
    Json decision = Json::object();
    decision.set("decision", "milestone-integration");
    decision.set("statuses", static_cast<unsigned long long>(statuses));
    decision.set("requests", static_cast<unsigned long long>(requests));
    decision.set("blocked", static_cast<unsigned long long>(blocked));
    decision.set("reviews", static_cast<unsigned long long>(reviews));
    decision.set("errors", static_cast<unsigned long long>(errors));
    decision.set("unresponsive_diagnosed", unresponsive_notice);
    (void)participant.blotter().record_decision(decision);

    for (const char *worker : {"3", "4", "5", "6", "7", "8"}) {
        Json complete = Json::object();
        complete.set("op", "complete");
        complete.set("reason", "integration milestone accepted");
        (void)participant.send(worker, MessageType::Complete, complete, "", 25);
    }
    sleep_ms(300);
    (void)participant.publish_status("complete", decision);
    (void)participant.leave("orchestrator-done");
    Json shutdown = Json::object();
    shutdown.set("op", "shutdown");
    (void)participant.request("broker", shutdown, 3000);
    (void)snapshot_dir;
    return (reviews >= 5 && statuses >= 6 && blocked >= 1) ? 0 : 1;
}

bool wait_children(const std::vector<pid_t> &children, unsigned timeout_ms) {
    const std::uint64_t deadline = now_ns() + static_cast<std::uint64_t>(timeout_ms) * 1'000'000ULL;
    bool all = false;
    while (now_ns() < deadline) {
        all = true;
        for (pid_t child : children) {
            int status = 0;
            const pid_t result = waitpid(child, &status, WNOHANG);
            if (result == 0) {
                all = false;
                break;
            }
        }
        if (all)
            return true;
        sleep_ms(25);
    }
    for (pid_t child : children) {
        int status = 0;
        if (waitpid(child, &status, WNOHANG) == 0)
            ::kill(child, SIGKILL);
        (void)waitpid(child, &status, 0);
    }
    return false;
}

} // namespace

int main(int argc, char **argv) {
    std::string snapshot_dir = "mahak-dev-sessions";
    bool keep = false;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--snapshot" && index + 1 < argc)
            snapshot_dir = argv[++index];
        else if (argument == "--keep")
            keep = true;
        else {
            std::cerr << "usage: mahak-coord-demo [--snapshot DIR] [--keep]\n";
            return 2;
        }
    }

    const std::string session = make_session_id();
    auto root_result = blotter_root(session);
    if (!root_result)
        return 2;
    const std::string root = root_result.value();
    std::vector<std::string> workers{"1", "2", "3", "4", "5", "6", "7", "8"};
    auto created = Blotter::create(session, workers);
    if (!created) {
        std::cerr << "blotter create failed: " << created.error().message << "\n";
        return 1;
    }
    Blotter blotter = std::move(created.value());

    std::vector<pid_t> children;
    const pid_t broker_pid = ::fork();
    if (broker_pid == 0) {
        BrokerOptions options;
        options.session = session;
        options.blotter_root = root;
        options.workers = workers;
        options.heartbeat_timeout_ms = 1500;
        options.request_timeout_ms = 8000;
        options.queue_depth = 4;
        options.verbose = std::getenv("MAHAK_COORD_VERBOSE") != nullptr;
        auto result = run_broker(options);
        _exit(result ? 0 : 1);
    }
    if (broker_pid < 0) {
        std::cerr << "fork broker failed\n";
        (void)blotter.cleanup();
        return 1;
    }
    children.push_back(broker_pid);
    for (int attempt = 0; attempt < 200 && !queue_exists(session); ++attempt)
        sleep_ms(10);
    if (!queue_exists(session)) {
        std::cerr << "broker did not create its ingress queue\n";
        (void)blotter.cleanup();
        return 1;
    }

    for (const char *id : {"3", "4", "5", "6", "7", "8"}) {
        const pid_t child = ::fork();
        if (child == 0) {
            const int result = run_implementer(session, root, id);
            _exit(result);
        }
        if (child > 0)
            children.push_back(child);
    }

    const int orchestrator_result = run_orchestrator(session, root, snapshot_dir);
    const bool children_ok = wait_children(children, 20000);

    bool duplicate_detected = false;
    bool unresponsive_detected = false;
    {
        auto journal = blotter.read_journal();
        if (journal) {
            for (const auto &event : journal.value()) {
                const Json *kind = event.find("kind");
                const Json *data = event.find("data");
                if (!kind || !kind->as_string())
                    continue;
                if (*kind->as_string() == "broker-stop" && data) {
                    const Json *duplicates = data->find("duplicates");
                    const Json *heartbeats = data->find("heartbeat_failures");
                    if (duplicates && duplicates->as_uint64().value_or(0) > 0)
                        duplicate_detected = true;
                    if (heartbeats && heartbeats->as_uint64().value_or(0) > 0)
                        unresponsive_detected = true;
                }
                if (*kind->as_string() == "worker-unavailable")
                    unresponsive_detected = true;
            }
        }
    }
    {
        std::ifstream inspection(root + "/orchestrator-inspect.json", std::ios::binary);
        std::string text((std::istreambuf_iterator<char>(inspection)),
                         std::istreambuf_iterator<char>());
        auto parsed = Json::parse(text, "orchestrator-inspect.json");
        if (parsed) {
            const Json *stats = parsed.value().find("stats");
            if (stats) {
                const Json *duplicates = stats->find("duplicates");
                duplicate_detected = duplicates && duplicates->as_uint64().value_or(0) > 0;
            }
        }
    }
    {
        auto decisions = blotter.read_decisions();
        if (decisions) {
            for (const auto &decision : decisions.value()) {
                const Json *flag = decision.find("unresponsive_diagnosed");
                if (flag && flag->as_bool().value_or(false))
                    unresponsive_detected = true;
            }
        }
    }

    const std::string destination = snapshot_dir + "/" + session;
    auto snapshot = blotter.snapshot(destination);
    Json summary = Json::object();
    summary.set("session", session);
    summary.set("snapshot", destination);
    summary.set("orchestrator_exit", orchestrator_result);
    summary.set("children_exited", children_ok);
    summary.set("duplicate_detected", duplicate_detected);
    summary.set("unresponsive_detected", unresponsive_detected);
    if (snapshot) {
        std::ofstream summary_file(destination + "/DEMO-SUMMARY.json",
                                   std::ios::binary | std::ios::trunc);
        summary_file << summary.dump(true) << "\n";
    } else {
        std::cerr << "snapshot failed: " << snapshot.error().message << "\n";
    }
    for (const char *id : {"1", "2", "3", "4", "5", "6", "7", "8"}) {
        const std::string suffix = std::string(id) == "2" ? "broker" : "w" + std::string(id);
        if (auto name = mailbox_name(session, suffix))
            (void)Mailbox::unlink(name.value());
    }
    if (!keep)
        (void)blotter.cleanup();
    std::cout << summary.dump(true) << "\n";
    return (orchestrator_result == 0 && children_ok && duplicate_detected &&
            unresponsive_detected)
               ? 0
               : 1;
}
