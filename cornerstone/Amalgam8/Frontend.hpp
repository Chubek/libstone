#pragma once

#include "Contracts.hpp"
#include "LibDSL.hpp"

#include <array>
#include <fstream>
#include <utility>

namespace am8 {

inline dsl::SExpr to_sexpr(const Syntax& syntax) {
    if (syntax.is_atom) return dsl::SExpr::atom(syntax.text, syntax.quoted);
    std::vector<dsl::SExpr> items;
    items.reserve(syntax.items.size());
    for (const auto& item : syntax.items) items.push_back(to_sexpr(item));
    return dsl::SExpr::list(std::move(items));
}

namespace frontend_detail {

// Match LibDSL's bounded S-expression reader, retaining exact source ranges
// while consuming the original bytes. Line and byte column are one-based.
class Reader {
    std::string_view source_;
    const std::string& filename_;
    std::size_t& nodes_left_;
    std::size_t max_depth_;
    std::size_t pos_{}, line_{1}, column_{1};

    Span at() const { return {filename_, pos_, pos_, line_, column_}; }

    char take() {
        char c = source_[pos_++];
        if (c == '\n') {
            ++line_;
            column_ = 1;
        } else {
            ++column_;
        }
        return c;
    }

    void skip() {
        while (pos_ < source_.size()) {
            if (static_cast<unsigned char>(source_[pos_]) <= ' ') {
                take();
            } else if (source_[pos_] == ';') {
                while (pos_ < source_.size() && source_[pos_] != '\n') take();
            } else {
                break;
            }
        }
    }

    Syntax node(std::size_t depth) {
        skip();
        auto span = at();
        if (depth > max_depth_) fail(span, "syntax nesting limit exceeded");
        if (nodes_left_ == 0) fail(span, "syntax node limit exceeded");
        if (pos_ == source_.size()) fail(span, "expected expression");
        if (source_[pos_] == ')') fail(span, "unexpected ')'");
        --nodes_left_;

        Syntax result;
        result.span = std::move(span);
        if (source_[pos_] == '(') {
            take();
            for (;;) {
                skip();
                if (pos_ == source_.size()) fail(at(), "unterminated list");
                if (source_[pos_] == ')') {
                    take();
                    break;
                }
                result.items.push_back(node(depth + 1));
            }
        } else if (source_[pos_] == '"') {
            result.is_atom = result.quoted = true;
            take();
            for (;;) {
                if (pos_ == source_.size()) fail(at(), "unterminated quoted atom");
                char c = take();
                if (c == '"') break;
                if (c == '\\') {
                    if (pos_ == source_.size()) fail(at(), "unfinished escape");
                    auto escape = at();
                    c = take();
                    switch (c) {
                    case 'n': c = '\n'; break;
                    case 'r': c = '\r'; break;
                    case 't': c = '\t'; break;
                    case '\\': case '"': break;
                    default: fail(escape, "unknown escape");
                    }
                }
                result.text += c;
            }
        } else {
            result.is_atom = true;
            while (pos_ < source_.size()) {
                char c = source_[pos_];
                if (static_cast<unsigned char>(c) <= ' ' ||
                    c == '(' || c == ')' || c == ';') break;
                // LibDSL's atom factory marks these bytes as requiring quotes,
                // including when they occurred within a bare token.
                if (c == '"' || c == '\\') result.quoted = true;
                result.text += take();
            }
        }
        result.span.end = pos_;
        return result;
    }

public:
    Reader(std::string_view source, const std::string& filename,
           std::size_t& nodes_left, std::size_t max_depth)
        : source_(source), filename_(filename), nodes_left_(nodes_left),
          max_depth_(std::min<std::size_t>(512, max_depth)) {}

    Syntax read() {
        auto root = node(0);
        skip();
        if (pos_ != source_.size()) fail(at(), "expected exactly one S-expression");
        return root;
    }
};

struct PendingImport {
    fs::path path;
    std::string alias;
    Span span;
    bool resolved{};
};

struct Frame {
    Module module;
    fs::path key, directory;
    std::vector<PendingImport> pending;
    std::map<std::string, std::size_t> aliases;
    std::set<std::pair<std::size_t, std::string>> edges;
    std::size_t next{};
};

inline Frame read_module(Syntax root, fs::path key, fs::path directory) {
    if ((head(root) != "library" && head(root) != "module") || root.items.size() < 2)
        fail(root, "expected (library NAME ...) or (module NAME ...)");

    Frame frame;
    frame.key = std::move(key);
    frame.directory = std::move(directory);
    frame.module.name = identifier(root.items[1]);
    frame.module.span = std::move(root.span);
    for (std::size_t i = 2; i < root.items.size(); ++i) {
        auto& form = root.items[i];
        if (head(form) == "import") {
            if (form.items.size() < 2 || form.items.size() > 3)
                fail(form, "expected (import \"path\" [Alias])");
            const auto text = quoted_text(form.items[1]);
            fs::path path(text);
            if (text.empty() || text.find('\0') != std::string::npos || !path.is_relative())
                fail(form.items[1], "expected a nonempty relative import path");
            auto alias = form.items.size() == 3 ? identifier(form.items[2]) : std::string{};
            frame.pending.push_back({std::move(path), std::move(alias), std::move(form.span)});
        } else if (head(form) == "export") {
            if (form.items.size() < 2) fail(form, "expected (export NAME ...)");
            for (std::size_t j = 1; j < form.items.size(); ++j) {
                auto name = identifier(form.items[j]);
                if (!frame.module.exports.insert(name).second)
                    fail(form.items[j], "duplicate export '" + name + "'");
            }
        } else {
            frame.module.forms.push_back(std::move(form));
        }
    }
    return frame;
}

class Loader {
    Limits limits_;
    std::size_t bytes_left_, nodes_left_;
    Program program_;
    std::vector<Frame> stack_;
    // Missing from both tables means not visited. Only completed modules have IDs.
    std::map<fs::path, std::size_t> active_, complete_;

    static fs::path canonical(const fs::path& path, const Span& where) {
        std::error_code ec;
        auto result = fs::canonical(path, ec);
        if (ec) fail(where, "cannot open input '" + path.string() + "': " + ec.message());
        if (!fs::is_regular_file(result, ec) || ec)
            fail(where, "input is not a regular file: '" + result.string() + "'");
        return result;
    }

    Syntax read_file(const fs::path& path, const Span& where, const std::string& filename) {
        std::ifstream stream(path, std::ios::binary);
        if (!stream) fail(where, "cannot open input '" + path.string() + "'");
        std::string source;
        std::array<char, 4096> chunk;
        for (;;) {
            // At most one byte beyond the remaining shared budget is read.
            const auto count = bytes_left_ < chunk.size() ? bytes_left_ + 1 : chunk.size();
            stream.read(chunk.data(), static_cast<std::streamsize>(count));
            const auto got = static_cast<std::size_t>(stream.gcount());
            if (stream.bad() || (stream.fail() && !stream.eof()))
                fail(where, "input read failed for '" + path.string() + "'");
            if (got > bytes_left_)
                fail(where, "source byte limit exceeded reading '" + path.string() + "'");
            bytes_left_ -= got;
            source.append(chunk.data(), got);
            if (stream.eof()) break;
        }
        return Reader(source, filename, nodes_left_, limits_.syntax_depth).read();
    }

    void check_module_limit(const Span& where) const {
        if (program_.modules.size() >= limits_.modules ||
            stack_.size() >= limits_.modules - program_.modules.size())
            fail(where, "module limit exceeded");
    }

    void push_file(const fs::path& path, const Span& where, const std::string& filename) {
        check_module_limit(where);
        auto syntax = read_file(path, where, filename);
        active_.emplace(path, stack_.size());
        stack_.push_back(read_module(std::move(syntax), path, path.parent_path()));
    }

    void count_memory(const Syntax& syntax, std::size_t depth) {
        if (depth > std::min<std::size_t>(512, limits_.syntax_depth))
            fail(syntax, "syntax nesting limit exceeded");
        if (nodes_left_ == 0) fail(syntax, "syntax node limit exceeded");
        --nodes_left_;
        if (syntax.text.size() > bytes_left_) fail(syntax, "source byte limit exceeded");
        bytes_left_ -= syntax.text.size();
        for (const auto& item : syntax.items) count_memory(item, depth + 1);
    }

    Program finish() {
        while (!stack_.empty()) {
            auto& frame = stack_.back();
            if (frame.next == frame.pending.size()) {
                if (!frame.key.empty()) {
                    complete_.emplace(frame.key, program_.modules.size());
                    active_.erase(frame.key);
                }
                program_.modules.push_back(std::move(frame.module));
                stack_.pop_back();
                continue;
            }
            auto& item = frame.pending[frame.next];
            if (!item.resolved) {
                item.path = canonical(frame.directory / item.path, item.span);
                item.resolved = true;
            }
            const auto& key = item.path;
            if (auto done = complete_.find(key); done != complete_.end()) {
                if (!item.alias.empty()) {
                    auto [alias, fresh] = frame.aliases.emplace(item.alias, done->second);
                    if (!fresh && alias->second != done->second)
                        fail(item.span, "duplicate import alias '" + item.alias + "'");
                }
                if (frame.edges.emplace(done->second, item.alias).second)
                    frame.module.imports.push_back({done->second, item.alias, item.span});
                ++frame.next;
            } else if (auto cycle = active_.find(key); cycle != active_.end()) {
                std::string message = "import cycle: ";
                for (std::size_t i = cycle->second; i < stack_.size(); ++i)
                    message += stack_[i].key.string() + " -> ";
                fail(item.span, message + key.string());
            } else {
                push_file(key, item.span, key.string());
            }
        }
        program_.root = program_.modules.size() - 1;
        return std::move(program_);
    }

public:
    explicit Loader(const Limits& limits)
        : limits_(limits), bytes_left_(limits.source_bytes), nodes_left_(limits.syntax_nodes) {}

    Program file(const fs::path& input) {
        const auto filename = input.string();
        const Span where{filename};
        push_file(canonical(input, where), where, filename);
        return finish();
    }

    Program memory(Syntax root, const fs::path& base) {
        check_module_limit(root.span);
        count_memory(root, 0);
        // The source buffer no longer exists. Use the root's byte end offset
        // when available, or at least the text bytes in a synthetic tree.
        if (root.span.end > limits_.source_bytes) fail(root, "source byte limit exceeded");
        bytes_left_ = std::min(bytes_left_, limits_.source_bytes - root.span.end);
        stack_.push_back(read_module(std::move(root), {}, base));
        return finish();
    }
};

} // namespace frontend_detail

inline Syntax parse(std::string_view source, std::string filename, const Limits& limits = {}) {
    if (source.size() > limits.source_bytes) fail(Span{filename}, "source byte limit exceeded");
    auto nodes_left = limits.syntax_nodes;
    return frontend_detail::Reader(source, filename, nodes_left, limits.syntax_depth).read();
}

inline Program load(const fs::path& input, const Limits& limits = {}) {
    return frontend_detail::Loader(limits).file(input);
}

// For an embedded root, base is the directory used for its relative imports.
inline Program load(Syntax root, const fs::path& base, const Limits& limits = {}) {
    return frontend_detail::Loader(limits).memory(std::move(root), base);
}

} // namespace am8
