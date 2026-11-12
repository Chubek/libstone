#include <cassert>
#include <chisel/Chisel.hpp>

using namespace moonstone::chisel;
using moonstone::Status;

int main() {
    Function ok{{{0,
                  {{Op::Create, 1, 0, {}},
                   {Op::BorrowShared, 1, 10, {}},
                   {Op::Use, 1, 0, {}},
                   {Op::EndBorrow, 1, 10, {}},
                   {Op::Move, 1, 0, {}}},
                  {}}},
                0};
    auto result = analyze(ok);
    assert(result.converged && result.diagnostics.empty() && result.status == Status::Proven);
    Function bad{{{0,
                   {{Op::Create, 1, 0, {}},
                    {Op::BorrowShared, 1, 10, {}},
                    {Op::BorrowUnique, 1, 11, {}},
                    {Op::Move, 1, 0, {}}},
                   {}}},
                 0};
    auto errors = analyze(bad);
    assert(errors.diagnostics.size() == 2 && errors.status == Status::Refuted);
    Function branch{{{0, {{Op::Create, 1, 0, {}}}, {1, 2}},
                     {1, {{Op::Move, 1, 0, {}}}, {3}},
                     {2, {{Op::Use, 1, 0, {}}}, {3}},
                     {3, {{Op::Use, 1, 0, {}}}, {}}},
                    0};
    assert(analyze(branch).status == Status::Refuted);
    branch.blocks.back().instructions = {{Op::Reinitialize, 1, 0, {}}};
    assert(analyze(branch).status == Status::Refuted); // May still be live on path 2.

    Function unique{{{0,
                      {{Op::Create, 1, 0, {}},
                       {Op::BorrowUnique, 1, 10, {}},
                       {Op::Use, 1, 0, {}},
                       {Op::Use, 1, 0, {}}},
                      {}}},
                    0};
    auto duplicates = analyze(unique);
    assert(duplicates.diagnostics.size() == 2); // Distinct instructions, same unknown source.
    unique.blocks[0].instructions = {{Op::Create, 1, 0, {}},
                                     {Op::BorrowUnique, 1, 10, {}},
                                     {Op::EndBorrow, 1, 10, {}},
                                     {Op::Use, 1, 0, {}}};
    assert(analyze(unique).status == Status::Proven);
    unique.blocks[0].instructions = {{static_cast<Op>(999), 1, 0, {}}};
    assert(analyze(unique).status == Status::InvalidInput);
    unique.blocks[0].successors = {42};
    assert(analyze(unique).status == Status::InvalidInput);
    assert(analyze(Function{}).status == Status::InvalidInput);
    Function duplicate_blocks{{{0, {}, {}}, {0, {}, {}}}, 0};
    assert(analyze(duplicate_blocks).status == Status::InvalidInput);

    Policy limit;
    limit.iteration_limit = 0;
    auto exhausted = analyze(ok, limit);
    assert(!exhausted.converged && exhausted.status == Status::BudgetExhausted);
    Function loop{{{0, {{Op::Create, 1, 0, {}}}, {1}},
                   {1, {{Op::Use, 1, 0, {}}}, {1, 2}},
                   {2, {{Op::Move, 1, 0, {}}}, {}},
                   {3, {{Op::Use, 99, 0, {}}}, {}}},
                  0};
    auto converged = analyze(loop);
    assert(converged.status == Status::Proven && converged.converged);
    assert(!converged.block_out.count(3)); // Unreachable errors are not emitted.
    loop.blocks[1].instructions = {{Op::Move, 1, 0, {}}};
    assert(analyze(loop).status == Status::Refuted);
    limit.iteration_limit = 1;
    assert(analyze(loop, limit).status == Status::BudgetExhausted);

    State left, right;
    left.places[1] = {Life::Live, {10}};
    left.tokens[10] = {1, false};
    right.places[1] = {Life::Moved, {}};
    auto joined = join(left, right);
    assert(joined.places[1].life == Life::MaybeInvalid && joined.places[1].loans.count(10));
    assert(join(left, left) == left);
    assert(join(left, right) == join(right, left));
    assert(join(joined, left) == joined);
    // Different static loans cannot share an identity at a CFG join.
    right.tokens[10] = {2, true};
    bool rejected = false;
    try {
        (void)join(left, right);
    } catch (const std::invalid_argument &) {
        rejected = true;
    }
    assert(rejected);
}
