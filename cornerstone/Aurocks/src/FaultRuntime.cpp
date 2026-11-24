#include "aurocks/Fault.h"

#include "DiagnosticsEngine.hpp"

#include <cstdio>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace lr = firestone::lr;
constexpr std::size_t inputLimit = (1U << 20) - 1;
constexpr std::size_t messageLimit = 4096;
constexpr std::size_t retryLimit = 8;
constexpr std::size_t ruleLimit = 64;
constexpr std::size_t matchBudget = 8U << 20;

std::string_view boundedText(const char* text, std::size_t limit) {
    if (!text) throw std::invalid_argument("null text");
    std::size_t length = 0;
    while (length <= limit && text[length]) ++length;
    if (length > limit) throw std::length_error("fault text exceeds its size limit");
    return {text, length};
}

lr::LRDiagSeverity severity(aurocks_fault_disposition disposition) {
    switch (disposition) {
    case AUROCKS_SILENT_PATCH: return lr::LRDiagSeverity::silent_patch;
    case AUROCKS_LOUD_PATCH: return lr::LRDiagSeverity::loud_patch;
    case AUROCKS_INFORM: return lr::LRDiagSeverity::inform;
    case AUROCKS_WARN: return lr::LRDiagSeverity::warning;
    case AUROCKS_ERROR: return lr::LRDiagSeverity::error;
    }
    throw std::invalid_argument("unknown fault disposition");
}

void report(const char* level, std::size_t offset, const char* message) {
    std::fprintf(stderr, "aurocks: %s at byte %zu: %s\n", level, offset, message);
}

class FaultRun {
public:
    FaultRun(const char* original, const aurocks_fault_rule* rules, std::size_t count)
        : original_(original), current_(boundedText(original, inputLimit)), rules_(rules), count_(count) {}

    void* parse(const char** error, aurocks_fault_attempt attempt, const void* parser) {
        for (;;) {
            const char* attemptError = current_.c_str();
            action_.reset();
            observerFailed_ = false;
            auto* result = attempt(current_.c_str(), &attemptError, observe, this, parser);
            if (!attemptError) {
                *error = nullptr;
                return result;
            }
            const auto offset = static_cast<std::size_t>(attemptError - current_.c_str());
            *error = original_ + originalOffset(offset);
            if (observerFailed_) {
                report("error", originalOffset(offset), "could not evaluate runtime fault rules");
                return nullptr;
            }
            if (!action_) return nullptr;
            const auto diagnosticOffset = originalOffset(state_.offset);
            if (!action_->patch) {
                const char* level = "error";
                if (action_->severity == lr::LRDiagSeverity::inform) level = "info";
                if (action_->severity == lr::LRDiagSeverity::warning) level = "warning";
                report(level, diagnosticOffset, action_->message.c_str());
                return nullptr;
            }
            const auto& patch = std::get<lr::LRBytePatch>(*action_->patch);
            const auto removed = patch.end - patch.begin;
            if (retry_ == retryLimit || patch.replacement.size() > inputLimit - (current_.size() - removed)
                || std::string_view(current_).substr(patch.begin, removed) == patch.replacement) {
                report("error", diagnosticOffset, "fault patch made no progress or exceeded recovery limits");
                return nullptr;
            }
            apply(patch);
            if (action_->severity == lr::LRDiagSeverity::loud_patch)
                report("patch", diagnosticOffset, action_->message.c_str());
            ++retry_;
        }
    }

private:
    std::size_t originalOffset(std::size_t offset) const {
        return offsets_.empty() ? offset : offsets_[offset];
    }

    void apply(const lr::LRBytePatch& patch) {
        if (offsets_.empty()) {
            offsets_.resize(current_.size() + 1);
            std::iota(offsets_.begin(), offsets_.end(), std::size_t{0});
        }
        const auto anchor = offsets_[patch.begin];
        // The original boundary following the edit belongs to the untouched suffix.
        offsets_.erase(offsets_.begin() + patch.begin, offsets_.begin() + patch.end);
        offsets_.insert(offsets_.begin() + patch.begin, patch.replacement.size(), anchor);
        current_.replace(patch.begin, patch.end - patch.begin, patch.replacement);
    }

    void buildEngine() {
        for (std::size_t i = 0; i < count_; ++i) {
            const auto* rule = rules_ + i;
            engine_.addRule(
                [this, peg = lr::LRDiagBytePEG(rule->pattern)](const lr::LRDiagState& state) {
                    const auto end = peg.matchPrefixAt(state.source, state.byteBegin, remaining_);
                    if (!end) return false;
                    state_.match_end = *end;
                    return true;
                },
                [this, rule](const lr::LRDiagState&) {
                    aurocks_fault_response response{state_.offset, state_.match_end, rule->replacement, rule->message};
                    if (rule->handler) rule->handler(&state_, &response);
                    const auto level = severity(rule->disposition);
                    lr::LRDiagAction action{level, {}, std::nullopt, {}};
                    if (level != lr::LRDiagSeverity::silent_patch)
                        action.message = boundedText(response.message, messageLimit);
                    if (level == lr::LRDiagSeverity::silent_patch || level == lr::LRDiagSeverity::loud_patch)
                        action.patch = lr::LRBytePatch{response.begin, response.end,
                                                      std::string(boundedText(response.replacement, inputLimit))};
                    return action;
                });
        }
        ready_ = true;
    }

    static void observe(void* data, aurocks_fault_backend backend, aurocks_fault_reason reason,
                        const void* native, std::size_t offset, const aurocks_fault_frontier* frontier) noexcept {
        auto& run = *static_cast<FaultRun*>(data);
        try {
            if (offset > run.current_.size()) throw std::out_of_range("invalid fault offset");
            if (!run.ready_) run.buildEngine();
            run.state_ = {backend, reason, native, run.current_.c_str(), run.current_.size(), offset, offset,
                          run.original_, run.originalOffset(offset), run.retry_,
                          frontier ? *frontier : aurocks_fault_frontier{}};
            // Scannerless parsers report source-byte positions, not token IDs.
            run.action_ = run.engine_.tryDiagnose({lr::LRDiagPhase::lexical, run.current_, "aurocks",
                                                    offset, offset, nullptr, nullptr, 0, std::nullopt, {}});
            run.state_.native_parser = nullptr;
        } catch (...) {
            run.observerFailed_ = true;
        }
    }

    const char* original_;
    std::string current_;
    const aurocks_fault_rule* rules_;
    std::size_t count_, retry_ = 0, remaining_ = matchBudget;
    std::vector<std::size_t> offsets_;
    aurocks_fault_state state_{};
    lr::LRDiagEngine engine_;
    std::optional<lr::LRDiagAction> action_;
    bool ready_ = false, observerFailed_ = false;
};

} // namespace

extern "C" void* aurocks_fault_run(const char* input, const char** error, aurocks_fault_attempt attempt,
                                   const void* parser, const aurocks_fault_rule* rules, std::size_t count) {
    const char* ignoredError;
    if (!error) error = &ignoredError;
    *error = input;
    if (!input || !attempt || (count && !rules) || count > ruleLimit) return nullptr;
    try {
        return FaultRun(input, rules, count).parse(error, attempt, parser);
    } catch (...) {
        return nullptr;
    }
}
