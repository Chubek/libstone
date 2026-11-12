#include <core/Diagnostics.hpp>
#include <core/Moonstone.hpp>
#include <core/SourceManager.hpp>

int main() {
    moonstone::SourceManager sources;
    auto id = sources.add("consumer.ex", "a\nb");
    if (!id)
        return 1;
    auto span = sources.span(id.value(), 0, 3);
    if (!span || span.value().end_line != 2)
        return 2;
    auto changed = moonstone::apply_edits("abc", {{0, 1, "x"}});
    return changed && *changed == "xbc" ? 0 : 3;
}
