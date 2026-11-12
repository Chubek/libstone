#pragma once
#include "Core.hpp"
namespace moonstone::sexpr {
struct Node {
    std::string atom;
    std::vector<Node> children;
    bool list = false;
    static Node symbol(std::string s) { return {std::move(s), {}, false}; }
    static Node sequence(std::vector<Node> n) { return {{}, std::move(n), true}; }
    bool operator==(const Node &) const = default;
};
class Parser {
    std::string_view s_;
    std::size_t pos_ = 0, depth_ = 0, nodes_ = 0;
    std::size_t max_nodes_, max_depth_;
    void skip() {
        while (pos_ < s_.size()) {
            if (std::isspace((unsigned char)s_[pos_])) {
                ++pos_;
                continue;
            }
            if (s_[pos_] == ';') {
                while (pos_ < s_.size() && s_[pos_] != '\n')
                    ++pos_;
                continue;
            }
            break;
        }
    }
    Node parse_node() {
        skip();
        if (pos_ == s_.size())
            throw std::invalid_argument("unexpected end of s-expression");
        if (++nodes_ > max_nodes_)
            throw std::length_error("s-expression node limit");
        if (s_[pos_] == '(') {
            if (++depth_ > max_depth_)
                throw std::length_error("s-expression nesting limit");
            ++pos_;
            std::vector<Node> xs;
            for (;;) {
                skip();
                if (pos_ >= s_.size())
                    throw std::invalid_argument("unterminated list");
                if (s_[pos_] == ')') {
                    ++pos_;
                    --depth_;
                    return Node::sequence(std::move(xs));
                }
                xs.push_back(parse_node());
            }
        }
        if (s_[pos_] == ')')
            throw std::invalid_argument("unexpected ')'");
        if (s_[pos_] == '"') {
            ++pos_;
            std::string v;
            bool closed = false;
            while (pos_ < s_.size()) {
                char c = s_[pos_++];
                if (c == '"') {
                    closed = true;
                    break;
                }
                if (c == '\\') {
                    if (pos_ == s_.size())
                        throw std::invalid_argument("trailing escape");
                    c = s_[pos_++];
                    if (c == 'n')
                        c = '\n';
                    else if (c == 't')
                        c = '\t';
                    else if (c == 'r')
                        c = '\r';
                    else if (c != '"' && c != '\\')
                        throw std::invalid_argument("unsupported string escape");
                }
                v += c;
            }
            if (!closed)
                throw std::invalid_argument("unterminated string");
            return Node::symbol(std::move(v));
        }
        auto start = pos_;
        while (pos_ < s_.size() && !std::isspace((unsigned char)s_[pos_]) && s_[pos_] != '(' &&
               s_[pos_] != ')' && s_[pos_] != ';')
            ++pos_;
        if (start == pos_)
            throw std::invalid_argument("invalid atom");
        return Node::symbol(std::string(s_.substr(start, pos_ - start)));
    }

  public:
    explicit Parser(std::string_view s, std::size_t max_nodes = 100000, std::size_t max_depth = 256)
        : s_(s), max_nodes_(max_nodes), max_depth_(max_depth) {}
    std::vector<Node> all() {
        std::vector<Node> n;
        skip();
        while (pos_ < s_.size()) {
            n.push_back(parse_node());
            skip();
        }
        return n;
    }
    Node one() {
        auto xs = all();
        if (xs.size() != 1)
            throw std::invalid_argument("expected exactly one expression");
        return xs.front();
    }
};
inline std::string print(const Node &n) {
    if (!n.list) {
        bool quoted =
            n.atom.empty() || n.atom.find_first_of(" \n\t\r\v\f()\";") != std::string::npos;
        if (!quoted)
            return n.atom;
        std::string s = "\"";
        for (char c : n.atom) {
            if (c == '\\' || c == '"')
                s += '\\';
            if (c == '\n') {
                s += "\\n";
                continue;
            }
            if (c == '\t') {
                s += "\\t";
                continue;
            }
            if (c == '\r') {
                s += "\\r";
                continue;
            }
            s += c;
        }
        return s + '"';
    }
    std::string s = "(";
    for (std::size_t i = 0; i < n.children.size(); ++i) {
        if (i)
            s += ' ';
        s += print(n.children[i]);
    }
    return s + ")";
}
using Bindings = std::map<std::string, Node>;
inline bool match(const Node &pattern, const Node &value, Bindings &binds) {
    if (!pattern.list && !pattern.atom.empty() && pattern.atom[0] == '?' &&
        pattern.atom.size() > 1) {
        auto [it, inserted] = binds.emplace(pattern.atom, value);
        return inserted || it->second == value;
    }
    if (pattern.list != value.list)
        return false;
    if (!pattern.list)
        return pattern.atom == value.atom;
    if (pattern.children.size() != value.children.size())
        return false;
    for (std::size_t i = 0; i < pattern.children.size(); ++i)
        if (!match(pattern.children[i], value.children[i], binds))
            return false;
    return true;
}
inline std::optional<Bindings> match(const Node &pattern, const Node &value) {
    Bindings b;
    if (match(pattern, value, b))
        return b;
    return std::nullopt;
}
inline Node substitute(const Node &p, const Bindings &b) {
    if (!p.list && !p.atom.empty() && p.atom[0] == '?') {
        auto it = b.find(p.atom);
        if (it == b.end())
            throw std::invalid_argument("unbound pattern variable");
        return it->second;
    }
    if (!p.list)
        return p;
    std::vector<Node> out;
    for (auto &c : p.children)
        out.push_back(substitute(c, b));
    return Node::sequence(std::move(out));
}
inline const Node *property(const Node &form, std::string_view key) {
    if (!form.list)
        return nullptr;
    for (auto &child : form.children)
        if (child.list && child.children.size() >= 2 && !child.children[0].list &&
            child.children[0].atom == key)
            return &child;
    return nullptr;
}
inline std::string property_atom(const Node &form, std::string_view key,
                                 std::string fallback = {}) {
    auto p = property(form, key);
    return p && !p->children[1].list ? p->children[1].atom : std::move(fallback);
}
} // namespace moonstone::sexpr
