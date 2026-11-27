#include <TTyTk/TTyTk-PromptTools.hpp>
#include <cassert>
int main() {
    using namespace ttytk::prompt;
    SegmentList list{{"界", {255, 255, 255}, {10, 20, 30}}, {"fallback"}};
    list[1].evaluate = [] { return "e\u0301"; };
    assert(render(list, 1).empty());
    const auto narrow = render(list, 2);
    assert(narrow.ends_with("界\x1b[0m"));
    assert(narrow.find("\xee\x82\xb0") == std::string::npos);
    const auto wide = render(list, 4);
    assert(wide.find("38;2;10;20;30;48;2;0;0;0m\xee\x82\xb0") != std::string::npos);
    assert(wide.ends_with("e\u0301\x1b[0m"));
    assert(render({{"👩‍👩"}}, 2).ends_with("👩‍👩\x1b[0m"));
}
