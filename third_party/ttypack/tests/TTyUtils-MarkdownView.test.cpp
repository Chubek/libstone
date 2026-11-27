#include <TTyUtils/TTyUtils-MarkdownView.hpp>
#include <cassert>

int main() {
    using namespace ttyutils;
    mdview::MarkdownView view;
    view.set_text("# Title\ntext\n```sh\n# comment\n```\n## [More](https://example.org)\nEND\n");
    assert(view.toc().size() == 2);
    assert(view.toc()[0].title == "Title" && view.toc()[1].title == "More");
    assert(view.toc()[1].line == 5 && view.toc()[1].level == 2);
    view.set_rect({0, 0, 10, 2});
    view.jump(view.toc()[1]);
    ui::Canvas canvas{{0, 0, 10, 2}};
    view.render(canvas);
    assert(canvas.texts().size() == 2 && canvas.texts().front().value == "More");
    assert(canvas.texts().front().style.attributes & 1U);
    assert(canvas.texts().back().value == "END" && !canvas.texts().back().style.attributes);
    assert(mdview::render("##literal") == "##literal\n");
    assert(mdview::render("# 猫猫猫", {.width = 4}) == "猫猫\n猫\n");
    assert(mdview::render("# \x1b[2J").find('\x1b') == std::string::npos);
}
