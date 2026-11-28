#include "mahak/coordination/Blotter.hpp"
#include "mahak/coordination/Mailbox.hpp"
#include "mahak/coordination/Protocol.hpp"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

using namespace moonstone::mahak::coordination;
using moonstone::mahak::Json;

namespace {

int failures = 0;

void check(bool condition, const char *message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

Envelope sample(const std::string &sender, std::uint64_t sequence,
                const std::string &recipient = "broker") {
    Envelope envelope;
    envelope.id = sender + "-" + std::to_string(sequence);
    envelope.sender = sender;
    envelope.recipient = recipient;
    envelope.sequence = sequence;
    envelope.type = MessageType::Request;
    envelope.priority = 7;
    envelope.timestamp_ns = 123456789;
    envelope.correlation_id = "corr-" + std::to_string(sequence);
    envelope.payload = Json::object();
    envelope.payload.set("op", "ping");
    envelope.payload.set("value", 42);
    return envelope;
}

void test_envelope() {
    auto encoded = encode_envelope(sample("3", 1));
    check(static_cast<bool>(encoded), "encode envelope");
    if (!encoded)
        return;
    auto decoded = decode_envelope(encoded.value());
    check(static_cast<bool>(decoded), "decode envelope");
    if (!decoded)
        return;
    check(decoded.value().sender == "3", "sender roundtrip");
    check(decoded.value().recipient == "broker", "recipient roundtrip");
    check(decoded.value().sequence == 1, "sequence roundtrip");
    check(decoded.value().type == MessageType::Request, "type roundtrip");
    check(decoded.value().priority == 7, "priority roundtrip");
    check(decoded.value().payload.find("value") != nullptr, "payload roundtrip");

    check(!decode_envelope("{"), "reject malformed envelope");
    check(!decode_envelope(R"({"v":2})"), "reject unknown envelope version");
    Envelope bad = sample("3", 2);
    bad.payload = Json("not-an-object");
    check(!encode_envelope(bad), "reject scalar payload");
    bad = sample("3", 0);
    check(!encode_envelope(bad), "reject zero sequence");
    bad = sample("../../", 3);
    check(!encode_envelope(bad), "reject unsafe sender");

    for (MessageType type : {MessageType::Request, MessageType::Response, MessageType::Ack,
                             MessageType::Blocked, MessageType::Contract, MessageType::Review,
                             MessageType::Status, MessageType::Error, MessageType::Complete}) {
        check(parse_message_type(name(type)).value_or(MessageType::Error) == type,
              "message type name roundtrip");
    }
}

void test_mailbox() {
    const std::string session = make_session_id();
    auto name = mailbox_name(session, "test");
    check(static_cast<bool>(name), "mailbox name");
    if (!name)
        return;
    (void)Mailbox::unlink(name.value());
    auto opened = Mailbox::open(name.value(), MailboxOptions{4, 0, 0600}, MailboxOpen::Create);
    check(static_cast<bool>(opened), "create mailbox");
    if (!opened)
        return;
    Mailbox queue = std::move(opened.value());
    std::vector<Envelope> sent;
    for (std::uint64_t sequence = 1; sequence <= 4; ++sequence) {
        auto envelope = sample("3", sequence);
        auto result = queue.try_send(envelope);
        check(static_cast<bool>(result), "send within depth");
        if (result)
            check(result.value(), "queue accepted message");
        sent.push_back(envelope);
    }
    auto overflow = queue.try_send(sample("3", 5));
    check(static_cast<bool>(overflow), "overflow is a value");
    if (overflow)
        check(!overflow.value(), "bounded queue applies backpressure");
    auto received = queue.receive(1000);
    check(static_cast<bool>(received), "receive queued message");
    if (received)
        check(received.value().sequence == 1, "FIFO order");
    auto after = queue.try_send(sample("3", 6));
    check(after && after.value(), "send succeeds after draining");
    (void)queue.close();
    (void)Mailbox::unlink(name.value());
}

void test_blotter() {
    const std::string session = make_session_id();
    std::vector<std::string> workers{"1", "2", "3", "4", "5", "6", "7", "8"};
    auto created = Blotter::create(session, workers);
    check(static_cast<bool>(created), "create blotter");
    if (!created)
        return;
    Blotter &blotter = created.value();
    check(blotter.root().rfind("/dev/shm/mahak-", 0) == 0, "blotter is under /dev/shm");

    Json contract = Json::object();
    contract.set("api", 1);
    contract.set("name", "v1");
    auto revision1 = blotter.publish_contract("api", contract);
    check(revision1 && revision1.value() == 1, "first contract revision");
    contract.set("api", 2);
    auto revision2 = blotter.publish_contract("api", contract);
    check(revision2 && revision2.value() == 2, "changed contract creates revision");
    auto loaded = blotter.contract("api");
    check(loaded && loaded.value().find("api") != nullptr &&
              loaded.value().find("api")->as_int64().value_or(0) == 2,
          "latest contract content");

    // Eight concurrent processes update their own worker records. The journal
    // must contain every update and no record may be lost or corrupt.
    std::vector<pid_t> children;
    for (const auto &worker : workers) {
        const pid_t child = ::fork();
        if (child == 0) {
            auto attached = Blotter::attach(blotter.root());
            if (!attached)
                _exit(2);
            for (int index = 0; index < 5; ++index) {
                Json status = Json::object();
                status.set("state", "running");
                status.set("index", index);
                if (!attached.value().update_worker(worker, status))
                    _exit(3);
            }
            _exit(0);
        }
        if (child > 0)
            children.push_back(child);
    }
    bool all_okay = true;
    for (pid_t child : children) {
        int status = 0;
        if (waitpid(child, &status, 0) != child || !WIFEXITED(status) ||
            WEXITSTATUS(status) != 0)
            all_okay = false;
    }
    check(all_okay, "concurrent worker updates");
    auto records = blotter.workers();
    check(records && records.value().size() == workers.size(), "all worker records readable");
    auto journal = blotter.read_journal();
    check(journal && journal.value().size() >= 41, "append-only journal has every update");
    auto revision = blotter.revision();
    check(revision && revision.value() >= 41, "revision counts journal records");

    Json decision = Json::object();
    decision.set("decision", "accept");
    check(static_cast<bool>(blotter.record_decision(decision)), "record decision");
    Json verdict = Json::object();
    verdict.set("verdict", "apply");
    check(static_cast<bool>(blotter.record_verdict("review-1", verdict)), "record verdict");
    auto verdict_read = blotter.verdict("review-1");
    check(verdict_read && verdict_read.value().find("verdict") != nullptr, "read verdict");

    const std::string snapshot = "/tmp/mahak-coordination-snapshot-" + session;
    std::filesystem::remove_all(snapshot);
    check(static_cast<bool>(blotter.snapshot(snapshot)), "snapshot writes outside /dev/shm");
    check(std::filesystem::exists(snapshot + "/manifest.json"), "snapshot manifest");
    check(std::filesystem::exists(snapshot + "/SNAPSHOT.json"), "snapshot metadata");
    std::filesystem::remove_all(snapshot);

    check(static_cast<bool>(blotter.cleanup()), "cleanup session");
    check(!std::filesystem::exists(blotter.root()), "session root removed");
}

} // namespace

int main() {
    test_envelope();
    test_mailbox();
    test_blotter();
    if (failures != 0) {
        std::cerr << failures << " coordination check(s) failed\n";
        return 1;
    }
    std::cout << "coordination checks passed\n";
    return 0;
}
