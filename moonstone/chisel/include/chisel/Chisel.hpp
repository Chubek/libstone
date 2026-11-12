#pragma once
#include "../../../core/Core.hpp"
#include <deque>

namespace moonstone::chisel {

using Id = std::uint32_t;
enum class Op {
    Create,
    Use,
    Mutate,
    BorrowShared,
    BorrowUnique,
    EndBorrow,
    Move,
    Drop,
    Reinitialize,
    Escape
};
struct Instruction {
    Op op = Op::Use;
    Id place = 0;
    Id token = 0;
    SourceSpan source;
};
struct Block {
    Id id = 0;
    std::vector<Instruction> instructions;
    std::vector<Id> successors;
};
struct Function {
    std::vector<Block> blocks;
    Id entry = 0;
};
struct Policy {
    bool allow_shared_borrows = true;
    bool allow_unique_borrows = true;
    bool allow_mutation_while_shared = false;
    bool allow_escape_of_borrow = false;
    bool implicit_copy = false;
    std::size_t iteration_limit = 100000;
};
enum class Life { Uninitialized, Live, Moved, Dropped, MaybeInvalid };
struct Loan {
    Id place = 0;
    bool unique = false;
    bool operator==(const Loan &) const = default;
};
struct PlaceState {
    Life life = Life::Uninitialized;
    std::set<Id> loans;
    bool operator==(const PlaceState &) const = default;
};
struct State {
    std::map<Id, PlaceState> places;
    std::map<Id, Loan> tokens;
    bool operator==(const State &) const = default;
};
struct Result {
    Diagnostics diagnostics;
    bool converged = true;
    std::map<Id, State> block_in;
    std::map<Id, State> block_out;
    // Proven means acceptance by this explicit-loan policy ONLY, not memory safety.
    Status status = Status::Unknown;
    std::string failure_reason;
};

inline PlaceState at(const State &state, Id place) {
    auto it = state.places.find(place);
    return it == state.places.end() ? PlaceState{} : it->second;
}

inline State join(const State &a, const State &b) {
    State result = a;
    for (auto &[id, value] : result.places) {
        if (!b.places.count(id) && value.life != Life::Uninitialized)
            value.life = Life::MaybeInvalid;
    }
    for (const auto &[id, value] : b.places) {
        auto [it, inserted] = result.places.try_emplace(id);
        (void)inserted;
        auto &target = it->second;
        if (target.life != value.life)
            target.life = Life::MaybeInvalid;
        target.loans.insert(value.loans.begin(), value.loans.end());
    }
    for (const auto &[id, loan] : b.tokens) {
        auto [it, inserted] = result.tokens.emplace(id, loan);
        if (!inserted && it->second != loan)
            throw std::invalid_argument("borrow token reused for incompatible loans");
    }
    return result;
}

inline Result analyze(const Function &function, Policy policy = {}) {
    Result result;
    std::map<Id, const Block *> blocks;
    auto invalid = [&](std::string reason) {
        result.status = Status::InvalidInput;
        result.converged = false;
        result.failure_reason = std::move(reason);
        return result;
    };
    for (const auto &block : function.blocks) {
        if (!blocks.emplace(block.id, &block).second)
            return invalid("duplicate basic block");
        for (const auto &instruction : block.instructions) {
            switch (instruction.op) {
            case Op::Create:
            case Op::Use:
            case Op::Mutate:
            case Op::BorrowShared:
            case Op::BorrowUnique:
            case Op::EndBorrow:
            case Op::Move:
            case Op::Drop:
            case Op::Reinitialize:
            case Op::Escape:
                break;
            default:
                return invalid("unsupported Chisel operation"); // Never a no-op.
            }
        }
    }
    if (!blocks.count(function.entry))
        return invalid("entry block missing");
    for (const auto &block : function.blocks) {
        for (auto successor : block.successors)
            if (!blocks.count(successor))
                return invalid("invalid successor");
    }
    auto transfer = [&](const Block &block, State state, bool report) {
        auto error = [&](std::string code, std::string message, const Instruction &instruction) {
            if (report)
                result.diagnostics.push_back({std::move(code),
                                              std::move(message),
                                              Severity::Error,
                                              instruction.source,
                                              {},
                                              {},
                                              {}});
        };
        for (const auto &instruction : block.instructions) {
            auto &place = state.places[instruction.place];
            auto require_live = [&]() {
                if (place.life == Life::Live)
                    return true;
                error("CHISEL-INVALID-LIFE",
                      "use of uninitialized, moved, dropped or possibly invalid place",
                      instruction);
                return false;
            };
            bool shared = false, unique = false;
            for (auto token : place.loans) {
                auto found = state.tokens.find(token);
                if (found != state.tokens.end()) {
                    if (found->second.unique)
                        unique = true;
                    else
                        shared = true;
                }
            }
            switch (instruction.op) {
            case Op::Create:
            case Op::Reinitialize:
                // MaybeInvalid includes a possibly LIVE path; overwriting it is not safe.
                if (place.life == Life::Live || place.life == Life::MaybeInvalid ||
                    !place.loans.empty()) {
                    error("CHISEL-REINIT", "reinitializing a live, possibly live or borrowed place",
                          instruction);
                } else
                    place.life = Life::Live;
                break;
            case Op::Use:
                if (require_live() && unique)
                    error("CHISEL-USE-UNIQUE", "owner access conflicts with exclusive loan",
                          instruction);
                break;
            case Op::Mutate:
                if (require_live() && (unique || (shared && !policy.allow_mutation_while_shared))) {
                    error("CHISEL-MUTATE-LOAN", "mutation conflicts with active borrow",
                          instruction);
                }
                break;
            case Op::BorrowShared:
                if (!require_live())
                    break;
                if (!policy.allow_shared_borrows || unique) {
                    error("CHISEL-SHARED-CONFLICT",
                          "shared borrow conflicts with exclusive loan or policy", instruction);
                } else if (state.tokens.count(instruction.token)) {
                    error("CHISEL-TOKEN-REUSED", "borrow token already active", instruction);
                } else {
                    place.loans.insert(instruction.token);
                    state.tokens[instruction.token] = {instruction.place, false};
                }
                break;
            case Op::BorrowUnique:
                if (!require_live())
                    break;
                if (!policy.allow_unique_borrows || !place.loans.empty()) {
                    error("CHISEL-UNIQUE-CONFLICT",
                          "mutable borrow conflicts with active loan or policy", instruction);
                } else if (state.tokens.count(instruction.token)) {
                    error("CHISEL-TOKEN-REUSED", "borrow token already active", instruction);
                } else {
                    place.loans.insert(instruction.token);
                    state.tokens[instruction.token] = {instruction.place, true};
                }
                break;
            case Op::EndBorrow: {
                auto found = state.tokens.find(instruction.token);
                if (found == state.tokens.end() || found->second.place != instruction.place) {
                    error("CHISEL-END-UNKNOWN", "ending an unknown or mismatched loan",
                          instruction);
                } else {
                    place.loans.erase(instruction.token);
                    state.tokens.erase(found);
                }
                break;
            }
            case Op::Move:
            case Op::Drop:
                if (!require_live())
                    break;
                if (!place.loans.empty()) {
                    error(instruction.op == Op::Move ? "CHISEL-MOVE-BORROWED"
                                                     : "CHISEL-DROP-BORROWED",
                          "cannot move or drop borrowed place", instruction);
                } else if (instruction.op == Op::Drop)
                    place.life = Life::Dropped;
                else if (!policy.implicit_copy)
                    place.life = Life::Moved;
                break;
            case Op::Escape:
                if (require_live() && !policy.allow_escape_of_borrow && !place.loans.empty()) {
                    error("CHISEL-ESCAPE", "borrowed place may escape its lifetime", instruction);
                }
                break;
            }
        }
        return state;
    };

    std::deque<Id> queue{function.entry};
    std::set<Id> queued{function.entry}, reached{function.entry};
    std::size_t steps = 0;
    while (!queue.empty()) {
        if (steps == policy.iteration_limit) {
            result.converged = false;
            result.status = Status::BudgetExhausted;
            result.failure_reason = "Chisel iteration limit";
            return result;
        }
        ++steps;
        auto id = queue.front();
        queue.pop_front();
        queued.erase(id);
        auto state = transfer(*blocks.at(id), result.block_in[id], false);
        result.block_out[id] = state;
        for (auto successor : blocks.at(id)->successors) {
            bool fresh = reached.insert(successor).second;
            State incoming;
            try {
                incoming = fresh ? state : join(result.block_in[successor], state);
            } catch (const std::invalid_argument &error) {
                return invalid(error.what());
            }
            if (fresh || incoming != result.block_in[successor]) {
                result.block_in[successor] = std::move(incoming);
                if (queued.insert(successor).second)
                    queue.push_back(successor);
            }
        }
    }
    // Emit only from the final conservative inputs of reachable blocks. Each
    // instruction is distinct even when no source location was supplied.
    for (auto id : reached)
        transfer(*blocks.at(id), result.block_in[id], true);
    result.status = result.diagnostics.empty() ? Status::Proven : Status::Refuted;
    return result;
}

} // namespace moonstone::chisel
