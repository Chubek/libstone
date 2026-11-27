#include <TTyUtils/TTyUtils-Diff.hpp>
#include <cassert>
#include <random>
#include <string>

int main() {
    ttyutils::textbuf::Buffer old_buffer{"a\nb\n"};
    ttyutils::textbuf::Buffer new_buffer{"a\nc\n"};
    const auto hunks = ttyutils::diff::lines(old_buffer.snapshot(), new_buffer.snapshot());
    assert(!hunks.empty());
    const auto merged = ttyutils::diff::merge3(old_buffer.snapshot(), new_buffer.snapshot(),
                                                old_buffer.snapshot());
    assert(merged.conflicts.empty());
    using ttyutils::textbuf::Buffer;
    using namespace ttyutils::diff;
    Buffer before{"a\nb\nc\nd\n"}, after{"a\nB\nc\nd"};
    const auto patch = Patch::unified(lines(before.snapshot(), after.snapshot()), 0);
    assert(patch.str() == "@@ -2 +2 @@\n-b\n+B\n@@ -4 +4 @@\n-d\n+d\n\\ No newline at end of file\n");
    Buffer wrong{"a\nx\nc\nd\n"};
    assert(!patch.apply(wrong) && wrong.text() == "a\nx\nc\nd\n");
    assert(patch.apply(before) && before.text() == after.text());
    const auto word_changes = words("hello world!", "hello editor!");
    assert(word_changes.size() == 4 && word_changes.front().content.front() == "hello");
    Buffer ancestor{"a\nb\nc\n"}, ours{"A\nb\nc\n"}, theirs{"a\nb\nC\n"};
    const auto clean = merge3(ancestor.snapshot(), ours.snapshot(), theirs.snapshot());
    assert(clean.conflicts.empty() && clean.result.text() == "A\nb\nC\n");
    const auto conflict = merge3(ancestor.snapshot(), ours.snapshot(), Buffer{"X\nb\nC\n"}.snapshot());
    assert(conflict.conflicts.size() == 1 && conflict.conflicts[0].base == "a\n");
    assert(conflict.result.text() == "<<<<<<< ours\nA\n=======\nX\n>>>>>>> theirs\nb\nC\n");
    std::string old_large, new_large;
    for (int i = 0; i < 600; ++i) {
        old_large += i == 300 ? "anchor\n" : "old\n";
        new_large += i == 300 ? "anchor\n" : "new\n";
    }
    Buffer old_fallback{old_large}, new_fallback{new_large};
    const auto large = lines(old_fallback.snapshot(), new_fallback.snapshot());
    assert(large.size() == 5 && large[2].kind == Hunk::Kind::equal);
    assert(Patch{large}.apply(old_fallback) && old_fallback.text() == new_large);
    std::mt19937 random{37};
    const auto make = [&] {
        std::string output;
        const auto count = random() % 12;
        for (std::size_t i = 0; i < count; ++i) {
            output += static_cast<char>('a' + random() % 4);
            if (i + 1 != count || random() % 2) output += '\n';
        }
        return output;
    };
    for (int i = 0; i < 300; ++i) {
        Buffer left{make()}, right{make()};
        assert(Patch{lines(left.snapshot(), right.snapshot())}.apply(left) && left.text() == right.text());
    }
    return 0;
}
