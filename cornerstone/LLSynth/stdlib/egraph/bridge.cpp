#include <EquinoxNG.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using equinoxng::EClassId;
using equinoxng::EGraph;
using equinoxng::Literal;
using equinoxng::RewriteRule;
using equinoxng::Term;
using Names = std::set<std::string>;

constexpr std::size_t max_input = 16 * 1024;
constexpr std::size_t max_depth = 32;
constexpr std::size_t max_atom = 64;
constexpr std::size_t max_term_nodes = 128;
constexpr std::size_t max_pattern_nodes = 48;
constexpr std::size_t max_rules = 64;
constexpr std::size_t max_iterations = 32;
constexpr std::size_t max_enodes = 4096;
constexpr std::size_t match_work = 100'000;

bool space(char ch) {
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
}

bool separator(char ch) {
    return space(ch) || ch == '(' || ch == ')';
}

bool name_start(char ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_';
}

bool digit(char ch) {
    return ch >= '0' && ch <= '9';
}

bool atom_char(char ch) {
    return name_start(ch) || digit(ch) ||
           std::string_view{"+*/%<>=.!:$&|~^-"}.find(ch) != std::string_view::npos;
}

class Parser {
public:
    Parser(const char *source, std::string_view label) : label_(label) {
        if (!source) fail("missing input");
        std::size_t length = 0;
        while (length <= max_input && source[length]) ++length;
        if (length > max_input) fail("input exceeds 16384 bytes");
        text_ = {source, length};
    }

    Term ground() {
        std::size_t nodes = max_term_nodes;
        auto result = term(1, nodes, nullptr);
        if (peek()) fail("expected one term");
        return result;
    }

    std::vector<RewriteRule> rules() {
        std::vector<RewriteRule> result;
        while (peek()) {
            if (result.size() == max_rules) fail("too many rewrite rules");
            if (!take('(') || atom() != "=>") fail("expected '(=> LEFT RIGHT)'");
            Names lhs_vars, rhs_vars;
            std::size_t nodes = max_pattern_nodes;
            auto lhs = term(1, nodes, &lhs_vars);
            if (lhs.is_var()) fail("the left side cannot be a bare variable");
            nodes = max_pattern_nodes;
            auto rhs = term(1, nodes, &rhs_vars);
            if (!take(')')) fail("expected ')' after the two rule terms");
            for (const auto &var : rhs_vars) {
                if (!lhs_vars.contains(var)) fail("unbound right-side variable ?" + var);
            }
            result.emplace_back("rule-" + std::to_string(result.size() + 1),
                                std::move(lhs), std::move(rhs));
        }
        return result;
    }

private:
    [[noreturn]] void fail(std::string message) const {
        throw std::invalid_argument(std::string(label_) + " at byte " +
                                    std::to_string(pos_ + 1) + ": " + message);
    }

    char peek() {
        while (pos_ < text_.size() && space(text_[pos_])) ++pos_;
        return pos_ < text_.size() ? text_[pos_] : '\0';
    }

    bool take(char ch) {
        if (peek() != ch) return false;
        ++pos_;
        return true;
    }

    std::string atom() {
        peek();
        const auto start = pos_;
        while (pos_ < text_.size() && atom_char(text_[pos_])) ++pos_;
        if (pos_ == start) fail("expected an unquoted ASCII atom");
        if (pos_ - start > max_atom) fail("atom exceeds 64 bytes");
        if (pos_ < text_.size() && !separator(text_[pos_])) fail("invalid character in atom");
        return std::string(text_.substr(start, pos_ - start));
    }

    Term term(std::size_t depth, std::size_t &nodes, Names *variables) {
        if (depth > max_depth) fail("term exceeds nesting limit");
        if (!nodes) fail("term exceeds syntax-node limit");
        --nodes;
        if (take('(')) {
            auto op = atom();
            std::vector<Term> args;
            while (!take(')')) {
                if (!peek()) fail("expected ')'");
                args.push_back(term(depth + 1, nodes, variables));
            }
            return Term::op(std::move(op), std::move(args));
        }
        if (take('?')) {
            if (!variables) fail("variables are only allowed in rewrite rules");
            if (pos_ == text_.size() || !name_start(text_[pos_])) {
                fail("expected a variable name immediately after '?'");
            }
            const auto start = pos_++;
            while (pos_ < text_.size() && (name_start(text_[pos_]) || digit(text_[pos_]))) {
                ++pos_;
            }
            if (pos_ - start > max_atom) fail("variable name exceeds 64 bytes");
            if (pos_ < text_.size() && !separator(text_[pos_])) fail("invalid variable name");
            auto name = std::string(text_.substr(start, pos_ - start));
            variables->insert(name);
            return Term::var(std::move(name));
        }
        return Term::lit(Literal{atom()});
    }

    std::string_view label_;
    std::string_view text_;
    std::size_t pos_ = 0;
};

// Atoms are opaque strings rather than integers: "0" has no built-in
// arithmetic meaning, and a bare atom differs from a nullary application.
void render(std::string &out, const Term &term) {
    if (term.is_lit()) {
        out += std::get<std::string>(term.as_lit().value);
        return;
    }
    const auto &node = term.as_node();
    out += '(';
    out += node.op.name;
    for (const auto &child : node.children) {
        out += ' ';
        render(out, child);
    }
    out += ')';
}

std::string quoted(std::string_view text) {
    std::string out{"\""};
    for (const unsigned char ch : text) {
        if (ch == '"' || ch == '\\') out += '\\';
        if (ch < 0x20 || ch >= 0x7f) {
            constexpr std::string_view hex = "0123456789abcdef";
            out += "\\u00";
            out += hex[ch >> 4];
            out += hex[ch & 15];
        } else {
            out += static_cast<char>(ch);
        }
    }
    out += '"';
    return out;
}

struct TreeSizeCost {
    using cost_type = std::size_t;
    cost_type literal_cost(const Literal &) const { return 1; }
    cost_type symbol_cost(const equinoxng::Symbol &, std::size_t) const { return 1; }
};

std::size_t tree_size(const Term &term) {
    std::size_t size = 1;
    if (term.is_node()) {
        for (const auto &child : term.as_node().children) size += tree_size(child);
    }
    return size;
}

struct Search {
    std::string_view stop = "iteration-limit";
    std::size_t iterations = 0;
};

Search saturate(EGraph &graph, const std::vector<RewriteRule> &rules,
                EClassId root, std::optional<EClassId> other,
                std::size_t iterations, std::size_t nodes) {
    auto same_class = [&] { return other && graph.find(root) == graph.find(*other); };
    if (same_class()) return {"equivalent", 0};
    if (rules.empty()) return {"saturated", 0};
    std::size_t work = match_work;
    for (std::size_t i = 1; i <= iterations; ++i) {
        const auto old_nodes = graph.num_enodes();
        const auto old_unions = graph.stats().unions_performed;
        try {
            for (const auto &rule : rules) graph.apply_rule(rule, nodes, &work);
        } catch (const equinoxng::NodeLimitExceeded &) {
            graph.rebuild();
            return {same_class() ? "equivalent" : "node-limit", i};
        } catch (const equinoxng::MatchLimitExceeded &) {
            graph.rebuild();
            return {same_class() ? "equivalent" : "work-limit", i};
        }
        graph.rebuild();
        if (same_class()) return {"equivalent", i};
        if (old_nodes == graph.num_enodes() && old_unions == graph.stats().unions_performed) {
            return {"saturated", i};
        }
    }
    return {"iteration-limit", iterations};
}

std::string analyze(const char *source, const char *other, const char *rule_source,
                    bool compare, std::int64_t iterations, std::int64_t nodes) {
    if (iterations < 1 || iterations > static_cast<std::int64_t>(max_iterations)) {
        throw std::invalid_argument("iterations must be in 1..32");
    }
    if (nodes < 1 || nodes > static_cast<std::int64_t>(max_enodes)) {
        throw std::invalid_argument("nodes must be in 1..4096");
    }

    const auto term = Parser(source, "term").ground();
    const auto rules = Parser(rule_source, "rules").rules();
    EGraph graph;
    const auto root = graph.add(term);
    std::optional<EClassId> target;
    if (compare) target = graph.add(Parser(other, "other term").ground());
    if (graph.num_enodes() > static_cast<std::size_t>(nodes)) {
        throw std::invalid_argument("input terms exceed the e-node limit");
    }
    const auto search = saturate(graph, rules, root, target, iterations, nodes);
    const auto status = compare ? (search.stop == "equivalent" ? "equivalent" : "unknown")
                                : search.stop;
    const auto bound = tree_size(term);
    const auto best = graph.extract(root, TreeSizeCost{}, bound);
    if (best.cost > bound) throw std::logic_error("input representative was lost");
    std::string extracted;
    render(extracted, best.term);
    return "{\"status\":" + quoted(status) + ",\"stop\":" + quoted(search.stop) +
           ",\"term\":" + quoted(extracted) + ",\"cost\":" + std::to_string(best.cost) +
           ",\"iterations\":" + std::to_string(search.iterations) +
           ",\"enodes\":" + std::to_string(graph.num_enodes()) +
           ",\"unions\":" + std::to_string(graph.stats().unions_performed) + "}";
}

} // namespace

extern "C" const char *llsynth_egraph_run(const char *term, const char *other, const char *rules,
                                        int compare, std::int64_t iterations,
                                        std::int64_t nodes) noexcept {
    // The LLSynth native bridge copies the result immediately.
    try {
        static thread_local std::string result;
        try {
            result = analyze(term, other, rules, compare != 0, iterations, nodes);
        } catch (const std::invalid_argument &error) {
            result = "{\"status\":\"error\",\"message\":" + quoted(error.what()) + "}";
        }
        return result.c_str();
    } catch (...) {
        return R"({"status":"error","message":"EquinoxNG analysis failed"})";
    }
}
