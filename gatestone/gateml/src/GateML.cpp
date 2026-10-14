#include <gateml/GateML.hpp>
#include <GateMLParser.hpp>
#include <chomsky3.h>
#include <algorithm>
#include <fstream>
#include <functional>
#include <limits>
#include <set>
#include <sstream>

namespace gateml {
namespace {
constexpr const char* alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
Span location(const std::string& source, size_t begin, size_t end) {
    Span span{begin, end, 1, 1};
    for (size_t i = 0; i < begin && i < source.size(); ++i) {
        if (source[i] == '\n') { ++span.line; span.column = 1; }
        else ++span.column;
    }
    return span;
}
struct Token { std::string name, text; char code; Span span; };
struct Lexed { std::vector<Token> tokens; std::string tape; Span eof; };
using Context = std::unique_ptr<chomsky3_context_t, decltype(&chomsky3_context_free)>;
using Pattern = std::unique_ptr<chomsky3_pattern_t, decltype(&chomsky3_pattern_free)>;
class Lexer {
    Context context_{chomsky3_context_new(), chomsky3_context_free};
    std::vector<Pattern> patterns_;
    std::vector<Pattern> alternatives_;
    const std::vector<LexRule>& rules_;
public:
    explicit Lexer(const std::vector<LexRule>& rules) : rules_(rules) {
        if (!context_) throw std::bad_alloc();
        for (const auto& rule : rules) {
            chomsky3_error_t error;
            auto regex = "^(" + rule.pattern + ")";
            Pattern pattern(chomsky3_compile(context_.get(), regex.c_str(),
                CHOMSKY3_TARGET_BYTECODE, CHOMSKY3_FLAG_NONE, &error), chomsky3_pattern_free);
            if (!pattern) throw Error("invalid pattern for " + rule.name + ": " + chomsky3_error_string(error), rule.span);
            chomsky3_match_t match{};
            bool nullable = chomsky3_exec(pattern.get(), "", 0, &match);
            chomsky3_match_free(&match);
            if (nullable) throw Error("nullable lexical rule: " + rule.name, rule.span);
            patterns_.push_back(std::move(pattern));
            // The regex VM returns the first successful alternation. Full-prefix
            // checks recover the longest lexical match without changing libchomsky3.
            bool alternate = false, escaped = false, in_class = false;
            for (char c : rule.pattern) {
                if (escaped) { escaped = false; continue; }
                if (c == '\\') { escaped = true; continue; }
                if (c == '[') in_class = true;
                else if (c == ']') in_class = false;
                else if (c == '|' && !in_class) alternate = true;
            }
            Pattern full(nullptr, chomsky3_pattern_free);
            if (alternate) {
                regex += '$';
                full.reset(chomsky3_compile(context_.get(), regex.c_str(), CHOMSKY3_TARGET_BYTECODE,
                    CHOMSKY3_FLAG_NONE, &error));
                if (!full) throw Error("cannot compile full-prefix pattern: " + rule.name, rule.span);
            }
            alternatives_.push_back(std::move(full));
        }
    }
    Lexed scan(const std::string& source, const std::vector<char>& codes) {
        Lexed result;
        size_t offset = 0, line = 1, column = 1;
        while (offset < source.size()) {
            size_t best = 0, chosen = 0;
            for (size_t i = 0; i < patterns_.size(); ++i) {
                chomsky3_match_t match{};
                size_t length = 0;
                if (chomsky3_exec(patterns_[i].get(), source.data() + offset, source.size() - offset, &match)
                    && match.start == source.data() + offset) length = match.length;
                chomsky3_match_free(&match);
                if (length && alternatives_[i]) {
                    for (size_t candidate = source.size() - offset; candidate > std::max(length, best); --candidate) {
                        match = {};
                        bool full = chomsky3_exec(alternatives_[i].get(), source.data() + offset, candidate, &match)
                            && match.start == source.data() + offset && match.length == candidate;
                        chomsky3_match_free(&match);
                        if (full) { length = candidate; break; }
                    }
                }
                if (length > best) { best = length; chosen = i; }
            }
            if (!best) throw Error("unrecognized input byte", {offset, offset + 1, line, column});
            if (!rules_[chosen].skip) {
                result.tokens.push_back({rules_[chosen].name, source.substr(offset, best), codes.at(chosen),
                    {offset, offset + best, line, column}});
                result.tape += codes.at(chosen);
            }
            for (size_t i = offset; i < offset + best; ++i) {
                if (source[i] == '\n') { ++line; column = 1; }
                else ++column;
            }
            offset += best;
        }
        result.eof = {offset, offset, line, column};
        return result;
    }
};
Span tape_span(const Lexed& lexed, size_t begin, size_t end) {
    Span span = begin < lexed.tokens.size() ? lexed.tokens[begin].span : lexed.eof;
    span.end = end > begin && end <= lexed.tokens.size() ? lexed.tokens[end - 1].span.end : span.begin;
    return span;
}
void run(novoparse::Parser& parser, const Lexed& lexed) {
    try { parser.parse(lexed.tape); }
    catch (const novoparse::ParseError& error) {
        auto message = error.status == NP_ERROR_SYNTAX ? "syntax error" : error.what();
        throw Error(message, tape_span(lexed, error.position, error.position + 1));
    }
}
std::string unquote(const Token& token) {
    std::string out;
    for (size_t i = 1; i + 1 < token.text.size(); ++i) {
        char c = token.text[i];
        if (c == '\\') {
            c = token.text[++i];
            switch (c) {
                case 'n': c = '\n'; break;
                case 'r': c = '\r'; break;
                case 't': c = '\t'; break;
                case '\\': case '"': break;
                default: throw Error("unsupported string escape", token.span);
            }
        }
        if (c == '\n' && token.text[i] == '\n') throw Error("newline in string literal", token.span);
        out += c;
    }
    return out;
}
void visit(const np_node* node, const std::function<void(const np_node*)>& fn) {
    fn(node);
    for (size_t i = 0; i < np_node_child_count(node); ++i) visit(np_node_child(node, i), fn);
}
bool named(const np_node* node, const char* name) { return std::string(np_node_name(node)) == name; }
std::vector<char> token_codes(const Frontend& frontend) {
    std::vector<char> codes;
    size_t count = 0;
    for (const auto& rule : frontend.lexer) {
        if (!rule.skip && count >= 62) throw Error("at most 62 named terminals are supported", rule.span);
        codes.push_back(rule.skip ? '\0' : alphabet[count++]);
    }
    return codes;
}
void validate(const Frontend& frontend) {
    std::set<std::string> terminals, skipped, heads, nodes;
    auto identifier = [](const std::string& name, Span span) {
        if (name.empty() || name.rfind("__np_", 0) == 0)
            throw Error("empty or NovoParse-reserved identifier: " + name, span);
    };
    identifier(frontend.name, {});
    if (frontend.lexer.empty()) throw Error("frontend requires lexical rules", {});
    for (const auto& rule : frontend.lexer) {
        identifier(rule.name, rule.span);
        if (terminals.count(rule.name) || skipped.count(rule.name)) throw Error("duplicate lexical rule: " + rule.name, rule.span);
        (rule.skip ? skipped : terminals).insert(rule.name);
    }
    for (const auto& rule : frontend.productions) {
        identifier(rule.head, rule.span);
        if (terminals.count(rule.head) || skipped.count(rule.head)) throw Error("terminal/nonterminal collision: " + rule.head, rule.span);
        heads.insert(rule.head);
    }
    if (!heads.count(frontend.start)) throw Error("undefined start symbol: " + frontend.start, {});
    for (const auto& name : frontend.nodes) {
        identifier(name, {});
        if (!nodes.insert(name).second) throw Error("duplicate AST node: " + name, {});
    }
    std::set<std::string> signatures;
    for (const auto& rule : frontend.productions) {
        std::string signature = rule.head + ":";
        size_t values = 0;
        for (const auto& symbol : rule.body) {
            if (!heads.count(symbol) && !terminals.count(symbol)) throw Error("undefined grammar symbol: " + symbol, rule.span);
            if (heads.count(symbol)) ++values;
            signature += symbol + " ";
        }
        if (!signatures.insert(signature).second) throw Error("duplicate production: " + rule.head, rule.span);
        if (!nodes.count(rule.node)) throw Error("undefined AST node: " + rule.node, rule.span);
        auto hook = frontend.lowering.find(rule.node);
        if (hook != frontend.lowering.end()) {
            const auto& op = hook->second;
            if ((op == "add" || op == "sub" || op == "mul" || op == "sdiv" || op == "srem" ||
                 op == "band" || op == "bor" || op == "bxor" || op == "shl" || op == "lshr" || op == "ashr") && values != 2)
                throw Error("binary lowering requires two nonterminal children: " + rule.node, rule.span);
            if ((op == "identity" || op == "neg" || op == "not") && values != 1)
                throw Error("unary lowering requires one nonterminal child: " + rule.node, rule.span);
            if (op == "integer" && (rule.body.size() != 1 || values != 0))
                throw Error("integer lowering requires one terminal: " + rule.node, rule.span);
        }
    }
    for (const auto& hook : frontend.lowering)
        if (!nodes.count(hook.first)) throw Error("lowering references undefined AST node: " + hook.first, {});
    std::set<std::string> productive;
    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto& rule : frontend.productions) {
            if (std::all_of(rule.body.begin(), rule.body.end(), [&](const auto& s) { return terminals.count(s) || productive.count(s); }))
                changed |= productive.insert(rule.head).second;
        }
    }
    for (const auto& head : heads) if (!productive.count(head)) throw Error("nonproductive grammar symbol: " + head, {});
    token_codes(frontend);
    Lexer lexer(frontend.lexer); // Validate patterns at definition time.
}
std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot open " + path.string());
    std::ostringstream source; source << input.rdbuf();
    if (input.bad()) throw std::runtime_error("cannot read " + path.string());
    return source.str();
}
struct RuntimeGrammar {
    std::vector<std::string> names, literals;
    std::vector<np_symbol_spec> symbols;
    std::vector<std::vector<int>> bodies;
    std::vector<np_production_spec> productions;
    np_grammar_spec spec{};
    explicit RuntimeGrammar(const Frontend& frontend) {
        auto codes = token_codes(frontend);
        std::map<std::string, int> ids;
        for (size_t i = 0; i < frontend.lexer.size(); ++i) {
            if (frontend.lexer[i].skip) continue;
            ids.emplace(frontend.lexer[i].name, static_cast<int>(names.size()));
            names.push_back(frontend.lexer[i].name);
            literals.emplace_back(1, codes[i]);
        }
        size_t terminals = names.size();
        for (const auto& rule : frontend.productions) if (!ids.count(rule.head)) {
            ids.emplace(rule.head, static_cast<int>(names.size()));
            names.push_back(rule.head);
        }
        for (size_t i = 0; i < names.size(); ++i)
            symbols.push_back({i < terminals ? NP_LITERAL : NP_NONTERMINAL, names[i].c_str(),
                i < terminals ? literals[i].c_str() : nullptr, 0, NP_ASSOC_NONE});
        bodies.reserve(frontend.productions.size());
        for (const auto& rule : frontend.productions) {
            bodies.emplace_back();
            for (const auto& symbol : rule.body) bodies.back().push_back(ids.at(symbol));
            productions.push_back({ids.at(rule.head), bodies.back().data(), bodies.back().size(),
                rule.node.c_str(), 0, NP_ASSOC_NONE});
        }
        spec = {frontend.name.c_str(), symbols.data(), symbols.size(), productions.data(), productions.size(),
            ids.at(frontend.start), NP_SKIP_NONE, nullptr, NP_REJECT, nullptr};
    }
};
Tree copy_tree(const np_node* node, const Lexed& lexed) {
    Tree tree;
    tree.name = np_node_name(node);
    size_t begin = np_node_start(node), end = np_node_end(node);
    tree.span = tape_span(lexed, begin, end);
    tree.terminal = np_node_text(node) != nullptr;
    if (tree.terminal) tree.text = lexed.tokens.at(begin).text;
    for (size_t i = 0; i < np_node_child_count(node); ++i) tree.children.push_back(copy_tree(np_node_child(node, i), lexed));
    return tree;
}
std::string quote(const std::string& value) {
    std::string result = "\"";
    const char* hex = "0123456789abcdef";
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') { result += '\\'; result += static_cast<char>(c); }
        else if (c < 32) { result += "\\u00"; result += hex[c >> 4]; result += hex[c & 15]; }
        else result += static_cast<char>(c);
    }
    return result + '"';
}
}

Frontend compile(const std::string& source) {
    auto nul = source.find('\0');
    if (nul != std::string::npos) throw Error("NUL byte in GateML definition", location(source, nul, nul + 1));
    std::vector<LexRule> rules = {
        {"WS", "[ \t\r\n]+", true, {}}, {"COMMENT", "//[^\n]*", true, {}},
        {"IDENT", "[A-Za-z_][A-Za-z0-9_]*", false, {}},
        {"STRING", R"re("([^"\\]|\\.)*")re", false, {}},
        {"ARROW", "=>", false, {}}, {"PUNCT", "[{};=|]", false, {}}
    };
    Lexer lexer(rules);
    auto lexed = lexer.scan(source, {'\0', '\0', 'i', 's', '>', '?'});
    const std::map<std::string, char> keywords = {{"frontend", 'A'}, {"lexer", 'B'}, {"token", 'C'},
        {"skip", 'D'}, {"parser", 'E'}, {"start", 'F'}, {"rule", 'G'}, {"ast", 'H'},
        {"node", 'I'}, {"adapter", 'J'}, {"lower", 'K'}, {"profile", 'L'}, {"option", 'M'}};
    for (size_t i = 0; i < lexed.tokens.size(); ++i) {
        auto& token = lexed.tokens[i];
        if (token.name == "IDENT" && keywords.count(token.text)) token.code = keywords.at(token.text);
        if (token.name == "PUNCT") token.code = token.text.front();
        lexed.tape[i] = token.code;
    }
    GateMLParser parser;
    run(parser, lexed);
    Frontend frontend;
    frontend.name = lexed.tokens.at(1).text;
    bool has_start = false;
    visit(parser.root(), [&](const np_node* node) {
        auto begin = np_node_start(node);
        auto at = [&](size_t n) -> const Token& { return lexed.tokens.at(begin + n); };
        if (named(node, "Token") || named(node, "Skip"))
            frontend.lexer.push_back({at(1).text, unquote(at(3)), named(node, "Skip"), at(1).span});
        else if (named(node, "Start")) {
            if (has_start) throw Error("duplicate start declaration", at(1).span);
            frontend.start = at(1).text; has_start = true;
        } else if (named(node, "Node")) frontend.nodes.push_back(at(1).text);
        else if (named(node, "Lower")) {
            if (!frontend.lowering.emplace(at(1).text, at(3).text).second) throw Error("duplicate lowering: " + at(1).text, at(1).span);
        } else if (named(node, "Option")) {
            if (!frontend.options.emplace(at(1).text, unquote(at(3))).second) throw Error("duplicate option: " + at(1).text, at(1).span);
        } else if (named(node, "Rule")) {
            auto head = at(1).text;
            visit(node, [&](const np_node* alternative) {
                if (!named(alternative, "Alternative")) return;
                size_t b = np_node_start(alternative), e = np_node_end(alternative);
                Production production{head, {}, lexed.tokens.at(e - 1).text, at(1).span};
                for (size_t i = b; i + 2 < e; ++i) production.body.push_back(lexed.tokens.at(i).text);
                frontend.productions.push_back(std::move(production));
            });
        }
    });
    validate(frontend);
    return frontend;
}
Frontend load(const std::filesystem::path& path) {
    if (!std::filesystem::is_directory(path)) {
        try { return compile(read_file(path)); }
        catch (Error& error) { error.source = path.string(); throw; }
    }
    std::istringstream manifest(read_file(path / "MANIFEST"));
    std::string entry, source;
    std::set<std::string> seen;
    struct Part { std::filesystem::path path; std::string text; size_t begin; };
    std::vector<Part> parts;
    while (std::getline(manifest, entry)) {
        if (!entry.empty() && entry.back() == '\r') entry.pop_back();
        if (entry.empty() || entry.front() == '#') continue;
        std::filesystem::path relative(entry);
        if (relative.is_absolute() || relative.has_parent_path() || relative.extension() != ".gml")
            throw std::runtime_error("MANIFEST entries must be local .gml filenames: " + entry);
        if (!seen.insert(entry).second) throw std::runtime_error("duplicate MANIFEST entry: " + entry);
        parts.push_back({path / relative, read_file(path / relative), source.size()});
        source += parts.back().text + "\n";
    }
    try { return compile(source); }
    catch (Error& error) {
        for (auto part = parts.rbegin(); part != parts.rend(); ++part) if (error.span.begin >= part->begin) {
            auto begin = std::min(error.span.begin - part->begin, part->text.size());
            auto end = std::min(error.span.end - part->begin, part->text.size());
            error.span = location(part->text, begin, end);
            error.source = part->path.string();
            break;
        }
        throw;
    }
}
Tree parse(const Frontend& frontend, const std::string& source) {
    validate(frontend);
    Lexer lexer(frontend.lexer);
    auto lexed = lexer.scan(source, token_codes(frontend));
    RuntimeGrammar grammar(frontend);
    novoparse::Parser parser(grammar.spec);
    run(parser, lexed);
    return copy_tree(parser.root(), lexed);
}
std::unique_ptr<jiterati::Module> lower(const Frontend& frontend, const Tree& tree, const LoweringHooks& facilities) {
    auto module = std::make_unique<jiterati::Module>(frontend.name);
    auto* function = module->create_function("entry", jiterati::Type::i64(), {});
    auto* block = function->create_block("entry");
    std::function<jiterati::Value(const Tree&)> emit = [&](const Tree& node) -> jiterati::Value {
        auto hook = frontend.lowering.find(node.name);
        if (hook == frontend.lowering.end()) throw Error("missing lowering hook: " + node.name, node.span);
        const auto& op = hook->second;
        if (op == "integer" && !facilities.count(op)) {
            if (node.children.size() != 1 || !node.children[0].terminal) throw Error("integer hook requires one terminal", node.span);
            const auto& text = node.children[0].text;
            try {
                size_t used = 0;
                auto value = std::stoll(text, &used, 10);
                if (used != text.size()) throw std::invalid_argument("integer suffix");
                return function->const_i64(value);
            } catch (const std::exception&) { throw Error("invalid or overflowing i64 literal: " + text, node.span); }
        }
        std::vector<jiterati::Value> values;
        for (const auto& child : node.children) if (!child.terminal) values.push_back(emit(child));
        auto facility = facilities.find(op);
        if (facility != facilities.end()) {
            auto value = facility->second(node, *function, *block, values);
            if (!value.valid() || function->value_type(value) != jiterati::Type::i64())
                throw Error("facility must produce an i64 value: " + op, node.span);
            return value;
        }
        if (op == "identity" || op == "neg" || op == "not") {
            if (values.size() != 1) throw Error("unary hook arity mismatch", node.span);
            if (op == "identity") return values[0];
            if (op == "neg") return block->neg(values[0]);
            return block->bitwise_not(values[0]);
        }
        if (op != "add" && op != "sub" && op != "mul" && op != "sdiv" && op != "srem" &&
            op != "band" && op != "bor" && op != "bxor" && op != "shl" && op != "lshr" && op != "ashr")
            throw Error("unresolved lowering facility: " + op, node.span);
        if (values.size() != 2) throw Error("binary hook arity mismatch: " + op, node.span);
        if (op == "add") return block->add(values[0], values[1]);
        if (op == "sub") return block->sub(values[0], values[1]);
        if (op == "mul") return block->mul(values[0], values[1]);
        if (op == "sdiv") return block->sdiv(values[0], values[1]);
        if (op == "srem") return block->srem(values[0], values[1]);
        if (op == "band") return block->bitwise_and(values[0], values[1]);
        if (op == "bor") return block->bitwise_or(values[0], values[1]);
        if (op == "bxor") return block->bitwise_xor(values[0], values[1]);
        if (op == "shl") return block->shl(values[0], values[1]);
        if (op == "lshr") return block->lshr(values[0], values[1]);
        if (op == "ashr") return block->ashr(values[0], values[1]);
        throw Error("unresolved lowering facility: " + op, node.span);
    };
    block->ret(emit(tree));
    return module;
}
std::string grammar(const Frontend& frontend) {
    auto codes = token_codes(frontend);
    std::map<std::string, char> terminals;
    for (size_t i = 0; i < codes.size(); ++i) if (!frontend.lexer[i].skip) terminals.emplace(frontend.lexer[i].name, codes[i]);
    std::ostringstream out;
    out << "// Token tape grammar; tokenize original input with libchomsky3.\n%grammar " << frontend.name
        << ";\n%start " << frontend.start << ";\n";
    std::set<std::string> emitted;
    for (const auto& first : frontend.productions) {
        if (!emitted.insert(first.head).second) continue;
        out << first.head << ":";
        bool initial = true;
        for (const auto& rule : frontend.productions) {
            if (rule.head != first.head) continue;
            if (!initial) out << "\n    |";
            initial = false;
            if (rule.body.empty()) out << " %empty";
            for (const auto& symbol : rule.body) {
                out << ' ';
                if (terminals.count(symbol)) out << '"' << terminals.at(symbol) << '"';
                else out << symbol;
            }
            out << " => " << rule.node;
        }
        out << ";\n";
    }
    return out.str();
}
std::string descriptor(const Frontend& frontend) {
    auto codes = token_codes(frontend);
    std::ostringstream out;
    out << "{\"format\":\"gateml.frontend\",\"version\":1,\"name\":" << quote(frontend.name)
        << ",\"start\":" << quote(frontend.start) << ",\"value_type\":\"i64\",\"lexer\":[";
    for (size_t i = 0; i < frontend.lexer.size(); ++i) {
        if (i) out << ',';
        const auto& rule = frontend.lexer[i];
        out << "{\"name\":" << quote(rule.name) << ",\"pattern\":" << quote(rule.pattern)
            << ",\"skip\":" << (rule.skip ? "true" : "false") << ",\"code\":";
        if (rule.skip) out << "null"; else out << quote(std::string(1, codes[i]));
        out << '}';
    }
    out << "],\"productions\":[";
    for (size_t i = 0; i < frontend.productions.size(); ++i) {
        if (i) out << ',';
        const auto& rule = frontend.productions[i];
        out << "{\"head\":" << quote(rule.head) << ",\"body\":[";
        for (size_t j = 0; j < rule.body.size(); ++j) { if (j) out << ','; out << quote(rule.body[j]); }
        out << "],\"node\":" << quote(rule.node) << '}';
    }
    out << "],\"nodes\":[";
    for (size_t i = 0; i < frontend.nodes.size(); ++i) { if (i) out << ','; out << quote(frontend.nodes[i]); }
    auto map = [&](const std::map<std::string, std::string>& entries) {
        bool initial = true;
        for (const auto& entry : entries) {
            if (!initial) out << ',';
            initial = false;
            out << quote(entry.first) << ':' << quote(entry.second);
        }
    };
    out << "],\"lowering\":{"; map(frontend.lowering);
    out << "},\"options\":{"; map(frontend.options);
    out << "}}";
    return out.str();
}
std::string json(const Tree& tree) {
    std::ostringstream out;
    out << "{\"name\":" << quote(tree.name) << ",\"start\":" << tree.span.begin << ",\"end\":" << tree.span.end
        << ",\"line\":" << tree.span.line << ",\"column\":" << tree.span.column;
    if (tree.terminal) out << ",\"text\":" << quote(tree.text);
    out << ",\"children\":[";
    for (size_t i = 0; i < tree.children.size(); ++i) {
        if (i) out << ',';
        out << json(tree.children[i]);
    }
    return out.str() + "]}";
}
}
