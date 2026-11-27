#include <TTyTk/TTyTk-TermMode.hpp>

#include <cassert>

int main() {
    ttytk::TermMode modes{};
    assert(modes.test(ttytk::Mode::AutoWrap));
    modes.restore();
    assert(modes.test(ttytk::Mode::AutoWrap));
    modes.set(ttytk::AnsiMode::Insert);
    assert(modes.test(4, false) && !modes.test(4, true));
    modes.save(ttytk::Mode::BracketedPaste);
    modes.set(ttytk::Mode::BracketedPaste);
    modes.restore(ttytk::Mode::BracketedPaste);
    assert(!modes.test(ttytk::Mode::BracketedPaste));
    modes.set(ttytk::Mode::MouseButtonEvent);
    modes.set(ttytk::Mode::MouseAnyEvent);
    modes.set(ttytk::Mode::MouseSgr);
    assert(!modes.test(ttytk::Mode::MouseButtonEvent));
    assert(modes.test(ttytk::Mode::MouseAnyEvent) && modes.test(ttytk::Mode::MouseSgr));
    modes.set(ttytk::Mode::AltScreen);
    modes.save();
    modes.reset(ttytk::Mode::AltScreen);
    assert(!modes.test(ttytk::Mode::AltScreen));
    modes.restore();
    assert(modes.test(ttytk::Mode::AltScreen));
}
