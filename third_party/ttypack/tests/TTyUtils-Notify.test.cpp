#include <TTyUtils/TTyUtils-Notify.hpp>
#include <cassert>

int main() {
    using namespace ttyutils::notify;
    auto* output = std::tmpfile();
    assert(output != nullptr);
    policy().backend = Backend::Terminal;
    policy().allow_external = false;
    policy().do_not_disturb = true;
    assert(!send({"title", "suppressed"}, output));
    policy().do_not_disturb = false;
    assert(::setenv("TERM_PROGRAM", "WezTerm", 1) == 0);
    const Message message{"a;b", "c\a\x1b]52"};
    assert(send(message, output));
    assert(!send(message, output));
    policy().backend = Backend::Bell;
    assert(send({"ding"}, output));
    assert(!send(message, output));
    std::rewind(output);
    char bytes[100];
    const auto count = std::fread(bytes, 1, sizeof(bytes), output);
    assert(std::string_view(bytes, count) == "\x1b]777;notify;a b;c  ]52\x1b\\\a");
    std::fclose(output);
}
