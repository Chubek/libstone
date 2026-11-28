// Broker integration: comrade names resolve to protocol ids through the roster.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <unistd.h>

#include <mqueue.h>

#include "mahak/coordination/Broker.hpp"
#include "mahak/coordination/Roster.hpp"

using moonstone::ErrorCategory;
using namespace moonstone::mahak;
using namespace moonstone::mahak::coordination;

namespace {

int failures = 0;

void check(bool condition, const std::string &message) {
    if (condition)
        return;
    std::cerr << "FAIL: " << message << "\n";
    ++failures;
}

std::string unique_session() {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%08x%08x", static_cast<unsigned>(::getpid()),
                  static_cast<unsigned>(
                      std::chrono::steady_clock::now().time_since_epoch().count() & 0xffffffffu));
    return buffer;
}

Envelope request(std::string id, std::string sender, std::string recipient,
                 std::uint64_t sequence, Json payload) {
    Envelope envelope;
    envelope.id = std::move(id);
    envelope.sender = std::move(sender);
    envelope.recipient = std::move(recipient);
    envelope.sequence = sequence;
    envelope.type = MessageType::Request;
    envelope.payload = std::move(payload);
    return envelope;
}

} // namespace

int main() {
    if (!std::filesystem::is_directory("/dev/shm")) {
        std::cout << "SKIP: /dev/shm is required for the coordination blotter\n";
        return 0;
    }
    const std::string session = unique_session();
    const std::string root = "/tmp/roster-broker-" + session;
    const std::string blotter = "/dev/shm/mahak-" + session;
    auto cleanup = [&] {
        for (const char *suffix : {"-broker", "-w2", "-w3"})
            ::mq_unlink(("/mahak-" + session + suffix).c_str());
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
        std::filesystem::remove_all(blotter, ignored);
    };
    cleanup();

    auto roster = build_roster(root, session, {
                                                 {"Atlas", "orchestrator", 1, true},
                                                 {"Flint", "engine", 2, false},
                                                 {"Reed", "cli", 3, false},
                                             });
    check(static_cast<bool>(roster), "roster builds");
    if (!roster)
        return 1;
    check(static_cast<bool>(publish_roster(roster.value())), "roster publishes");

    BrokerOptions options;
    options.session = session;
    options.workers = {"2", "3"};
    options.blotter_root = root + "/blotter";
    options.roster_root = root;

    std::atomic<bool> stop{false};
    std::atomic<int> broker_result{-1};
    std::thread broker([&] {
        auto stats = run_broker(options, &stop);
        broker_result.store(stats ? 0 : 1);
        if (!stats)
            std::cerr << "broker failed: " << stats.error().message << "\n";
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(400));

    auto flint_name = mailbox_name(session, "w2");
    auto reed_name = mailbox_name(session, "w3");
    auto ingress_name = mailbox_name(session, "broker");
    check(flint_name && reed_name && ingress_name, "queue names derive from the session");
    if (!flint_name || !reed_name || !ingress_name) {
        stop.store(true);
        broker.join();
        cleanup();
        return 1;
    }
    const MailboxOptions slot{4, 0, 0600};
    auto flint = Mailbox::open(flint_name.value(), slot, MailboxOpen::Create);
    auto reed = Mailbox::open(reed_name.value(), slot, MailboxOpen::Create);
    auto ingress = Mailbox::open(ingress_name.value(), slot, MailboxOpen::Attach);
    check(flint && reed && ingress, "worker mailboxes and the ingress queue open");
    if (!flint || !reed || !ingress) {
        stop.store(true);
        broker.join();
        cleanup();
        return 1;
    }

    // Workers write to the broker ingress and read their own inbox.
    auto register_worker = [&](const std::string &id, const std::string &role) {
        Json payload = Json::object();
        payload.set("op", "register");
        payload.set("worker", id);
        payload.set("role", role);
        return ingress.value().send(request(id + "-reg", id, "broker", 1, payload), 2000);
    };
    check(static_cast<bool>(register_worker("2", "engine")), "engine registers");
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    check(static_cast<bool>(register_worker("3", "cli")), "cli registers");
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Registration responses land in each worker's own inbox.
    auto registration = flint.value().receive(2000);
    check(registration && registration.value().type == MessageType::Response,
          "registration response reaches the engine inbox");
    auto reed_registration = reed.value().receive(2000);
    check(reed_registration && reed_registration.value().type == MessageType::Response,
          "registration response reaches the cli inbox");

    // Private contact by comrade name: Reed addresses Flint by name.
    Json ping = Json::object();
    ping.set("op", "ping");
    const bool sent = static_cast<bool>(
        ingress.value().send(request("3-2", "3", "Flint", 2, ping), 2000));
    check(sent, "a message addressed to a comrade name is accepted");
    auto delivered = flint.value().receive(3000);
    check(delivered && delivered.value().recipient == "2",
          "the broker resolves 'Flint' to protocol id 2");
    if (delivered) {
        const Json *op = delivered.value().payload.find("op");
        check(op != nullptr && op->as_string() && *op->as_string() == "ping",
              "the payload survives name resolution");
    }

    // An unknown comrade is rejected with an explicit error to the sender.
    Json stray = Json::object();
    stray.set("op", "ping");
    (void)ingress.value().send(request("3-3", "3", "Nobody", 3, stray), 2000);
    // The delivery ACK for the previous request may arrive first; scan a bounded
    // number of envelopes for the rejection.
    bool saw_error = false;
    std::string error_message;
    for (int attempt = 0; attempt < 3 && !saw_error; ++attempt) {
        auto rejected = reed.value().receive(2000);
        if (!rejected)
            break;
        if (rejected.value().type != MessageType::Error)
            continue;
        saw_error = true;
        const Json *message = rejected.value().payload.find("message");
        if (message != nullptr && message->as_string())
            error_message = *message->as_string();
    }
    check(saw_error, "an unknown comrade produces an Error envelope");
    check(error_message.find("unknown comrade") != std::string::npos,
          "the error names the unknown comrade");

    stop.store(true);
    broker.join();
    check(broker_result.load() == 0, "the broker shuts down cleanly");
    cleanup();

    if (failures != 0) {
        std::cerr << failures << " roster/broker check(s) failed\n";
        return 1;
    }
    std::cout << "roster/broker tests passed\n";
    return 0;
}
