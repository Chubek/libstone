#pragma once
#include "ast.hpp"
#include "bv_solver.hpp"
#include "elaboration.hpp"
#include "incremental_sat.hpp"
#include <algorithm>
#include <array>
#include <limits>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace smtlib2 {
namespace detail {
inline Limits session_input_limits(Limits limits) noexcept {
    // Concrete parsing is iterative, but typed syntax and elaboration recurse.
    limits.max_depth = std::min(limits.max_depth, Limits{}.max_depth);
    return limits;
}
}

enum class ResponseKind {
    success, check_result, model, values, assertions, unsat_assumptions, option, info, echo
};
struct ModelResponse { BoolModel bindings; };
struct ValueBinding { SExpr term; bool value; };
struct ValuesResponse { std::vector<ValueBinding> bindings; };
struct AssertionsResponse { std::vector<SExpr> assertions; };
struct UnsatAssumptionsResponse { std::vector<SExpr> assumptions; };
struct OptionResponse { std::string keyword; bool value; };
struct InfoResponse { std::string keyword; std::string value; };
struct EchoResponse { std::string value; };
struct BVBinding { unsigned width; std::uint64_t value; }; // width 0 denotes Bool
struct BVModelResponse { std::map<std::string, BVBinding> bindings; };
struct BVValuesResponse { std::vector<std::pair<SExpr, BVBinding>> bindings; };
using ResponsePayload = std::variant<std::monostate, CheckResult, ModelResponse, ValuesResponse,
                                     AssertionsResponse, UnsatAssumptionsResponse, OptionResponse,
                                     InfoResponse, EchoResponse, BVModelResponse, BVValuesResponse>;
struct Response {
    std::string text;
    ResponseKind kind = ResponseKind::success;
    ResponsePayload payload;
};

/// State machine for Boolean commands and an explicitly selected QF_BV subset.
/// Boolean checks retain assertion encodings across checks and assertion-only
/// scope changes; declaration changes discard the cache. Unsupported commands,
/// sorts, and invalid terms throw without changing the current context/result.
/// Legacy defaults allow implicit logic selection and enable model production.
/// @implstring The concrete input is parsed as a batch before dispatch; each
/// command is then syntax-checked and executed in order. General theories, full
/// logic validation, cores, proofs, and output-channel options are unsupported.
class Session {
public:
    explicit Session(std::size_t max_variables = BoolSolver::default_max_variables,
                     SatLimits limits = {}) : max_variables_(max_variables), limits_(limits) {}

    std::vector<Response> execute(std::string_view source, Limits limits = {}) {
        auto commands = parse(source, detail::session_input_limits(limits));
        std::vector<Response> responses;
        for (auto& command : commands) {
            if (exited()) throw Error(command.span, "command after exit");
            // Allocate before dispatch so recording a reply cannot fail after
            // a command commits. Grow only when previously emitted replies fill
            // the buffer, not in proportion to the number of commands parsed.
            if (responses.size() == responses.capacity()) {
                const auto room = responses.max_size() - responses.size();
                if (!room) throw std::length_error("too many SMT-LIB responses");
                responses.reserve(responses.size() + std::min(room, std::max(responses.size(), std::size_t{1})));
            }
            auto response = run(make_command(std::move(command)));
            if (response) responses.push_back(std::move(*response));
        }
        return responses;
    }
    // Boolean solver result; QF_BV has a separate, non-projecting model API.
    const SolveResult& last_result() const noexcept { return last_.result; }
    const std::optional<bv_solver::Result>& last_bv_result() const noexcept { return last_.bv; }
    bool exited() const noexcept { return phase_ == Phase::exited; }
    std::size_t encoded_assertion_count() const noexcept {
        return solver_ ? solver_->encoded_assertion_count() : 0;
    }

private:
    enum class Phase { configuring, active, exited };
    struct Assertion { SExpr source; SExpr term; };
    struct Frame {
        std::vector<std::string> names;
        std::vector<Assertion> assertions;
    };
    struct Options {
        bool print_success = false;
        bool produce_models = true;
        bool produce_assertions = false;
        bool produce_unsat_assumptions = false;
        bool global_declarations = false;
    };
    struct LastCheck {
        SolveResult result;
        std::vector<SExpr> assumptions;
        bool current = false;
        std::optional<bv_solver::Result> bv;
        CheckResult status() const noexcept { return bv ? bv->status : result.status; }
    };
    using Reply = std::optional<Response>;

    static bool named(const SExpr& e, std::string_view text) {
        return !e.is_list && (e.kind == TokenKind::atom || e.kind == TokenKind::quoted_symbol)
            && bool_elaboration::symbol_view(e, e.span) == text;
    }
    static const char* print_bool(bool value) { return value ? "true" : "false"; }
    static void require_bool_sort(const SortNodeSyntax& sort) {
        if (!sort.arguments.empty() || !sort.identifier.indices.empty()
            || sort.identifier.symbol.name() != "Bool")
            throw Error(sort.span, "only Bool sorts are supported");
    }
    static std::size_t number(const NumeralSyntax& e) {
        std::size_t n = 0;
        for (char c : e.spelling) {
            const auto digit = std::size_t(c - '0');
            if (n > (std::numeric_limits<std::size_t>::max() - digit) / 10)
                throw Error(e.span, "numeral exceeds supported range", ErrorCategory::resource_limit);
            n = n * 10 + digit;
        }
        return n;
    }
    static std::string symbol_spelling(const std::string& key) {
        // A canonical name may be a reserved word or contain characters not
        // admitted in an unquoted symbol. Only emit a bare token if it is legal.
        try {
            const auto tokens = lex(key);
            if (tokens.size() == 1 && tokens[0].kind == TokenKind::atom && tokens[0].text == key)
                return key;
        } catch (const Error&) {}
        return "|" + key + "|";
    }
    static std::string print_list(const std::vector<SExpr>& list) {
        std::string text = "(";
        for (std::size_t i = 0; i < list.size(); ++i) {
            if (i) text += ' ';
            text += print(list[i]);
        }
        return text + ')';
    }
    static std::string string_contents(const std::string& spelling) {
        std::string value;
        for (std::size_t i = 1; i + 1 < spelling.size(); ++i) {
            value += spelling[i];
            if (spelling[i] == '"') ++i;
        }
        return value;
    }
    static Reply success(bool enabled) {
        if (enabled) return Response{"success", ResponseKind::success, {}};
        return {};
    }
    void invalidate() noexcept {
        last_.result.status = CheckResult::unknown;
        last_.result.model.clear();
        last_.assumptions.clear();
        last_.current = false;
        last_.bv.reset();
    }
    void changed_context() noexcept {
        phase_ = Phase::active;
        invalidate();
    }
    static bool Options::* option(std::string_view keyword, Span span) {
        if (keyword == ":print-success") return &Options::print_success;
        if (keyword == ":produce-models") return &Options::produce_models;
        if (keyword == ":produce-assertions" || keyword == ":interactive-mode")
            return &Options::produce_assertions;
        if (keyword == ":produce-unsat-assumptions") return &Options::produce_unsat_assumptions;
        if (keyword == ":global-declarations") return &Options::global_declarations;
        throw Error(span, "unsupported option: " + std::string(keyword));
    }
    SExpr normalize(const SExpr& term, const std::vector<std::string>& params = {}) const {
        try {
            return normalize_bool(term, environment_, params);
        } catch (const UnsupportedBool& e) {
            throw Error(e.span(), e.what(), e.category());
        }
    }
    std::string new_name(const SymbolSyntax& name) const {
        auto key = name.name();
        if (is_bool_builtin(key) || environment_.count(key))
            throw Error(name.span, "symbol already declared: " + name.spelling);
        return key;
    }
    void declare(std::string key, BoolDefinition definition) {
        if (options_.global_declarations) {
            environment_.emplace(std::move(key), std::move(definition));
        } else {
            auto& names = frames_.back().names;
            names.push_back(key);
            try {
                environment_.emplace(std::move(key), std::move(definition));
            } catch (...) {
                names.pop_back();
                throw;
            }
        }
        solver_.reset();
        changed_context();
    }
    void drop_names(const Frame& frame) {
        if (!frame.names.empty()) solver_.reset();
        for (const auto& key : frame.names) {
            environment_.erase(key);
            bv_environment_.erase(key);
        }
    }
    void require_sat(Span span, const char* query) const {
        if (!options_.produce_models || !last_.current || last_.status() != CheckResult::sat)
            throw Error(span, std::string(query) + " requires model production and a current sat result");
    }
    Reply check(const std::vector<PropositionSyntax>& propositions = {},
                std::vector<SExpr> literals = {}) {
        const auto record_check = [&](CheckResult status) noexcept {
            if (!options_.produce_unsat_assumptions || status != CheckResult::unsat) literals.clear();
            last_.assumptions = std::move(literals);
            last_.current = true;
            phase_ = Phase::active;
        };
        const auto check_reply = [](CheckResult status) -> Reply {
            return Response{to_string(status), ResponseKind::check_result, status};
        };
        if (bv_logic_) {
            for (const auto& p : propositions) {
                const auto it = bv_environment_.find(p.symbol.name());
                if (it == bv_environment_.end() || it->second != 0)
                    throw Error(p.span, "expected declared Boolean constant in assumption");
            }
            auto terms = literals;
            for (const auto& f : frames_)
                for (const auto& a : f.assertions) terms.push_back(a.source);
            auto solved = bv_solver::check(bv_environment_, terms, std::min<std::size_t>(16, max_variables_),
                                           limits_.max_work);
            if (!options_.produce_models) solved.model.clear();
            const auto status = solved.status;
            auto response = check_reply(status);
            last_.bv = std::move(solved);
            record_check(status);
            return response;
        }
        std::vector<SExpr> terms;
        for (std::size_t i = 0; i < propositions.size(); ++i) {
            const auto& variable = propositions[i].symbol;
            const auto decl = environment_.find(variable.name());
            if (decl == environment_.end() || !decl->second.parameters.empty())
                throw Error(variable.span, "expected declared Boolean constant in assumption");
            terms.push_back(normalize(literals[i]));
        }
        std::optional<IncrementalBoolSolver> fresh;
        if (!solver_) {
            std::vector<std::string> variables;
            for (const auto& entry : environment_)
                if (!entry.second.body && entry.second.parameters.empty()) variables.push_back(entry.first);
            fresh.emplace(variables, limits_, max_variables_);
            for (std::size_t i = 0; i < frames_.size(); ++i) {
                if (i) fresh->push();
                for (const auto& a : frames_[i].assertions) fresh->assert_formula(a.term);
            }
        }
        // An existing solver can grow its active encoding on success. Prepare
        // all possible replies before the check so rendering cannot interrupt
        // the commit after that encoding becomes visible.
        std::array<Reply, 3> replies{check_reply(CheckResult::sat), check_reply(CheckResult::unsat),
                                     check_reply(CheckResult::unknown)};
        auto solved = (fresh ? *fresh : *solver_).check(terms);
        const auto status = solved.status;
        if (!options_.produce_models) solved.model.clear();
        if (fresh) solver_.emplace(std::move(*fresh));
        last_.result = std::move(solved);
        record_check(status);
        return std::move(replies[status == CheckResult::sat ? 0 : status == CheckResult::unsat ? 1 : 2]);
    }
    Reply model(Span span) const {
        require_sat(span, "model");
        std::string text = "(";
        if (last_.bv) {
            BVModelResponse payload;
            for (const auto& entry : last_.bv->model) {
                const auto w = bv_environment_.at(entry.first);
                payload.bindings.emplace(entry.first, BVBinding{w, entry.second});
                text += "\n  (define-fun " + symbol_spelling(entry.first) + " () "
                     + (w ? "(_ BitVec " + std::to_string(w) + ")" : "Bool") + " "
                     + bv_solver::literal(w, entry.second) + ")";
            }
            return Response{text + "\n)", ResponseKind::model, std::move(payload)};
        }
        for (const auto& entry : last_.result.model)
            text += "\n  (define-fun " + symbol_spelling(entry.first)
                 + " () Bool " + print_bool(entry.second) + ")";
        text += "\n)";
        return Response{std::move(text), ResponseKind::model, ModelResponse{last_.result.model}};
    }
    Reply values(const SExpr& terms) const {
        require_sat(terms.span, "get-value");
        if (last_.bv) {
            BVValuesResponse payload;
            std::string text = "(";
            for (const auto& term : terms.children) {
                const auto w = bv_solver::width(term, bv_environment_);
                const auto v = bv_solver::value(term, bv_environment_, last_.bv->model);
                if (!payload.bindings.empty()) text += ' ';
                text += "(" + print(term) + " " + bv_solver::literal(w, v) + ")";
                payload.bindings.emplace_back(term, BVBinding{w, v});
            }
            return Response{text + ')', ResponseKind::values, std::move(payload)};
        }
        ValuesResponse payload;
        std::string text = "(";
        for (const auto& term : terms.children) {
            const auto normalized = normalize(term);
            bool value = false;
            if (!BoolSolver::evaluate(normalized, last_.result.model, value))
                throw Error(term.span, "cannot evaluate term in model");
            if (!payload.bindings.empty()) text += ' ';
            text += "(" + print(term) + " " + print_bool(value) + ")";
            payload.bindings.push_back({term, value});
        }
        return Response{text + ')', ResponseKind::values, std::move(payload)};
    }
    Reply get_info(const KeywordSyntax& key) const {
        std::string value;
        if (key.spelling == ":name") value = "\"SMTLIB2\"";
        else if (key.spelling == ":version") value = "\"0.1.0\"";
        else if (key.spelling == ":error-behavior") value = "immediate-exit";
        else if (key.spelling == ":reason-unknown") {
            if (!last_.current || last_.status() != CheckResult::unknown)
                throw Error(key.span, "reason-unknown requires a current unknown result");
            value = last_.bv ? "\"" + last_.bv->reason + "\"" : "incomplete";
        } else throw Error(key.span, "unsupported info keyword: " + key.spelling);
        return Response{"(" + key.spelling + " " + value + ")", ResponseKind::info,
                        InfoResponse{key.spelling, std::move(value)}};
    }
    static void set_info(const AttributeSyntax& info) {
        const auto& value = info.value;
        const auto span = value ? value->span : info.span;
        if (info.keyword == ":smt-lib-version") {
            if (!value || value->is_list || value->kind != TokenKind::decimal
                || value->atom.compare(0, 3, "2.6") != 0
                || value->atom.find_first_not_of('0', 3) != std::string::npos)
                throw Error(span, "only SMT-LIB 2.6 is supported");
        } else if (info.keyword == ":status") {
            if (!value || (!named(*value, "sat") && !named(*value, "unsat") && !named(*value, "unknown")))
                throw Error(span, "expected status sat, unsat, or unknown");
        }
        // SMT-LIB metadata does not change the assertion context or a result.
    }
    Reply set_option(const AttributeSyntax& attribute, Span key_span) {
        const auto member = option(attribute.keyword, key_span);
        const auto& value = attribute.value;
        if (member != &Options::print_success && phase_ != Phase::configuring)
            throw Error(key_span, "option must be set before starting the context");
        if (!value || value->is_list || value->kind != TokenKind::atom
            || (value->atom != "true" && value->atom != "false"))
            throw Error(value ? value->span : key_span, "expected Boolean option value", ErrorCategory::syntax);
        const bool enabled = value->atom == "true";
        auto reply = success(member == &Options::print_success ? enabled : options_.print_success);
        options_.*member = enabled;
        return reply;
    }
    void define(const FunctionDefinitionSyntax& definition, const SyntaxArena& arena, const SExpr& source) {
        const auto& signature = definition.signature;
        auto name = new_name(signature.symbol);
        require_bool_sort(arena[signature.result]);
        std::vector<std::string> params;
        std::set<std::string> seen;
        for (const auto& param : signature.parameters) {
            require_bool_sort(arena[param.sort]);
            auto key = param.symbol.name();
            bool_elaboration::require_bindable(key, param.symbol.span);
            if (!seen.insert(key).second)
                throw Error(param.symbol.span, "duplicate function parameter");
            params.push_back(std::move(key));
        }
        auto body = normalize(source, params);
        declare(std::move(name), BoolDefinition{std::move(params), std::move(body)});
    }
    void scopes(const NumeralSyntax& count, bool push) {
        const auto n = number(count);
        if (n == 0) return;
        if (push) {
            constexpr std::size_t max_frames = 1025; // includes the base frame
            if (n > max_frames - frames_.size())
                throw Error(count.span, "scope limit exceeded", ErrorCategory::resource_limit);
            frames_.resize(frames_.size() + n);
            try {
                if (solver_) solver_->push_scopes(n);
            } catch (...) {
                frames_.resize(frames_.size() - n);
                throw;
            }
        } else {
            if (n >= frames_.size()) throw Error(count.span, "pop exceeds scope depth");
            const auto end = frames_.size() - n;
            for (auto i = end; i < frames_.size(); ++i) drop_names(frames_[i]);
            if (solver_) solver_->pop(n);
            frames_.resize(end);
        }
        changed_context();
    }
    void reset(bool full) {
        solver_.reset();
        if (full) {
            bv_logic_ = false;
            bv_environment_.clear();
            options_ = {};
            environment_.clear();
            phase_ = Phase::configuring;
        } else {
            for (const auto& frame : frames_) drop_names(frame);
        }
        frames_.resize(1);
        frames_.front() = {};
        invalidate();
    }
    Reply assertions(Span span) const {
        if (!options_.produce_assertions)
            throw Error(span, "get-assertions requires :produce-assertions");
        AssertionsResponse payload;
        for (const auto& f : frames_)
            for (const auto& a : f.assertions) payload.assertions.push_back(a.source);
        return Response{print_list(payload.assertions), ResponseKind::assertions, std::move(payload)};
    }
    Reply unsat_assumptions(Span span) const {
        if (!options_.produce_unsat_assumptions || !last_.current
            || last_.status() != CheckResult::unsat)
            throw Error(span, "get-unsat-assumptions requires enabled production and a current unsat result");
        // The full input list is a valid subset; no minimality is promised.
        return Response{print_list(last_.assumptions), ResponseKind::unsat_assumptions,
                        UnsatAssumptionsResponse{last_.assumptions}};
    }
    Reply run(CommandSyntax command) {
        using Kind = CommandKind;
        const auto span = command.syntax.span;
        auto& children = command.syntax.children;
        const auto& value = command.value;
        // Prepare the success response before any context update. Full reset
        // uses the pre-reset option; set-option computes its own response.
        auto reply = success(options_.print_success);
        switch (command.kind) {
        case Kind::set_logic: {
            const auto& symbol = std::get<SymbolSyntax>(value);
            if (phase_ != Phase::configuring)
                throw Error(span, "logic must be set before starting the context");
            const auto logic = symbol.name();
            if (logic != "QF_UF" && logic != "ALL" && logic != "QF_BV")
                throw Error(symbol.span, "unsupported logic");
            bv_logic_ = logic == "QF_BV";
            phase_ = Phase::active;
            break;
        }
        case Kind::set_option: return set_option(std::get<AttributeSyntax>(value), children[1].span);
        case Kind::get_option: {
            const auto& key = std::get<KeywordSyntax>(value);
            const bool enabled = options_.*option(key.spelling, key.span);
            return Response{print_bool(enabled), ResponseKind::option, OptionResponse{key.spelling, enabled}};
        }
        case Kind::set_info:
            set_info(std::get<AttributeSyntax>(value));
            break;
        case Kind::get_info: return get_info(std::get<KeywordSyntax>(value));
        case Kind::echo: {
            const auto& spelling = std::get<ConstantSyntax>(value).spelling;
            return Response{spelling, ResponseKind::echo, EchoResponse{string_contents(spelling)}};
        }
        case Kind::declare_const:
        case Kind::declare_fun: {
            const auto& decl = std::get<FunctionDeclarationSyntax>(value);
            if (!decl.parameters.empty())
                throw Error(children[2].span, "only nullary uninterpreted functions are supported");
            if (bv_logic_) {
                const auto& sort = command.arena[decl.result];
                unsigned w = 0;
                const auto& indices = sort.identifier.indices;
                if (sort.identifier.symbol.name() == "BitVec" && sort.arguments.empty()
                    && indices.size() == 1 && std::holds_alternative<NumeralSyntax>(indices[0])) {
                    const auto n = number(std::get<NumeralSyntax>(indices[0]));
                    if (!n || n > 64) throw Error(sort.span, "supported bit-vector widths are 1..64");
                    w = static_cast<unsigned>(n);
                } else require_bool_sort(sort);
                auto key = decl.symbol.name();
                if (bv_solver::builtin(key) || bv_environment_.count(key))
                    throw Error(decl.symbol.span, "symbol already declared: " + key);
                auto inserted = bv_environment_.emplace(key, w);
                try {
                    if (!options_.global_declarations) frames_.back().names.push_back(std::move(key));
                } catch (...) {
                    bv_environment_.erase(inserted.first);
                    throw;
                }
                changed_context();
                break;
            }
            require_bool_sort(command.arena[decl.result]);
            declare(new_name(decl.symbol), {});
            break;
        }
        case Kind::define_fun:
            if (bv_logic_) throw Error(span, "define-fun unsupported in QF_BV");
            define(std::get<FunctionDefinitionSyntax>(value), command.arena, children[4]);
            break;
        case Kind::assert_: {
            SExpr term;
            if (bv_logic_) {
                if (bv_solver::width(children[1], bv_environment_) != 0)
                    throw Error(children[1].span, "assert requires a Boolean term");
            } else term = normalize(children[1]);
            frames_.back().assertions.push_back({std::move(children[1]), std::move(term)});
            try {
                if (solver_) solver_->assert_formula(frames_.back().assertions.back().term);
            } catch (...) {
                frames_.back().assertions.pop_back();
                throw;
            }
            changed_context();
            break;
        }
        case Kind::push:
        case Kind::pop:
            scopes(std::get<NumeralSyntax>(value), command.kind == Kind::push);
            break;
        case Kind::reset_assertions:
        case Kind::reset:
            reset(command.kind == Kind::reset);
            break;
        case Kind::check_sat: return check();
        case Kind::check_sat_assuming:
            return check(std::get<std::vector<PropositionSyntax>>(value), std::move(children[1].children));
        case Kind::get_model: return model(span);
        case Kind::get_value: return values(children[1]);
        case Kind::get_assertions: return assertions(span);
        case Kind::get_unsat_assumptions: return unsat_assumptions(span);
        case Kind::exit:
            reset(true);
            phase_ = Phase::exited;
            break;
        default:
            throw Error(children[0].span, "unsupported command: " + children[0].atom);
        }
        return reply;
    }

    std::vector<Frame> frames_{1};
    BoolEnvironment environment_;
    bv_solver::Environment bv_environment_;
    bool bv_logic_ = false;
    std::size_t max_variables_;
    SatLimits limits_;
    std::optional<IncrementalBoolSolver> solver_;
    Options options_;
    LastCheck last_;
    Phase phase_ = Phase::configuring;
};
}
