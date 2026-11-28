#pragma once
#include "solver.hpp"
#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace smtlib2 {

/// @implstring Scoped C++ API for normalized Boolean terms over a fixed set
/// of canonical symbol names. Assertions are queued, encoded on demand, and
/// reused by later checks until popped. All assumptions (which may be Boolean
/// formulas) are temporary. Search starts fresh each time, and any SAT result
/// is independently checked against the original assertions and assumptions.
/// Unsupported terms or exhausted bounds produce unknown. This does not
/// provide SMT-LIB command processing, scoped declarations, or learned clauses.
class IncrementalBoolSolver {
public:
    explicit IncrementalBoolSolver(const std::vector<std::string>& variables = {},
                                   SatLimits limits = {},
                                   std::size_t max_variables = BoolSolver::default_max_variables)
        : cnf_(limits) {
        if (variables.size() > max_variables) {
            ready_ = false;
            return;
        }
        for (const auto& name : variables) {
            if (!cnf_.declare_symbol(name, max_variables)) {
                ready_ = false;
                return;
            }
        }
    }

    IncrementalBoolSolver(const IncrementalBoolSolver&) = default;
    IncrementalBoolSolver& operator=(const IncrementalBoolSolver& other) {
        if (this != &other) {
            // Do not replace any CNF state until the matching assertions
            // and rollback checkpoints have also been copied successfully.
            IncrementalBoolSolver copy(other);
            *this = std::move(copy);
        }
        return *this;
    }

    IncrementalBoolSolver(IncrementalBoolSolver&& other) noexcept
        : cnf_(std::move(other.cnf_)), ready_(other.ready_), encoded_(other.encoded_),
          assertions_(std::move(other.assertions_)), frames_(std::move(other.frames_)) {
        other.clear_moved_from();
    }

    IncrementalBoolSolver& operator=(IncrementalBoolSolver&& other) noexcept {
        if (this != &other) {
            cnf_ = std::move(other.cnf_);
            ready_ = other.ready_;
            encoded_ = other.encoded_;
            assertions_ = std::move(other.assertions_);
            frames_ = std::move(other.frames_);
            other.clear_moved_from();
        }
        return *this;
    }

    void push() { frames_.push_back(assertions_.size()); }

    // Create empty scopes as one operation; a failure leaves all scopes intact.
    void push_scopes(std::size_t count) {
        if (count > frames_.max_size() - frames_.size())
            throw std::length_error("Boolean scope limit exceeded");
        frames_.resize(frames_.size() + count, assertions_.size());
    }

    // Underflow leaves both assertions and encoding untouched.
    bool pop(std::size_t count = 1) {
        if (count > frames_.size()) return false;
        if (count == 0) return true;
        const auto remaining_frames = frames_.size() - count;
        const auto remaining_assertions = frames_[remaining_frames];
        if (remaining_assertions < encoded_) {
            cnf_.rollback(assertions_[remaining_assertions].encoding->before);
            encoded_ = remaining_assertions;
        }
        assertions_.resize(remaining_assertions);
        frames_.resize(remaining_frames);
        return true;
    }

    // An unsupported or unencodable assertion blocks checks until removed.
    void assert_formula(SExpr formula) { assertions_.push_back({std::move(formula), {}}); }

    // Number of active assertions whose Tseitin roots are already cached.
    std::size_t encoded_assertion_count() const noexcept { return encoded_; }

    SolveResult check(const std::vector<SExpr>& assumptions = {}) {
        if (!ready_) return {};
        const auto before = cnf_.checkpoint();
        const auto cached = encoded_;
        try {
            return check_impl(assumptions);
        } catch (...) {
            cnf_.rollback(before);
            while (encoded_ > cached) assertions_[--encoded_].encoding.reset();
            throw;
        }
    }

private:
    SolveResult check_impl(const std::vector<SExpr>& assumptions) {
        while (encoded_ < assertions_.size()) {
            auto& assertion = assertions_[encoded_];
            const auto before = cnf_.checkpoint();
            const auto root = cnf_.encode_root(assertion.formula);
            if (!root) return {};
            assertion.encoding = Encoding{before, root};
            ++encoded_;
        }

        // Keep all scoped clauses in place and append only what this check
        // needs. Retract temporary encoding on every exit, including unknown.
        const RestoreCnf temporary{cnf_, cnf_.checkpoint()};
        std::vector<sat_detail::Literal> roots;
        std::vector<const SExpr*> originals;
        for (const auto& assertion : assertions_) {
            roots.push_back(assertion.encoding->root);
            originals.push_back(&assertion.formula);
        }
        for (const auto& assumption : assumptions) {
            const auto root = cnf_.encode_root(assumption);
            if (!root) return {};
            roots.push_back(root);
            originals.push_back(&assumption);
        }
        return cnf_.solve(roots, originals);
    }

    using Cnf = sat_detail::Cnf;
    using Checkpoint = Cnf::Checkpoint;

    struct RestoreCnf {
        Cnf& cnf;
        Checkpoint before;
        ~RestoreCnf() { cnf.rollback(before); }
    };
    struct Encoding {
        Checkpoint before;
        sat_detail::Literal root;
    };
    struct Assertion {
        SExpr formula;
        std::optional<Encoding> encoding;
    };

    // CNF checkpoints and the cached prefix belong to the destination now.
    // The moved-from solver can be replaced but cannot check a partial CNF.
    void clear_moved_from() noexcept {
        ready_ = false;
        encoded_ = 0;
        assertions_.clear();
        frames_.clear();
    }

    Cnf cnf_;
    bool ready_ = true;
    std::size_t encoded_ = 0;
    std::vector<Assertion> assertions_;
    std::vector<std::size_t> frames_;
};

} // namespace smtlib2
