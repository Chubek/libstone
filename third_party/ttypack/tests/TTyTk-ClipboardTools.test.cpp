#include <TTyTk/TTyTk-ClipboardTools.hpp>
#include <cassert>
int main() {
    using namespace ttytk::clipboard;
    set("text");
    set("primary", Primary);
    assert(get() == "text");
    assert(get(Primary) == "primary");
    clear();
    assert(get().empty() && get(Primary) == "primary");
    assert(request_osc52(Primary) == "\x1b]52;p;?\x1b\\");
    assert(set_osc52("a") == "\x1b]52;c;YQ==\x1b\\");
    assert(set_osc52("ab") == "\x1b]52;c;YWI=\x1b\\");
    assert(parse_osc52(set_osc52(std::string_view{"a\0\xff", 3})) == std::string("a\0\xff", 3));
    assert(parse_osc52("\x1b]52;p;YQ==\a", Primary) == "a");
    assert(parse_osc52("52;c;") == "");
    assert(!parse_osc52("\x1b]52;c;YQ=="));
    assert(!parse_osc52("52;c;YR=="));
    assert(!parse_osc52("52;c;?"));
    assert(!parse_osc52("52;p;YQ=="));
}
