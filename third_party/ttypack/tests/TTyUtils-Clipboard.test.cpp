#include <TTyUtils/TTyUtils-Clipboard.hpp>
#include <cassert>
#include <string>
#include <vector>

int main() {
    using namespace ttyutils::clip;
    History::global().clear();
    assert(copy("hello"));
    assert(paste() == "hello");
    assert(!paste(true));
    Policy policy{.max_bytes = 3};
    std::string osc;
    std::vector<std::string> calls;
    Transport transport;
    transport.send_osc = [&](std::string_view sequence) { osc = sequence; return true; };
    transport.write_command = [&](const auto& argv, std::string_view text) {
        calls.push_back(argv.front());
        assert(text == "😀");
        return argv.front() == "xclip";
    };
    assert(copy("\x1B" "AB", transport, policy));
    assert(osc == "\x1B]52;c;G0FC\x1B\\");
    assert(calls.empty());
    assert(copy("😀", transport, policy));
    assert((calls == std::vector<std::string>{"wl-copy", "xclip"}));
    assert(History::global().entry(0) == "😀");
    int queries = 0;
    transport.query_osc = [&](std::string_view request) {
        assert(request == "\x1B]52;c;?\x1B\\");
        ++queries;
        return std::optional<std::string>{ttytk::clipboard::set_osc52("too large")};
    };
    transport.read_command = [](const auto& argv) {
        return argv.front() == "pbpaste" ? std::optional<std::string>{"native\n"} : std::nullopt;
    };
    assert(!paste(transport, true, policy) && queries == 0);
    assert(paste(transport, false, policy) == "native\n" && queries == 1);
    transport.query_osc = [](std::string_view) { return std::optional<std::string>{"52;c;YWJj"}; };
    policy.allow_remote_read = true;
    assert(paste(transport, true, policy) == "abc");
}
