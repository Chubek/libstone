#include <TTyUtils/TTyUtils-LogView.hpp>
#include <cassert>

int main() {
    using namespace ttyutils;
    auto filter = logview::Filter::exclude(R"(\bhealthz\b)");
    assert(filter.accepts("ERROR"));
    assert(!filter.accepts("/healthz"));
    assert(filter.accepts("healthz2"));
    assert(!logview::Filter::include("[").valid());

    logview::LogView view{pager::Source{"ERROR first\nINFO /healthz\nINFO second\nWARN third\n"}};
    view.set_rect({0, 0, 16, 2});
    view.set_filter(std::move(filter));
    assert(view.length() == 3 && view.offset() == 1);
    theme::Style error{.foreground = ttytk::color::Rgb{255, 0, 0}};
    view.add_rule({R"(^ERROR\b)", error});
    view.scroll_to(0);
    view.append("ERROR fourth\n");
    assert(!view.following() && view.pending_lines() == 1);
    ui::Canvas canvas{{0, 0, 16, 2}};
    view.render(canvas);
    assert(canvas.texts().front().style == error);
    assert(canvas.texts().back().value.starts_with("1 new lines"));
    view.resume();
    assert(view.offset() == 2 && view.pending_lines() == 0);
}
