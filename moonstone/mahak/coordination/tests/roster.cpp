// Worker roster contracts: publish, reload, name resolution, acks, ownership.
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>
#include <unistd.h>
#include <vector>

#include "mahak/coordination/Protocol.hpp"
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

std::vector<RosterIdentity> nine_workers() {
    return {
        {"Atlas", "orchestrator", 1, true}, {"Flint", "engine", 2, false},
        {"Reed", "cli-c-abi", 3, false},    {"Lark", "sdk", 4, false},
        {"Wren", "lua", 5, false},          {"Cedar", "storage", 6, false},
        {"Sage", "recommendations", 7, false},
        {"Iris", "statistics", 8, false},   {"Ash", "cephyr-suites", 9, false},
    };
}

} // namespace

int main() {
    const std::string session = "0123456789abcdef";
    const std::string root =
        (std::filesystem::temp_directory_path() / ("mahak-roster-" + std::to_string(::getpid())))
            .string();
    std::filesystem::remove_all(root);

    check(default_root() == "/dev/shm/foo-blotter", "default roster root is /dev/shm/foo-blotter");

    auto roster = build_roster(root, session, nine_workers());
    check(static_cast<bool>(roster), "nine named workers build a roster");
    if (!roster)
        return 1;
    check(roster.value().entries.size() == 9, "roster holds nine entries");
    check(roster.value().broker_ingress == "/mahak-" + session + "-broker",
          "orchestrator uses the broker ingress queue");

    auto atlas = resolve_worker(roster.value(), "Atlas");
    check(static_cast<bool>(atlas), "Atlas resolves by name");
    if (atlas) {
        check(atlas.value().protocol_id == 1, "Atlas has protocol id 1");
        check(atlas.value().inbox == roster.value().broker_ingress, "Atlas inbox is the ingress");
    }
    auto flint = resolve_worker(roster.value(), "Flint");
    check(flint && flint.value().inbox == "/mahak-" + session + "-w2",
          "Flint inbox derives from the protocol id");
    auto by_id = resolve_protocol_id(roster.value(), "9");
    check(by_id && by_id.value() == 9, "numeric ids resolve as well as names");
    auto unknown = resolve_worker(roster.value(), "Nobody");
    check(!unknown, "unknown comrade is rejected");
    if (!unknown)
        check(unknown.error().category == ErrorCategory::NotFound,
              "unknown comrade reports NotFound");

    auto bad_duplicate = build_roster(root, session, {
                                                         {"Atlas", "orchestrator", 1, true},
                                                         {"Atlas", "engine", 2, false},
                                                     });
    check(!bad_duplicate, "duplicate names are rejected");
    auto double_broker = build_roster(root, session, {
                                                         {"Atlas", "orchestrator", 1, true},
                                                         {"Flint", "orchestrator", 2, true},
                                                     });
    check(!double_broker, "two orchestrators are rejected");
    auto no_broker = build_roster(root, session, {{"Flint", "engine", 2, false}});
    check(!no_broker, "a roster without an orchestrator is rejected");
    auto bad_session = build_roster(root, "../escape", nine_workers());
    check(!bad_session, "unsafe session ids are rejected");

    Roster published = roster.value();
    published.published_unix_ns = 1234;
    check(static_cast<bool>(publish_roster(published)), "roster publishes");
    check(std::filesystem::exists(root + "/WORKERS.json"), "WORKERS.json exists");
    check(std::filesystem::exists(root + "/roster.lock"), "roster.lock exists");

    auto loaded = load_roster(root);
    check(static_cast<bool>(loaded), "published roster reloads");
    if (loaded) {
        check(loaded.value().revision == 1, "revision survives publication");
        check(loaded.value().session == session, "session survives publication");
        check(loaded.value().entries.size() == 9, "all workers survive publication");
        const RosterEntry *ash = loaded.value().lookup("Ash");
        check(ash != nullptr && ash->role == "cephyr-suites", "role survives publication");
        check(loaded.value().lookup("Ash")->orchestrator == false,
              "orchestrator flag survives publication");
    }
    auto missing = load_roster(root + "-absent");
    check(!missing, "missing roster is an error");
    if (!missing)
        check(missing.error().category == ErrorCategory::NotFound, "missing roster is NotFound");
    auto optional_missing = try_load_roster(root + "-absent");
    check(optional_missing && !optional_missing.value().has_value(),
          "try_load_roster reports absence without failing");

    auto pending = pending_acknowledgements(loaded.value());
    check(pending && pending.value().size() == 9, "every worker starts unacknowledged");
    check(static_cast<bool>(acknowledge_roster(root, "Atlas", 1)), "Atlas acknowledges");
    check(static_cast<bool>(acknowledge_roster(root, "Flint", 1)), "Flint acknowledges");
    check(static_cast<bool>(acknowledge_roster(root, "Atlas", 1)), "repeat ack is idempotent");
    pending = pending_acknowledgements(loaded.value());
    check(pending && pending.value().size() == 7, "two workers remain pending");
    auto revision = acknowledged_revision(root);
    check(revision && revision.value() == 1, "acknowledged revision recorded");
    auto stale = acknowledge_roster(root, "Reed", 0);
    check(!stale, "stale revision ack rejected");
    auto stranger = acknowledge_roster(root, "Nobody", 1);
    check(!stranger, "ack from an unknown worker rejected");

    Roster revised = loaded.value();
    revised.revision = 2;
    check(static_cast<bool>(publish_roster(revised)), "revised roster publishes");
    auto reloaded = load_roster(root);
    check(reloaded && reloaded.value().revision == 2, "revision bumped");
    auto after_bump = pending_acknowledgements(reloaded.value());
    check(after_bump && after_bump.value().size() == 9,
          "a new revision invalidates previous acks");
    auto old_ack = acknowledge_roster(root, "Reed", 1);
    check(!old_ack, "ack against the previous revision is stale");

    {
        auto lock = RosterLock::acquire(root);
        check(static_cast<bool>(lock), "session lock is acquired");
        auto second = RosterLock::acquire(root);
        check(!second, "a second session cannot own the same roster directory");
        if (!second)
            check(second.error().category == ErrorCategory::BudgetExhausted,
                  "ownership conflict reports BudgetExhausted");
        if (lock) {
            auto owner = lock.value().owner_pid();
            check(owner && owner.value() == static_cast<long long>(::getpid()),
                  "session.lock records the owning pid");
        }
    }
    check(static_cast<bool>(RosterLock::acquire(root)), "the lock is released on destruction");

    std::filesystem::remove_all(root);

    if (failures != 0) {
        std::cerr << failures << " roster check(s) failed\n";
        return 1;
    }
    std::cout << "roster tests passed\n";
    return 0;
}
