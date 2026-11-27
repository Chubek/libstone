#pragma once

#include "Frontend.hpp"
#include "Native.hpp"

#include <memory>
#include <utility>
#include <variant>

namespace am8 {
namespace expansion_detail {

namespace synth = dsl::synth;
namespace structural = synth::specification_detail;

struct Host : dsl::DSL<Host, dsl::LanguageSynthesizer> {};

inline bool symbol(const Syntax& x, std::string_view name) {
    return x.is_atom && !x.quoted && x.text == name;
}

inline bool variable(const Syntax& x) {
    return x.is_atom && !x.quoted && x.text.size() > 1 && x.text.front() == '$';
}

inline bool staging(std::string_view name) {
    return name == "stage" || name == "stage-bind" || name == "stage-fn";
}

inline bool data(std::string_view name) {
    return name == "quote" || name == "quasiquote" || name == "splice" ||
           name == "splice-list";
}

inline bool reserved(std::string_view name) {
    return staging(name) || data(name) || name == "syntax" || name == "native" ||
           name == "declarations" || name == "import" || name == "export";
}

inline constexpr std::string_view fresh_prefix = "am8_fresh_";

inline synth::SpecBuilder core() {
    synth::SpecBuilder spec("Amalgam8");
    spec.form("syntax", 2).form("native", 1)
        .form("stage", 1).form("stage-bind", 2).form("stage-fn", 3)
        .form({"declarations", 0, std::nullopt})
        .form("include", 1).form({"use", 1, 2}).form("def", 2).form("fn", 4)
        .form("c", 1).form("cpp", 1).form("cli", 1)
        .form("type", 2).form("record", 2).form("enum", 2)
        .form("var", 3).form("extern", 3).form("cpp-type", 1);
    return spec;
}

class Expansion {
    const Program& program_;
    const NativeRegistry& registry_;

    // All modules and both phases debit these same counters. LibDSL's matcher
    // also debits this work budget directly when looking for a macro rule.
    structural::Budget work_;
    std::size_t rewrites_, generated_, steps_;
    std::set<std::string, std::less<>> names_;
    std::size_t next_fresh_ = 0;
    const synth::MetaLanguage core_ = Host::synthesize(core().build());

    template<class Action>
    static decltype(auto) dsl_at(const Syntax& where, Action&& action) {
        try {
            return std::forward<Action>(action)();
        } catch (const std::invalid_argument& e) {
            fail(where, e.what());
        } catch (const std::length_error& e) {
            fail(where, e.what());
        }
    }

    void charge(const Syntax& x, std::size_t depth) {
        if (depth > work_.depth) fail(x, "expansion nesting limit exceeded");
        if (x.origin_module && *x.origin_module >= program_.modules.size())
            fail(x, "invalid syntax origin module");
        spend(x, 1);
        spend(x, x.text.size());
    }

    void spend(const Syntax& x, std::size_t n = 1) {
        if (n > work_.left) fail(x, "compilation-wide expansion work limit exceeded");
        work_.left -= n;
    }

    static void debit(std::size_t& left, const Syntax& x, std::string_view limit) {
        if (!left) fail(x, "compilation-wide " + std::string(limit) + " limit exceeded");
        --left;
    }

    static bool same_span(const Span& a, const Span& b) {
        return a.begin == b.begin && a.end == b.end && a.line == b.line &&
               a.column == b.column && a.file == b.file;
    }

    void note(Syntax& out, const Span& from) {
        if (from.file.empty() || same_span(out.span, from)) return;
        for (const auto& s : out.provenance) {
            spend(out);
            if (same_span(s, from)) return;
        }
        spend(out);
        spend(out, from.file.size());
        out.provenance.push_back(from);
    }

    void trail(Syntax& out, const Syntax& from) {
        note(out, from.span);
        for (const auto& s : from.provenance) note(out, s);
    }

    Syntax copy_head(const Syntax& x, std::size_t depth, bool generated,
                     std::size_t module, const Syntax* trace = nullptr,
                     bool at_trace = false) {
        charge(x, depth);
        if (generated) debit(generated_, x, "generated syntax");
        const auto& at = trace && (at_trace || x.span.file.empty()) ? trace->span : x.span;
        spend(x, at.file.size());
        Syntax out{x.is_atom, x.text, {}, x.quoted, at, {},
                   x.origin_module.value_or(module)};
        if (at_trace) note(out, x.span);
        for (const auto& s : x.provenance) note(out, s);
        if (trace) trail(out, *trace);
        return out;
    }

    Syntax copy(const Syntax& x, std::size_t depth, bool generated, std::size_t module,
                const Syntax* trace = nullptr, const Syntax* definition = nullptr) {
        auto out = copy_head(x, depth, generated, module, trace);
        if (definition) trail(out, *definition);
        for (const auto& child : x.items)
            out.items.push_back(copy(child, depth + 1, generated, module, trace, definition));
        return out;
    }

    void check_tree(const Syntax& x, std::size_t depth) {
        charge(x, depth);
        if ((x.is_atom && (!x.items.empty() || (!x.quoted && !structural::bare(x.text)))) ||
            (!x.is_atom && (!x.text.empty() || x.quoted)))
            fail(x, "malformed expansion syntax");
        for (const auto& child : x.items) check_tree(child, depth + 1);
    }

    void reserve_source(const Syntax& x, std::size_t depth) {
        charge(x, depth);
        if (x.is_atom && !x.quoted) names_.insert(x.text);
        if (x.is_atom && x.quoted) {
            // Opaque bodies are not rewritten. Reserve identifier spellings
            // nevertheless so explicit fresh names cannot collide with them.
            const std::string_view text = x.text;
            for (std::size_t i = 0; i < text.size();) {
                if (!letter(text[i]) && !digit(text[i])) { ++i; continue; }
                auto begin = i++;
                while (i < text.size() && (letter(text[i]) || digit(text[i]))) ++i;
                auto word = text.substr(begin, i - begin);
                if (word.starts_with(fresh_prefix)) names_.emplace(word);
            }
        }
        for (const auto& child : x.items) reserve_source(child, depth + 1);
    }

    void validate(const Syntax& x, const synth::MetaLanguage& language,
                  std::size_t depth = 0) {
        check_tree(x, depth);
        dsl_at(x, [&] { language.validate_form(to_sexpr(x)); });
    }

    using Data = std::shared_ptr<const Syntax>;
    struct Function;
    using Callable = std::shared_ptr<const Function>;
    using Value = std::variant<Data, Callable>;
    struct Environment {
        std::shared_ptr<const Environment> parent;
        std::map<std::string, Value, std::less<>> bindings;
    };
    struct StageContext {
        std::size_t module, cutoff, destination;
        std::shared_ptr<const Environment> local;
    };
    struct Function {
        std::string name;
        Data parameters, body;
        StageContext context;
    };
    struct StageBinding {
        std::size_t order;
        Value value;
    };
    struct ModuleState {
        std::optional<synth::MetaLanguage> language;
        std::map<std::string, std::vector<std::size_t>, std::less<>> macros;
        std::vector<const Syntax*> macro_sources;
        std::map<std::string, const NativeForm*, std::less<>> natives;
        std::map<std::string, StageBinding, std::less<>> values;
    };
    std::vector<ModuleState> states_;

    struct Lookup {
        std::size_t module;
        std::string_view name;
    };

    template<class Exists>
    std::optional<Lookup> lookup(const Syntax& where, std::string_view name,
                                 std::size_t from, std::string_view kind, Exists exists) {
        if (from >= states_.size()) fail(where, "invalid syntax origin module");
        spend(where, name.size());
        std::optional<Lookup> result;
        const auto dot = name.find('.');
        const auto alias = dot == name.npos ? std::string_view{} : name.substr(0, dot);
        const auto local = dot == name.npos ? name : name.substr(dot + 1);
        const std::string export_name(local);
        const auto add = [&](std::size_t module) {
            if (result && result->module != module)
                fail(where, "ambiguous " + std::string(kind) + " '" + std::string(name) + "'");
            result = Lookup{module, local};
        };
        if (dot == name.npos && exists(from, local)) add(from);
        for (const auto& im : program_.modules[from].imports) {
            spend(where);
            spend(where, name.size());
            if (im.target >= states_.size()) fail(im.span, "invalid import target");
            if (im.alias == alias && (dot == name.npos || !alias.empty()) &&
                program_.modules[im.target].exports.contains(export_name) &&
                exists(im.target, local))
                add(im.target);
        }
        return result;
    }

    static std::string name(const Syntax& x) {
        const auto text = atom(x);
        if (x.quoted || text.find('.') != text.npos)
            fail(x, "expected an unqualified name");
        dsl_at(x, [&] { structural::require_name(text); });
        return text;
    }

    using Captures = std::map<std::string_view, const Syntax*, std::less<>>;
    using Fresh = std::map<std::string, std::string, std::less<>>;

    const std::string& fresh(const Syntax& label, Fresh& instance) {
        const auto key = name(label);
        auto [it, inserted] = instance.try_emplace(key);
        if (inserted) {
            do {
                spend(label);
                if (next_fresh_ == std::numeric_limits<std::size_t>::max())
                    fail(label, "fresh name space exhausted");
                it->second = std::string(fresh_prefix) + std::to_string(++next_fresh_);
                spend(label, it->second.size());
            } while (!names_.insert(it->second).second);
        }
        return it->second;
    }

    void record_captures(const dsl::SExpr& pattern, const Syntax& x, Captures& captures) {
        spend(x);
        if (structural::metavariable(pattern)) captures.try_emplace(pattern.text, &x);
        else
            for (std::size_t i = 0; i < pattern.items.size(); ++i)
                record_captures(pattern.items[i], x.items[i], captures);
    }

    Syntax substitute(const Syntax& x, const Syntax& use, const Syntax& definition,
                      std::size_t module, std::size_t destination,
                      const Captures& captures, Fresh& instance,
                      std::size_t depth) {
        if (variable(x))
            return copy(*captures.at(x.text), depth, true, module, &use, &definition);
        auto out = copy_head(x, depth, true, module, &use, true);
        trail(out, definition);
        if (head(x) == "fresh") {
            out.is_atom = true;
            out.text = fresh(x.items[1], instance);
            out.origin_module = destination;
        } else {
            for (const auto& child : x.items)
                out.items.push_back(substitute(child, use, definition, module, destination,
                                               captures, instance, depth + 1));
        }
        return out;
    }

    bool rewrite(Syntax& x, std::size_t owner, std::size_t depth) {
        const auto op = head(x);
        if (op.empty() || reserved(op)) return false;
        const auto found = lookup(x, op, x.items[0].origin_module.value_or(owner), "macro",
            [&](std::size_t module, auto local) { return states_[module].macros.contains(local); });
        if (!found) return false;
        const auto& state = states_[found->module];
        check_tree(x, depth);
        auto input = to_sexpr(x);
        input.items[0].text = found->name;
        // Match LibDSL's synthesized rules with our shared budget. The
        // standalone MetaLanguage::expand API owns a fresh per-call budget
        // and returns only S-expressions, so annotate copies here instead.
        for (auto index : state.macros.find(found->name)->second) {
            const auto& pattern = state.language->specification().macros()[index].pattern;
            structural::Captures matched;
            if (!dsl_at(x, [&] { return structural::match(pattern, input, matched, work_); }))
                continue;
            debit(rewrites_, x, "rewrite");
            Captures captures;
            record_captures(pattern, x, captures);
            const auto& source = *state.macro_sources[index];
            Fresh instance;
            x = substitute(source.items[2], x, source, found->module, owner,
                           captures, instance, depth);
            return true;
        }
        return false;
    }

    // Descend outside quote and staging barriers. If a child changed, the
    // parent pattern may now match (LibDSL's outermost-first rule).
    bool normalize(Syntax& x, std::size_t owner, std::size_t depth, bool declaration) {
        bool changed = false;
        for (;;) {
            charge(x, depth);
            const auto op = head(x);
            if (x.is_atom || data(op) ||
                (declaration && (staging(op) || op == "declarations" ||
                                 op == "syntax" || op == "native")))
                return changed;
            if (rewrite(x, owner, depth)) {
                changed = true;
                continue;
            }
            bool child_changed = false;
            for (auto& child : x.items)
                child_changed |= normalize(child, owner, depth + 1, false);
            if (!child_changed) return changed;
            changed = true;
        }
    }

    static const Syntax& syntax(const Value& value, const Syntax& where) {
        if (const auto* d = std::get_if<Data>(&value)) return **d;
        fail(where, "stage callable is not syntax");
    }

    static Value wrap(Syntax x) { return std::make_shared<const Syntax>(std::move(x)); }

    static bool literal(const Syntax& x) {
        if (!x.is_atom) return false;
        if (x.quoted || x.text == "true" || x.text == "false") return true;
        auto chars = std::string_view(x.text);
        if (chars.starts_with('-')) chars.remove_prefix(1);
        return !chars.empty() && std::all_of(chars.begin(), chars.end(), digit);
    }

    bool equal(const Syntax& a, const Syntax& b) {
        spend(a);
        spend(a, a.text.size());
        spend(b, b.text.size());
        if (a.is_atom != b.is_atom || a.quoted != b.quoted || a.text != b.text ||
            a.items.size() != b.items.size())
            return false;
        for (std::size_t i = 0; i < a.items.size(); ++i)
            if (!equal(a.items[i], b.items[i])) return false;
        return true;
    }

    static void arity(const Syntax& x, std::size_t n) {
        if (x.items.size() != n + 1)
            fail(x, "wrong arity for stage form '" + std::string(head(x)) + "'");
    }

    void check_pattern(const Syntax& x, std::set<std::string>& seen, std::size_t depth) {
        charge(x, depth);
        if (!x.is_atom) {
            for (const auto& item : x.items) check_pattern(item, seen, depth + 1);
        } else if (!literal(x) && !symbol(x, "_")) {
            const auto key = name(x);
            if (!seen.insert(key).second) fail(x, "duplicate stage parameter '" + key + "'");
        }
    }

    void bind(const Syntax& pattern, const Value& value, Environment& env, std::size_t depth) {
        charge(pattern, depth);
        if (symbol(pattern, "_")) return;
        if (pattern.is_atom && !literal(pattern)) {
            env.bindings.emplace(pattern.text, value);
            return;
        }
        const auto& x = syntax(value, pattern);
        if (pattern.is_atom ? !equal(pattern, x) :
            x.is_atom || x.items.size() != pattern.items.size())
            fail(pattern, "stage destructuring mismatch");
        if (!pattern.is_atom) {
            const auto& parent = std::get<Data>(value);
            for (std::size_t i = 0; i < pattern.items.size(); ++i)
                bind(pattern.items[i], Data(parent, &x.items[i]), env, depth + 1);
        }
    }

    Callable function(std::string key, const Syntax& parameters, const Syntax& body,
                      const StageContext& ctx, std::size_t depth) {
        if (parameters.is_atom) fail(parameters, "expected a stage parameter list");
        std::set<std::string> seen;
        for (const auto& p : parameters.items) check_pattern(p, seen, depth + 1);
        return std::make_shared<const Function>(Function{
            std::move(key), std::make_shared<const Syntax>(copy(parameters, depth, false, ctx.module)),
            std::make_shared<const Syntax>(copy(body, depth, false, ctx.module)), ctx});
    }

    Syntax quasiquote(const Syntax& x, const StageContext& ctx, Fresh& instance,
                      std::size_t level, std::size_t depth) {
        charge(x, depth);
        const auto op = head(x);
        if (op == "quote") return copy(x, depth, true, ctx.module);
        if (op == "quasiquote" || op == "splice" || op == "splice-list") {
            arity(x, 1);
            if (level == 1 && op == "splice") {
                const auto value = eval(x.items[1], ctx, depth + 1);
                return copy(syntax(value, x), depth, true, ctx.module, &x);
            }
            if (level == 1 && op == "splice-list")
                fail(x, "splice-list must be inside a quasiquoted list");
            auto out = copy_head(x, depth, true, ctx.module);
            out.items.push_back(copy(x.items[0], depth + 1, true, ctx.module));
            out.items.push_back(quasiquote(x.items[1], ctx, instance,
                op == "quasiquote" ? level + 1 : level - 1, depth + 1));
            return out;
        }
        auto out = copy_head(x, depth, true, ctx.module);
        if (level == 1 && op == "fresh") {
            arity(x, 1);
            out.is_atom = true;
            out.text = fresh(x.items[1], instance);
            out.origin_module = ctx.destination;
            return out;
        }
        for (const auto& child : x.items) {
            if (level == 1 && head(child) == "splice-list") {
                arity(child, 1);
                const auto value = eval(child.items[1], ctx, depth + 1);
                const auto& list = syntax(value, child);
                if (list.is_atom) fail(child, "splice-list expects list syntax");
                for (const auto& item : list.items)
                    out.items.push_back(copy(item, depth + 1, true, ctx.module, &child));
            } else {
                out.items.push_back(quasiquote(child, ctx, instance, level, depth + 1));
            }
        }
        return out;
    }

    Value eval(const Syntax& x, const StageContext& ctx, std::size_t depth) {
        charge(x, depth);
        debit(steps_, x, "stage step");
        if (x.is_atom) {
            if (literal(x)) return wrap(copy(x, depth, false, ctx.module));
            for (auto frame = ctx.local; frame; frame = frame->parent) {
                spend(x);
                if (auto it = frame->bindings.find(x.text); it != frame->bindings.end())
                    return it->second;
            }
            const auto origin = x.origin_module.value_or(ctx.module);
            const auto found = lookup(x, x.text, origin, "stage binding",
                [&](std::size_t module, auto local) {
                    const auto& values = states_[module].values;
                    const auto it = values.find(local);
                    return it != values.end() &&
                        (module != ctx.module || it->second.order < ctx.cutoff);
                });
            if (found) return states_[found->module].values.find(found->name)->second.value;
            fail(x, "unbound stage variable '" + x.text + "'");
        }
        if (x.items.empty()) return wrap(copy(x, depth, false, ctx.module));
        const auto op = head(x);
        if (op == "quote") {
            arity(x, 1);
            return wrap(copy(x.items[1], depth, true, ctx.module));
        }
        if (op == "quasiquote") {
            arity(x, 1);
            Fresh instance;
            return wrap(quasiquote(x.items[1], ctx, instance, 1, depth));
        }
        if (op == "splice" || op == "splice-list") fail(x, "splice outside quasiquote");
        if (op == "lambda") {
            arity(x, 2);
            return function({}, x.items[1], x.items[2], ctx, depth);
        }
        if (op == "if") {
            arity(x, 3);
            const auto condition = eval(x.items[1], ctx, depth + 1);
            const auto& test = syntax(condition, x.items[1]);
            if (symbol(test, "true")) return eval(x.items[2], ctx, depth + 1);
            if (symbol(test, "false")) return eval(x.items[3], ctx, depth + 1);
            fail(test, "stage condition is not boolean");
        }
        if (op == "let") {
            arity(x, 3);
            std::set<std::string> seen;
            check_pattern(x.items[1], seen, depth + 1);
            const auto value = eval(x.items[2], ctx, depth + 1);
            auto scope = std::make_shared<Environment>(Environment{ctx.local, {}});
            bind(x.items[1], value, *scope, depth + 1);
            return eval(x.items[3], {ctx.module, ctx.cutoff, ctx.destination, scope}, depth + 1);
        }
        if (op == "list" || op == "declarations") {
            auto out = copy_head(x, depth, true, ctx.module);
            if (op == "declarations")
                out.items.push_back(copy(x.items[0], depth + 1, true, ctx.module));
            for (std::size_t i = 1; i < x.items.size(); ++i) {
                const auto value = eval(x.items[i], ctx, depth + 1);
                out.items.push_back(copy(syntax(value, x.items[i]), depth + 1, true, ctx.module));
            }
            return wrap(std::move(out));
        }
        if (op == "equal?") {
            arity(x, 2);
            const auto a = eval(x.items[1], ctx, depth + 1);
            const auto b = eval(x.items[2], ctx, depth + 1);
            auto out = copy_head(x, depth, true, ctx.module);
            out.is_atom = true;
            out.text = equal(syntax(a, x.items[1]), syntax(b, x.items[2])) ? "true" : "false";
            return wrap(std::move(out));
        }
        auto called = eval(x.items[0], ctx, depth + 1);
        const auto* callee = std::get_if<Callable>(&called);
        if (!callee) fail(x, "stage application requires a callable");
        const auto& fn = **callee;
        if (fn.parameters->items.size() != x.items.size() - 1)
            fail(x, "wrong number of stage function arguments");
        auto scope = std::make_shared<Environment>(Environment{fn.context.local, {}});
        for (std::size_t i = 1; i < x.items.size(); ++i) {
            const auto arg = eval(x.items[i], ctx, depth + 1);
            bind(fn.parameters->items[i - 1], arg, *scope, depth + 1);
        }
        // Parameters may shadow the self binding, just as lexically scoped
        // local bindings may shadow earlier stage definitions.
        if (!fn.name.empty()) scope->bindings.try_emplace(fn.name, called);
        try {
            return eval(*fn.body, {fn.context.module, fn.context.cutoff, ctx.destination, scope},
                        depth + 1);
        } catch (const Error& error) {
            throw Error(std::string(error.what()) + "\n" + location(x.span) +
                        ": note: stage function called here");
        }
    }

    void process(Syntax x, std::size_t owner, std::size_t depth, std::vector<Syntax>& out) {
        normalize(x, owner, depth, true);
        const auto op = head(x);
        if (op == "syntax" || op == "native")
            fail(x, "syntax and native registrations must be literal top-level declarations");
        if (staging(op) || op == "declarations") {
            validate(x, core_, depth);
            if (op == "declarations") {
                for (std::size_t i = 1; i < x.items.size(); ++i)
                    process(std::move(x.items[i]), owner, depth + 1, out);
                return;
            }
            auto& values = states_[owner].values;
            const StageContext ctx{owner, values.size(), owner, {}};
            if (op == "stage") {
                const auto result = eval(x.items[1], ctx, depth + 1);
                process(copy(syntax(result, x), depth + 1, true, owner, &x),
                        owner, depth + 1, out);
                return;
            }
            const auto key = name(x.items[1]);
            if (reserved(key) || key == "list" || key == "lambda" || key == "let" ||
                key == "if" || key == "equal?" || literal(x.items[1]))
                fail(x.items[1], "reserved stage name '" + key + "'");
            if (values.contains(key)) fail(x.items[1], "duplicate stage name '" + key + "'");
            Value value = op == "stage-bind" ? eval(x.items[2], ctx, depth + 1) :
                Value(function(key, x.items[2], x.items[3], ctx, depth + 1));
            values.emplace(key, StageBinding{ctx.cutoff, std::move(value)});
            return;
        }
        const auto from = x.items.empty() ? owner : x.items[0].origin_module.value_or(owner);
        if (from >= states_.size()) fail(x, "invalid syntax origin module");
        const auto& state = states_[from];
        if (auto native = state.natives.find(op); native != state.natives.end()) {
            validate(x, *state.language, depth);
            debit(rewrites_, x, "rewrite");
            const auto& entry = *native->second;
            std::vector<Syntax> result;
            try {
                if (entry.validate) entry.validate(x);
                result = entry.lower(x);
            } catch (const std::exception& e) {
                fail(x, "native form '" + entry.name + "': " + e.what());
            }
            for (const auto& node : result)
                process(copy(node, depth + 1, true, from, &x), owner, depth + 1, out);
            return;
        }
        validate(x, core_, depth);
        out.push_back(std::move(x));
    }

    void register_module(std::size_t index) {
        const auto& module = program_.modules[index];
        auto& state = states_[index];
        auto builder = core();
        for (const auto& x : module.forms) {
            reserve_source(x, 0);
            if (head(x) == "syntax") {
                validate(x, core_);
                const auto& p = x.items[1];
                if (head(p).empty()) fail(p, "expected a literal-headed macro pattern");
                const auto key = name(p.items[0]);
                if (reserved(key)) fail(p, "reserved macro name '" + key + "'");
                // Capture the actual immutable rule index in the synthesized language.
                spend(x, state.macro_sources.size());
                dsl_at(x, [&] { builder.macro(to_sexpr(p), to_sexpr(x.items[2])); });
                state.macros[key].push_back(state.macro_sources.size());
                state.macro_sources.push_back(&x);
            } else if (head(x) == "native") {
                validate(x, core_);
                const auto key = name(x.items[1]);
                const auto* entry = registry_.find(key);
                if (reserved(key)) fail(x, "reserved native form '" + key + "'");
                if (!entry) fail(x, "native form '" + key + "' is not available from the host");
                if (!state.natives.try_emplace(key, entry).second)
                    fail(x, "duplicate native registration '" + key + "'");
                dsl_at(x, [&] { builder.form({entry->name, entry->min_arity, entry->max_arity}); });
            }
        }
        state.language.emplace(Host::synthesize(builder.build()));
    }

public:
    Expansion(const Program& program, const NativeRegistry& registry, const Limits& limits)
        : program_(program), registry_(registry),
          work_{limits.expansion_work,
                std::min(limits.expansion_depth, structural::max_tree_depth)},
          rewrites_(limits.rewrites), generated_(limits.generated_nodes),
          steps_(limits.stage_steps), states_(program.modules.size()) {}

    std::vector<std::vector<Syntax>> run() {
        for (std::size_t i = 0; i < states_.size(); ++i) register_module(i);
        std::vector<std::vector<Syntax>> outputs(states_.size());
        for (std::size_t i = 0; i < states_.size(); ++i)
            for (const auto& form : program_.modules[i].forms) {
                const auto op = head(form);
                if (op != "syntax" && op != "native")
                    process(copy(form, 0, false, i), i, 0, outputs[i]);
            }
        return outputs;
    }

    std::set<std::string> compile_time_exports(std::size_t index) const {
        std::set<std::string> result;
        const auto& state = states_[index];
        for (const auto& name : program_.modules[index].exports)
            if (state.macros.contains(name) || state.values.contains(name))
                result.insert(name);
        return result;
    }
};

} // namespace expansion_detail

inline void expand(Program& program, const NativeRegistry& registry, const Limits& limits = {}) {
    expansion_detail::Expansion expansion(program, registry, limits);
    auto outputs = expansion.run();
    for (std::size_t i = 0; i < outputs.size(); ++i) {
        program.modules[i].forms = std::move(outputs[i]);
        program.modules[i].compile_time_exports = expansion.compile_time_exports(i);
    }
}

} // namespace am8
