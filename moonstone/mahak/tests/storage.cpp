// Session serialization and durable history store contracts.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <unistd.h>

#include "mahak/Storage.hpp"

using namespace moonstone::mahak;

namespace {

int failures = 0;

void check(bool condition, const std::string &message) {
    if (condition)
        return;
    std::cerr << "FAIL: " << message << "\n";
    ++failures;
}

SessionRecord make_session(std::string id, double value) {
    SessionRecord session;
    session.id = std::move(id);
    session.manifest_id = "unit";
    session.manifest_version = "1.0.0";
    session.manifest_digest = "deadbeefdeadbeef";
    session.created_unix_ns = 1000;
    session.finished_unix_ns = 2000;
    session.status = "completed";
    session.host = "test-host";
    session.environment.architecture = "x86_64";

    RunRecord run;
    run.case_id = "bench@target";
    run.benchmark_id = "bench";
    run.target_id = "target";
    run.configuration = "size=1";
    run.status = "completed";
    run.oracle.passed = true;
    run.oracle.validity = Validity::Valid;

    SampleRecord sample;
    sample.index = 0;
    sample.repetition = 0;
    sample.observation.metric = "wall";
    sample.observation.unit = "ns";
    sample.observation.direction = MetricDirection::LowerIsBetter;
    sample.observation.value = value;
    sample.observation.valid = true;
    sample.observation.validity = Validity::Valid;
    sample.observation.sequence = 0;
    sample.measurement.wall_ns = static_cast<std::uint64_t>(value);
    run.samples.push_back(sample);
    session.runs.push_back(run);
    return session;
}

} // namespace

int main() {
    const SessionRecord session = make_session("roundtrip", 1234.5);
    auto text = encode_session_text(session, true);
    check(static_cast<bool>(text), "session encodes");
    if (text) {
        auto decoded = decode_session_text(text.value(), "inline");
        check(static_cast<bool>(decoded), "session decodes");
        if (decoded) {
            check(decoded.value().id == session.id, "id survives round-trip");
            check(decoded.value().runs.size() == 1, "run count survives round-trip");
            check(decoded.value().runs.front().samples.size() == 1, "sample survives round-trip");
            check(decoded.value().runs.front().samples.front().observation.value == 1234.5,
                  "observation value survives round-trip");
            check(decoded.value().manifest_digest == session.manifest_digest,
                  "digest survives round-trip");
            check(session_digest(decoded.value()) == session_digest(session),
                  "digest is stable across encode/decode");
        }
    }

    auto future = Json::parse(R"({"schema":99,"id":"x","runs":[]})", "future");
    check(static_cast<bool>(future), "future document parses as JSON");
    if (future) {
        auto rejected = decode_session(future.value());
        check(!rejected, "future result schema rejected");
        if (!rejected)
            check(rejected.error().category == ErrorCategory::Unsupported,
                  "future schema reports Unsupported");
    }

    SessionRecord invalid = make_session("", 1.0);
    check(!validate_session(invalid), "empty id is invalid");
    SessionRecord bad_status = make_session("status", 1.0);
    bad_status.status = "exploded";
    check(!validate_session(bad_status), "unknown status is invalid");

    auto csv = session_csv(session);
    check(static_cast<bool>(csv), "csv encodes");
    if (csv) {
        check(csv.value().rfind("session_id,", 0) == 0, "csv has a stable header");
        check(csv.value().find("wall") != std::string::npos, "csv contains the metric");
    }
    const std::string schema = result_schema_json();
    check(schema.find("\"schema\"") != std::string::npos, "schema document mentions schema");

    const std::string root =
        (std::filesystem::temp_directory_path() / ("mahak-store-test-" + std::to_string(::getpid())))
            .string();
    std::filesystem::remove_all(root);
    auto store = HistoryStore::open(root);
    check(static_cast<bool>(store), "history store opens");
    if (store) {
        const SessionRecord first = make_session("s1", 10.0);
        check(static_cast<bool>(store.value().commit(first)), "first commit succeeds");
        check(static_cast<bool>(store.value().commit(first)), "identical re-commit is idempotent");
        auto conflicting = make_session("s1", 11.0);
        auto rejected = store.value().commit(conflicting);
        check(!rejected, "conflicting re-commit rejected");
        if (!rejected)
            check(rejected.error().category == ErrorCategory::InvalidInput,
                  "conflict reports InvalidInput");

        auto listed = store.value().list();
        check(listed && listed.value().size() == 1, "index lists one session");
        auto loaded = store.value().load("s1");
        check(loaded && loaded.value().runs.size() == 1, "session loads");
        check(!store.value().load("../escape"), "unsafe session ids are rejected");

        std::filesystem::remove(std::filesystem::path(root) / "index.json");
        auto rebuilt = store.value().rebuild_index();
        check(static_cast<bool>(rebuilt), "index rebuilds from canonical sessions");
        auto relisted = store.value().list();
        check(relisted && relisted.value().size() == 1, "rebuilt index lists the session");

        std::ofstream stray(std::filesystem::path(root) / "sessions" / "junk.json.tmp");
        stray << "partial";
        stray.close();
        check(static_cast<bool>(store.value().recover()), "recover succeeds");
        check(!std::filesystem::exists(std::filesystem::path(root) / "sessions" / "junk.json.tmp"),
              "recover removes temporary files");

        check(static_cast<bool>(store.value().commit(make_session("s2", 20.0))),
              "second commit succeeds");
        auto ordered = store.value().list();
        check(ordered && ordered.value().size() == 2, "two sessions listed");
        check(static_cast<bool>(store.value().remove("s2")), "session removal succeeds");
        auto after = store.value().list();
        check(after && after.value().size() == 1, "removal updates the index");
    }
    std::filesystem::remove_all(root);

    if (failures != 0) {
        std::cerr << failures << " storage check(s) failed\n";
        return 1;
    }
    std::cout << "storage tests passed\n";
    return 0;
}
