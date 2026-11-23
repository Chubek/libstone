#include "../src/blackstone_opt.hpp"
#include "cephyr/diagnostics.hpp"

#include <iostream>

int main()
{
    // Preserve the call (whose symbol happens to be an opcode) and two
    // distinct loads. Only integer result definitions below may change.
    const std::string original = R"(archon-mir 1
module 1 1 0
function f i64 2 18 2 0
types i64 ptr i64 i64 i64 i64 i64 i64 i64 i64 i64 i64 i64 i64 i64 i64 i64 i64
block 0 14
params
const 2 255
sext 3 2 8
const 4 1
add 5 3 4
load 6 1 0
load 7 1 0
sub 8 6 7
call 9
add 1 8
xor 10 9 9
add_checked_s 11 3 4
div_s 12 2 5
mul 13 9 4
or 14 13 5
and 15 0 0
br 1 1 14
block 1 1
params 16
add 17 16 10
ret 17
import add i64 1 i64
end
)";
    std::string expected = original;
    for (auto [from, to] : {std::pair<std::string_view, std::string_view>
                                {"2 18 2 0", "2 15 2 0"},
                            {" i64 i64 i64\nblock 0 14", "\nblock 0 11"},
                            {"add 5 3 4", "const 5 0"},
                            {"xor 10 9 9", "const 10 0"},
                            {"mul 13 9 4\nor 14 13 5\nand 15 0 0\n", ""},
                            {"br 1 1 14", "br 1 1 9"},
                            {"params 16\nadd 17 16 10\nret 17", "params 13\nadd 14 13 10\nret 14"}})
        expected.replace(expected.find(from), from.size(), to);
    cephyr::Diagnostics diag;
    bool changed = false;
    std::string mir = original;
    if (!cephyr::blackstone_opt::rewrite(mir, diag, "test.mir", changed) ||
        !changed || mir != expected ||
        !cephyr::blackstone_opt::rewrite(mir, diag, "test.mir", changed) ||
        changed || mir != expected) {
        diag.render(std::cerr);
        std::cerr << mir;
        return 1;
    }
    // Matching a shape can be reused, but the IDs and the bit patterns used
    // to emit its replacement must come from this occurrence.
    std::string repeated = R"(archon-mir 1
module 1 0 0
function repeated i64 2 14 1 0
types i64 i64 i64 i64 i64 i64 i64 i64 i64 i64 i64 i64 i64 i64
block 0 12
params
const 2 123
const 3 456
mul 4 2 3
mul 5 3 4
add 6 0 2
add 7 1 3
xor 8 0 1
xor 9 1 1
const 10 0
sub 11 10 3
mul 12 10 3
or 13 12 2
ret 13
end
)";
    // Keep the two folded products and wrapping subtraction observable.
    repeated.replace(repeated.find("ret 13"), 6, "call - sink 4 4 5 11 13\nret 13");
    repeated.replace(repeated.find("block 0 12"), 10, "block 0 13");
    repeated.replace(repeated.find("module 1 0 0"), 12, "module 1 1 0");
    repeated.insert(repeated.find("end\n"), "import sink void 4 i64 i64 i64 i64\n");
    expected = R"(archon-mir 1
module 1 1 0
function repeated i64 2 10 1 0
types i64 i64 i64 i64 i64 i64 i64 i64 i64 i64
block 0 9
params
const 2 123
const 3 456
const 4 56088
const 5 25576128
add 6 0 2
add 7 1 3
xor 8 0 1
const 9 18446744073709551160
call - sink 4 4 5 9 2
ret 2
import sink void 4 i64 i64 i64 i64
end
)";
    if (!cephyr::blackstone_opt::rewrite(repeated, diag, "repeated.mir", changed) ||
        !changed || repeated != expected) {
        diag.render(std::cerr);
        std::cerr << repeated;
        return 1;
    }
    // Floating addition must preserve signed zero and FP status; pointer
    // arithmetic must preserve pointer types even for a zero displacement.
    const std::string typed = R"(archon-mir 1
module 2 0 0
function fp f64 1 3 1 0
types f64 f64 f64
block 0 2
params
fconst 1 0
fadd 2 0 1
ret 2
function ptr ptr 1 3 1 0
types ptr i64 ptr
block 0 2
params
const 1 0
ptr_add 2 0 1
ret 2
end
)";
    mir = typed;
    if (!cephyr::blackstone_opt::rewrite(mir, diag, "typed.mir", changed) ||
        changed || mir != typed) {
        diag.render(std::cerr);
        std::cerr << mir;
        return 1;
    }
    // The producer can occur later in the serialized block order. Both
    // edges pass its full-width identity; the actual narrowing must remain.
    mir = R"(archon-mir 1
module 1 0 0
function backward i64 1 6 3 0
types i64 i64 i64 i64 i64 i64
block 0 0
params
br 2 0
block 1 2
params 3
add 4 3 2
trunc 5 4 8
ret 5
block 0 2
params
sext 2 0 64
copy 1 2
cbr 1 1 1 1 1 1 2
end
)";
    expected = R"(archon-mir 1
module 1 0 0
function backward i64 1 4 3 0
types i64 i64 i64 i64
block 0 0
params
br 2 0
block 1 2
params 1
add 2 1 0
trunc 3 2 8
ret 3
block 0 0
params
cbr 0 1 1 0 1 1 0
end
)";
    if (!cephyr::blackstone_opt::rewrite(mir, diag, "backward.mir", changed) ||
        !changed || mir != expected) {
        diag.render(std::cerr);
        std::cerr << mir;
        return 1;
    }
    auto invalid = original;
    invalid.replace(invalid.find("add 17 16 10"), 12, "add 17 1 10"); // ptr operand
    mir = invalid;
    changed = true;
    if (cephyr::blackstone_opt::rewrite(mir, diag, "invalid.mir", changed) ||
        changed || mir != invalid || !diag.error_count()) {
        std::cerr << "expected invalid typed input to be rejected without edits\n";
        return 1;
    }
    return 0;
}
