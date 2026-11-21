#include "../src/mir_verify.h"
#include "../src/qml_rewrite.hpp"
#include "cephyr/diagnostics.hpp"

#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>

namespace {

using cephyr::Diagnostics;
using cephyr::qml::Pipeline;

void check(bool result, const char *message, const Diagnostics &diag)
{
    if (!result) {
        std::cerr << message << '\n';
        diag.render(std::cerr);
        std::exit(1);
    }
}

bool contains(std::string_view text, std::string_view pattern)
{
    return text.find(pattern) != text.npos;
}

bool diagnosed(const Diagnostics &diag, std::string_view text)
{
    std::ostringstream out;
    diag.render(out);
    return contains(out.str(), text);
}

void valid(const std::string &mir, const Diagnostics &diag)
{
    ArchonBackendDiagnostic native{};
    check(cephyr_verify_mir(mir.data(), mir.size(), &native) == ARCHON_BACKEND_OK,
          native.message, diag);
}

// Exact Cephyr output for int f(int x) { return (x + 0) * 2; }.
constexpr std::string_view promoted_int = R"(archon-mir 1
module 1 0 0
function f i64 1 17 1 1
types i64 ptr i64 i64 i64 i64 i64 i64 i64 i64 i64 i64 i64 i64 i64 i64 i64
local 4 4
block 0 17
params
frame_addr 1 0
sext 2 0 32
store32 1 2 0
load_s32 3 1 0
sext 4 3 32
const 5 0
sext 6 4 32
sext 7 5 32
add 8 6 7
sext 9 8 32
const 10 2
sext 11 9 32
sext 12 10 32
mul 13 11 12
sext 14 13 32
sext 15 14 32
sext 16 15 32
ret 16
end
)";

constexpr std::string_view plan = R"(CEPHYR-QML-1
R canonicalize identity n add 2 m x i 0 n add 2 m x i 0
R canonicalize add_zero n add 2 m x i 0 m x
R optimize twice n mul 2 m x i 2 n add 2 m x m x
X sema no_multiply n mul 2 w w
P canonicalize
P optimize
)";

void run_promoted_int()
{
    Diagnostics diag;
    Pipeline pipeline;
    check(pipeline.add_compiled(plan, diag, "builtin.qmb"), "load plan", diag);
    check(pipeline.configure({}, 2, diag), "configure compiled default order", diag);
    std::string mir(promoted_int);
    valid(mir, diag);
    check(pipeline.rewrite(mir, diag, "example.c"), "rewrite promoted int", diag);
    std::string expected(promoted_int);
    expected.replace(expected.find("add 8 6 7"), 9, "copy 8 6");
    expected.replace(expected.find("mul 13 11 12"), 12, "add 13 11 11");
    check(mir == expected, "only the two pure integer instructions may change", diag);
    valid(mir, diag);
    check(pipeline.rewrite(mir, diag, "example.c") && mir == expected, "stable on rerun", diag);

    mir = promoted_int;
    bool changed = false;
    check(pipeline.rewrite_canonical(mir, diag, "example.c", &changed) && changed && mir == expected,
          "trusted producer path must agree with the verified path", diag);
    check(pipeline.rewrite_canonical(mir, diag, "example.c", &changed) && !changed && mir == expected,
          "trusted producer path must report an unchanged output", diag);

    check(pipeline.configure({"optimize"}, 1, diag), "select only optimize", diag);
    mir = promoted_int;
    check(pipeline.rewrite(mir, diag, "example.c") && contains(mir, "\nadd 8 6 7\n") &&
              contains(mir, "\nadd 13 11 11\n"),
          "unselected canonicalize must not run", diag);
}

void run_rejection_and_fuel()
{
    Diagnostics diag;
    Pipeline pipeline;
    check(pipeline.add_compiled(plan, diag, "sample.qmb"), "load plan", diag);
    check(pipeline.configure({}, 1, diag), "configure low global fuel", diag);
    std::string mir(promoted_int);
    check(!pipeline.rewrite(mir, diag, "example.c") && mir == promoted_int &&
              diagnosed(diag, "cephyr.qml.fuel"),
          "fuel exhaustion must roll back earlier phases", diag);
    bool changed = true;
    check(!pipeline.rewrite_canonical(mir, diag, "example.c", &changed) &&
              !changed && mir == promoted_int,
          "trusted producer path must report no change on failure", diag);

    Diagnostics reject_diag;
    check(pipeline.configure({"canonicalize"}, 20, reject_diag), "select canonicalize", reject_diag);
    check(!pipeline.rewrite(mir, reject_diag, "example.c") && mir == promoted_int &&
              diagnosed(reject_diag, "example.c:21") && diagnosed(reject_diag, "no_multiply"),
          "unselected final sema detects a rejected instruction and rolls back", reject_diag);

    Diagnostics order_diag;
    check(pipeline.configure({"sema", "optimize"}, 2, order_diag), "configure precheck", order_diag);
    check(!pipeline.rewrite(mir, order_diag, "example.c") && mir == promoted_int,
          "ordering must honor an explicit early sema check", order_diag);

    Pipeline loop;
    Diagnostics loop_diag;
    constexpr std::string_view cycle = R"(CEPHYR-QML-1
R opt left n add 2 m x m y n sub 2 m x m y
R opt right n sub 2 m x m y n add 2 m x m y
)";
    check(loop.add_compiled(cycle, loop_diag, "cycle.qmb"), "load cyclic rules", loop_diag);
    check(loop.configure({"opt"}, 99, loop_diag, {{"opt", 3}}), "configure phase cap", loop_diag);
    check(!loop.rewrite(mir, loop_diag, "example.c") && mir == promoted_int &&
              diagnosed(loop_diag, "phase 'opt' at rule 'right'"),
          "revisit replacement opcodes without bypassing the per-phase cap", loop_diag);
}

void run_constants()
{
    constexpr std::string_view text = R"(archon-mir 1
module 1 0 0
function f i64 1 12 1 0
types i64 i64 i64 i64 i64 i64 i64 i64 i64 i64 i64 i64
block 0 11
params
const 1 0x80
sext 2 1 8
const 3 64
ashr 4 2 3
zext 5 4 64
trunc 6 5 1
add 7 0 6
xor 8 7 7
add 9 8 6
sub 10 8 6
or 11 10 5
ret 11
end
)";
    constexpr std::string_view constants_plan = R"(CEPHYR-QML-1
R first same n xor 2 m x m x i 0
R first narrow n add 2 m x i 0 m x
R second signed n or 2 m x i -128 n or 2 i 18446744073709551488 m x
R second equal_constants_are_not_same_ssa n sub 2 m x m x i 42
P first
P second
)";
    Pipeline pipeline;
    Diagnostics diag;
    check(pipeline.add_compiled(constants_plan, diag, "constants.qmb") &&
              pipeline.configure({}, 4, diag),
          "literal bit patterns and matched RHS literals", diag);
    std::string mir(text);
    check(pipeline.rewrite(mir, diag, "constants.c") && contains(mir, "\nor 11 5 10\n"),
          "matched literals may reuse the original SSA ID in a replacement", diag);

    // A root literal emits a const. Wrapping shift by 64, sign extension
    // and truncation are used as facts, not emitted as edits themselves.
    constexpr std::string_view terminating = R"(CEPHYR-QML-1
R first same n xor 2 m x m x i 0
R first narrow n add 2 m x i 0 m x
R second signed n or 2 w i -128 i 18446744073709551488
R second equal_constants_are_not_same_ssa n sub 2 m x m x i 42
P first
P second
)";
    Pipeline other;
    Diagnostics other_diag;
    check(other.add_compiled(terminating, other_diag, "constants.qmb") &&
              other.configure({}, 4, other_diag), "configure constants", other_diag);
    mir = text;
    check(other.rewrite(mir, other_diag, "constants.c") &&
              contains(mir, "\ncopy 7 0\n") && contains(mir, "\nconst 8 0\n") &&
              contains(mir, "\ncopy 9 8\n") && contains(mir, "\nsub 10 8 6\n") &&
              contains(mir, "\nconst 11 18446744073709551488\n"),
          "normalization, recomputed facts and SSA identity", other_diag);
    valid(mir, other_diag);
}

void run_block_and_type_isolation()
{
    constexpr std::string_view text = R"(archon-mir 1
module 2 1 1
function f i64 2 7 2 0
types i64 ptr i64 ptr i64 i64 i64
block 0 4
params
const 2 0
copy 3 1
store 3 0 0
load 4 3 0
br 1 1 4
block 1 1
params 5
add 6 5 2
ret 6
function g i64 2 4 1 0
types i64 i64 i64 i64
block 0 2
params
call 2 ext 1 0
add 3 2 1
ret 3
import ext i64 1 i64
global data 1 init 18446744073709551615
end
)";
    Diagnostics diag;
    Pipeline pipeline;
    check(pipeline.add_compiled("CEPHYR-QML-1\nR opt add_zero n add 2 m x i 0 m x\n"
                               "X sema integer_copy n copy 1 w\n", diag, "local.qmb") &&
              pipeline.configure({}, 2, diag),
          "configure conservative local-only checks", diag);
    std::string mir(text);
    check(pipeline.rewrite(mir, diag, "local.c") && mir == text,
          "preserve ptr copy, control flow, calls and cross-block uses verbatim", diag);
}

void run_bitwise_and_shifts()
{
    constexpr std::string_view text = R"(archon-mir 1
module 1 0 0
function f i64 1 7 1 0
types i64 i64 i64 i64 i64 i64 i64
block 0 6
params
const 1 0
or 2 0 1
xor 3 2 1
shl 4 3 1
lshr 5 4 1
ashr 6 5 1
ret 6
end
)";
    constexpr std::string_view rules = R"(CEPHYR-QML-1
R opt or_zero n or 2 m x i 0 m x
R opt xor_zero n xor 2 m x i 0 m x
R opt shl_zero n shl 2 m x i 0 m x
R opt lshr_zero n lshr 2 m x i 0 m x
R opt ashr_zero n ashr 2 m x i 0 m x
)";
    Pipeline pipeline;
    Diagnostics diag;
    check(pipeline.add_compiled(rules, diag, "bitwise.qmb") && pipeline.configure({}, 5, diag),
          "configure native bitwise and shift operators", diag);
    std::string mir(text);
    check(pipeline.rewrite(mir, diag, "bitwise.c") &&
              contains(mir, "\ncopy 2 0\ncopy 3 2\ncopy 4 3\ncopy 5 4\ncopy 6 5\nret 6\n"),
          "all bitwise and shift identities must preserve original SSA IDs", diag);
    valid(mir, diag);
}

void run_canonical_flag()
{
    constexpr std::string_view text = R"(archon-mir 1
module 1 0 0
function f i64 1 2 1 0
types i64 i64
block 0 1
params
or 1 0 0
ret 1
end
)";
    constexpr std::string_view swap = R"(CEPHYR-QML-1
R forward replace n or 2 m x m x n and 2 m x m x
R backward restore n and 2 m x m x n or 2 m x m x
P forward
P backward
)";
    Pipeline pipeline;
    Diagnostics diag;
    check(pipeline.add_compiled(swap, diag, "swap.qmb") && pipeline.configure({}, 2, diag),
          "configure reversible phases", diag);
    std::string mir(text);
    bool changed = true;
    check(pipeline.rewrite_canonical(mir, diag, "swap.c", &changed) && !changed && mir == text,
          "report exact final byte changes rather than intermediate edits", diag);

    Diagnostics limit_diag;
    constexpr std::string_view oversized = "archon-mir 1\nmodule 4294967295 0 0\nend\n";
    mir = oversized;
    changed = true;
    check(!pipeline.rewrite_canonical(mir, limit_diag, "large.mir", &changed) &&
              !changed && mir == oversized,
          "trusted path must not allocate unchecked record counts", limit_diag);

    Pipeline empty;
    Diagnostics empty_diag;
    check(empty.configure({}, 1, empty_diag) && !empty.rewrite(mir, empty_diag, "large.mir"),
          "default path must verify input even when no phase applies", empty_diag);
}

void run_integer_guards()
{
    constexpr std::string_view text = R"(archon-mir 1
module 1 0 0
function f i64 2 21 2 0
types i64 i64 i64 i64 i64 i64 i64 i64 i64 i64 i64 i64 i64 i64 i64 i64 i64 i64 i64 i64 i64
block 0 17
params
const 2 0
const 3 1
const 4 0x8000000000000000
const 5 255
const 6 3
rem_u 7 0 2
rem_u 8 0 3
rem_u 9 0 4
rem_u 10 0 6
rem_u 11 0 1
rem_s 12 0 4
zext 13 0 8
sext 14 0 8
and 15 13 5
and 16 5 13
and 17 14 5
and 18 13 6
br 1 0
block 0 2
params
rem_u 19 0 4
and 20 13 5
ret 20
end
)";
    constexpr std::string_view rules = R"(CEPHYR-QML-1
R opt remainder n rem_u 2 m x n pow2 1 m d n low_bits_for_pow2 2 m x m d
R opt mask_right n and 2 n masked_by 2 m x m mask m mask m x
R opt mask_left n and 2 m mask n masked_by 2 m x m mask m x
)";
    Pipeline pipeline;
    Diagnostics diag;
    check(pipeline.add_compiled(rules, diag, "integer.qmb") &&
              pipeline.configure({}, 4, diag), "configure guarded native rules", diag);
    std::string mir(text);
    auto expected = mir;
    const auto replace = [&](std::string_view before, std::string_view after) {
        expected.replace(expected.find(before), before.size(), after);
    };
    replace("\nrem_u 8 0 3\n", "\nconst 8 0\n");
    replace("\nrem_u 9 0 4\n", "\ntrunc 9 0 63\n");
    replace("\nand 15 13 5\n", "\ncopy 15 13\n");
    replace("\nand 16 5 13\n", "\ncopy 16 13\n");
    check(pipeline.rewrite(mir, diag, "integer.mir") && mir == expected,
          "only proven unsigned remainder and redundant masks may change", diag);
    valid(mir, diag);
    check(pipeline.rewrite(mir, diag, "integer.mir") && mir == expected,
          "guarded rewrites must be stable", diag);
}

void run_bit_mask_guards()
{
    constexpr std::string_view text = R"(archon-mir 1
module 1 1 0
function f i64 2 20 2 0
types i64 i64 i64 i64 i64 i64 i64 i64 i64 i64 i64 i64 i64 i64 i64 i64 i64 i64 i64 i64
block 0 17
params
call 2 source 0
zext 3 2 8
const 4 255
or 5 1 4
or 6 3 5
or 7 5 3
const 8 63
shl 9 0 8
zext 10 1 63
and 11 9 10
and 12 10 9
sext 13 2 8
and 14 9 13
or 15 13 5
and 16 0 3
const 17 127
or 18 3 17
br 1 0
block 0 1
params
and 19 9 10
ret 19
import source i64 0
end
)";
    constexpr std::string_view rules = R"(CEPHYR-QML-1
R integer_reduce redundant_or_right n or 2 n masked_by 2 m x m mask m mask m mask
R integer_reduce redundant_or_left n or 2 m mask n masked_by 2 m x m mask m mask
R integer_reduce disjoint_and n and 2 n disjoint_bits 2 m x m y m y i 0
)";
    Pipeline pipeline;
    Diagnostics diag;
    check(pipeline.add_compiled(rules, diag, "bit-mask.qmb") &&
              pipeline.configure({"integer_reduce"}, 4, diag), "configure bit-mask guards", diag);
    std::string mir(text);
    auto expected = mir;
    const auto replace = [&](std::string_view before, std::string_view after) {
        expected.replace(expected.find(before), before.size(), after);
    };
    replace("\nor 6 3 5\n", "\ncopy 6 5\n");
    replace("\nor 7 5 3\n", "\ncopy 7 5\n");
    replace("\nand 11 9 10\n", "\nconst 11 0\n");
    replace("\nand 12 10 9\n", "\nconst 12 0\n");
    check(pipeline.rewrite(mir, diag, "bit-mask.mir") && mir == expected,
          "preserve the producer call, overlaps, unknown bits and cross-block uses", diag);
    valid(mir, diag);
    check(pipeline.rewrite(mir, diag, "bit-mask.mir") && mir == expected,
          "bit-mask rewrites must be stable", diag);
}

void run_malformed_inputs()
{
    constexpr std::string_view bad[] = {
        "NOT-QML\n",
        "CEPHYR-QML-1\nR p nested n add 2 n sub 2 m a m b m c m a\n",
        "CEPHYR-QML-1\nR p nested n add 2 m a m b n sub 2 n add 2 m a m b m a\n",
        "CEPHYR-QML-1\nR p calls n add 2 m a m b n call 2 m a m b\n",
        "CEPHYR-QML-1\nR p calls n call 2 m a m b m a\n",
        "CEPHYR-QML-1\nR p traps n add_checked_s 2 m a m b m a\n",
        "CEPHYR-QML-1\nR p unguarded n rem_u 2 m a m b m a\n",
        "CEPHYR-QML-1\nR p new_trap n add 2 m a n pow2 1 m b n rem_u 2 m a m b\n",
        "CEPHYR-QML-1\nR p no_pow2 n add 2 m a m b n low_bits_for_pow2 2 m a m b\n",
        "CEPHYR-QML-1\nR p no_mask n and 2 n masked_by 2 m a m c m b m a\n",
        "CEPHYR-QML-1\nR p unbound_peer n and 2 n disjoint_bits 2 m a m c m b i 0\n",
        "CEPHYR-QML-1\nR p literal_peer n and 2 n disjoint_bits 2 m a i 0 m b i 0\n",
        "CEPHYR-QML-1\nR p bad_guard n mul 2 m a n pow2 1 i 2 m a\n",
        "CEPHYR-QML-1\nR p wrong_arity n sub 1 m a m a\n",
        "CEPHYR-QML-1\nR p unbound n add 2 m a m b m c\n",
        "CEPHYR-QML-1\nR p new_literal n add 2 m a m b n mul 2 m a i 7\n",
        "CEPHYR-QML-1\nR p large_literal n add 2 m a m b i 18446744073709551616\n",
        "CEPHYR-QML-1\nR p large_literal n add 2 m a m b i -9223372036854775809\n",
        "CEPHYR-QML-1\nR p wild n add 2 m a m b w\n",
        "CEPHYR-QML-1\nR p symbols n add 2 m a m b s alpha\n",
        "CEPHYR-QML-1\nR p booleans n add 2 m a b 1 m a\n",
        "CEPHYR-QML-1\nR p tail n add 2 m a m b m a extra\n",
        "CEPHYR-QML-1\nR sema rewrite n add 2 m a m b m a\n",
        "CEPHYR-QML-1\nX p tail n add 2 w w i 0\n",
        "CEPHYR-QML-1\nP p extra\n",
    };
    Diagnostics initial;
    Pipeline pipeline;
    check(pipeline.add_compiled(plan, initial, "builtin.qmb") &&
              pipeline.configure({}, 2, initial), "configure prior plan", initial);
    for (auto text : bad) {
        Diagnostics diag;
        check(!pipeline.add_compiled(text, diag, "bad.qmb") && diagnosed(diag, "bad.qmb:"),
              "invalid compiled plan must be diagnosed", diag);
    }
    std::string too_many("CEPHYR-QML-1\n");
    for (unsigned i = 0; i < 1025; ++i)
        too_many += "X p excessive n add 2 w w\n";
    Diagnostics cap_diag;
    check(!pipeline.add_compiled(too_many, cap_diag, "large.qmb") &&
              diagnosed(cap_diag, "rule limit"),
          "bound the number of compiled rules", cap_diag);
    Diagnostics invalid_phase;
    check(!pipeline.configure({"no_such_phase"}, 1, invalid_phase) &&
              diagnosed(invalid_phase, "unknown native QML phase"),
          "configuration fails for unknown phase", invalid_phase);
    Diagnostics prior_diag;
    std::string mir(promoted_int);
    check(pipeline.rewrite(mir, prior_diag, "example.c"), "old plan survives bad changes", prior_diag);

    constexpr std::string_view split_call = R"(archon-mir 1
module 1 1 0
function f i64 1 3 1 0
types i64 i64 i64
block 0 2
params
call 1
add 1 0 add 2 1 1
ret 2
import add i64 1 i64
end
)";
    mir = split_call;
    valid(mir, prior_diag);
    Diagnostics format_diag;
    check(!pipeline.rewrite(mir, format_diag, "split.mir") && mir == split_call &&
              diagnosed(format_diag, "line-oriented"),
          "valid native text with split opcodes must fail closed", format_diag);
    mir = promoted_int;
    mir.replace(mir.find("add 8 6 7"), 9, "add 8 8 7");
    const std::string malformed = mir;
    Diagnostics mir_diag;
    check(!pipeline.rewrite(mir, mir_diag, "malformed.mir") && mir == malformed &&
              diagnosed(mir_diag, "invalid input MachineIR"),
          "native verifier rejects use before definition before any mutation", mir_diag);
}

} // namespace

int main()
{
    run_promoted_int();
    run_rejection_and_fuel();
    run_constants();
    run_block_and_type_isolation();
    run_bitwise_and_shifts();
    run_canonical_flag();
    run_integer_guards();
    run_bit_mask_guards();
    run_malformed_inputs();
}
