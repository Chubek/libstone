#include <TTyTk/TTyTk-EmbedTools.hpp>
#include <cassert>

int main() {
    using namespace ttytk::embed;
    assert(wrap_shell("echo x") == "exec echo x");
    assert(inject_env("_NAME9", "a'b\n$c") == "export _NAME9='a'\\''b\n$c'");
    assert(inject_env("9bad", "value").empty());
    assert(inject_env("bad-name", "value").empty());
    assert(inject_env("NAME", std::string_view{"a\0b", 3}).empty());
    int source[2]{}, destination[2]{};
    if (::pipe(source) != 0 || ::pipe(destination) != 0) return 1;
    if (::write(source[1], "a\0b", 3) != 3) return 1;
    ::close(source[1]);
    const auto first = passthrough(source[0], destination[1]);
    assert(first && first.value() == 3);
    char bytes[3]{};
    if (::read(destination[0], bytes, sizeof(bytes)) != 3) return 1;
    assert((std::string_view{bytes, sizeof(bytes)} == std::string_view{"a\0b", 3}));
    const auto eof = passthrough(source[0], destination[1]);
    assert(eof && eof.value() == 0);
    assert(!passthrough(-1, destination[1]));
    ::close(source[0]);
    ::close(destination[0]);
    ::close(destination[1]);
}
