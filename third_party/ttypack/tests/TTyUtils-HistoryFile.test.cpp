#include <TTyUtils/TTyUtils-HistoryFile.hpp>
#include <cassert>
#include <thread>

int main() {
    const auto path = std::filesystem::temp_directory_path() /
        ("ttyutils-history-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    ttyutils::hist::History history{path, ttyutils::hist::Merge::none};
    history.ignore("-private");
    assert(history.append({.text = "echo hi\n\tworld", .timestamp = "1234", .cwd = "a\tb", .status = -7}));
    assert(history.append({.text = " echo leading space"}));
    assert(history.append({.text = "echo -private"}));
    assert(history.entries().size() == 1);
#if defined(__unix__) || defined(__APPLE__)
    std::thread peer([&] {
        ttyutils::hist::History other{path};
        for (int i = 0; i < 30; ++i) assert(other.append({.text = "peer " + std::to_string(i)}));
    });
    for (int i = 0; i < 30; ++i) assert(history.append({.text = "local " + std::to_string(i)}));
    peer.join();
#endif
    assert(history.load());
    assert(history.entries().front().text == "echo hi\n\tworld");
    assert(history.entries().front().cwd == "a\tb");
    assert(history.entries().front().timestamp == "1234");
    assert(history.entries().front().status == -7);
    assert(history.match_prefix("echo").size() == 1);
#if defined(__unix__) || defined(__APPLE__)
    assert(history.match_prefix("peer ").size() == 30);
    assert(history.match_prefix("local ").size() == 30);
#endif
    std::filesystem::remove(path);
    return 0;
}
