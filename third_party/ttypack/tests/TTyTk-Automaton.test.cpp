#include <TTyTk/TTyTk-Automaton.hpp>

#include <cassert>
#include <string>
#include <utility>
#include <vector>

int main() {
    ttytk::Automaton parser;
    using Record = std::pair<char, std::string>;
    std::vector<Record> events;
    parser.on_print = [&](const std::string_view text) {
        if (!events.empty() && events.back().first == 'p') events.back().second += text;
        else events.emplace_back('p', text);
    };
    parser.on_csi = [&](const std::string_view value) { events.emplace_back('c', value); };
    parser.on_osc = [&](const std::string_view value) { events.emplace_back('o', value); };
    parser.on_dcs = [&](const std::string_view value) { events.emplace_back('d', value); };
    parser.on_escape = [&](const std::string_view value) { events.emplace_back('e', value); };
    parser.on_control = [&](const unsigned char value) {
        events.emplace_back('x', std::string(1, static_cast<char>(value)));
    };

    const std::string_view stream{
        "ok\xC3\x9B\x1B[38:2::0:1:2m\x1B]2;title\x07\x9D" "2;\xC3\x9C\x9C"
        "\x1B[2\nJ\x1BP1qdata\x1B\\\x1B(B\x1B_ignored\x1B\\"
        "\x1B[1 2m\x1B]2;cancel\x18\x1B]abandoned\x1B[H\x90q\x1B\\"};
    const std::vector<Record> expected{
        {'p', "ok\xC3\x9B"}, {'c', "38:2::0:1:2m"}, {'o', "2;title"}, {'o', "2;\xC3\x9C"},
        {'x', "\n"}, {'c', "2J"}, {'d', "1qdata"}, {'e', "(B"}, {'x', "\x18"},
        {'c', "H"}, {'d', "q"}};
    for (std::size_t split = 0; split <= stream.size(); ++split) {
        parser.reset();
        events.clear();
        parser.feed(stream.substr(0, split));
        parser.feed(stream.substr(split));
        assert(events == expected);
    }

    events.clear();
    parser.max_sequence_bytes = 3;
    parser.feed("\x1B[123m\x1B]title\x07\x1BPqdata\x1B\\\x1B[31m");
    parser.feed("\x1B]half");
    parser.reset();
    parser.feed("end");
    assert((events == std::vector<Record>{{'c', "31m"}, {'p', "end"}}));
}
