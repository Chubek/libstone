#include <TTyTk/TTyTk-TermCap.hpp>

#include <cassert>

int main() {
    const auto entry = ttytk::termcap::load_from("vt|test:am:co#80:cm=\\E[%i%d;%dH:");
    assert(entry);
    assert(entry.value().flag("am"));
    assert(entry.value().num("co") == 80);
    assert(entry.value().str("cm").has_value());
    assert(ttytk::termcap::tgoto(*entry.value().str("cm"), 0, 0) == "\x1b[1;1H");
    using namespace ttytk;
    assert(termcap::tgoto(*entry.value().str("cup"), 7, 2) == "\x1b[3;8H");
    assert(termcap::tgoto("5.5*%r%2;%3", 7, 2) == "5.5*07;002");
    assert(termcap::tgoto("%>AB%+ %B%d", 12, 100) == std::string("\xc6" "18"));
    const auto chained = termcap::load_from(
        "# comment\n"
        "child|alias:li#30:bs@:tc=base-alias:\\\n"
        "\t:zz=\\E\\072a\\:b^?:\n"
        "base|base-alias:co#0100:bs:tc=root:\n"
        "root|root desc:co#1:li#12:am:\n");
    assert(chained);
    assert(chained.value().num("cols") == 64 && chained.value().num("lines") == 30);
    assert(chained.value().flag("am") && !chained.value().flag("bs"));
    assert(chained.value().str("zz") == "\x1b:a:b\x7f");
    assert(!termcap::load_from("a:tc=b:\nb:tc=a:"));
    assert(!termcap::load_from("a:tc=missing:"));
    assert(!termcap::load_from("a:co#12x:"));

    terminfo::Entry modern;
    modern.flags = {false, true};
    modern.numbers = {120};
    modern.strings.resize(11);
    modern.strings[10] = "\x1b[%i%p1%d;%p2%dH";
    const auto bridge = termcap::from_terminfo(modern);
    assert(bridge.flag("am") && bridge.num("co") == 120);
    assert(bridge.str("cm") == modern.str("cup"));
    assert(termcap::tgoto(*bridge.str("cm"), 12, 5) == "\x1b[6;13H");
}
