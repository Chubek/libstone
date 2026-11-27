#pragma once

#include <TTyTk/TTyTk-Encoding.hpp>
#include <TTyUtils/TTyUtils-Config.hpp>
#include <TTyUtils/TTyUtils-Search.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace ttyutils::syntax {

class Scope {
public:
    Scope() = default;
    explicit Scope(std::string name) : name_(intern(std::move(name))) {}
    [[nodiscard]] auto name() const noexcept -> std::string_view {
        return name_ ? std::string_view{*name_} : std::string_view{};
    }
    [[nodiscard]] friend auto operator==(const Scope&, const Scope&) -> bool = default;

private:
    static auto intern(std::string name) -> std::shared_ptr<const std::string>;
    std::shared_ptr<const std::string> name_;
};

/** A resolved TextMate match or begin/end rule. */
struct Rule {
    std::string match{}, begin{}, end{};
    Scope scope{};
};

class Grammar {
public:
    std::string id{};
    std::string name{};
    std::vector<std::string> filename_globs{};
    std::vector<std::string> keywords{};
    Scope root_scope{};
    std::vector<Rule> rules{};
    std::string first_line_match{};

    [[nodiscard]] auto matches_filename(const std::string_view filename) const -> bool {
        const auto separator = filename.find_last_of("/\\");
        const auto basename = separator == std::string_view::npos ? filename : filename.substr(separator + 1);
        for (const auto& glob : filename_globs) {
            if (matches_glob(glob, glob.find('/') == std::string::npos ? basename : filename)) return true;
        }
        return false;
    }

private:
    [[nodiscard]] static auto matches_glob(std::string_view glob, std::string_view value) -> bool {
        std::size_t i = 0, j = 0, star = std::string_view::npos, retry = 0;
        while (j < value.size()) {
            if (i < glob.size() && glob[i] == '*') { star = i++; retry = j; }
            else if (i < glob.size() && (glob[i] == '?' || glob[i] == value[j])) { ++i; ++j; }
            else if (star != std::string_view::npos) { i = star + 1; j = ++retry; }
            else return false;
        }
        while (i < glob.size() && glob[i] == '*') ++i;
        return i == glob.size();
    }
};

namespace detail {

struct Json {
    std::string text;
    std::vector<Json> array;
    std::vector<std::string> keys;
    std::vector<Json> fields;

    [[nodiscard]] auto field(std::string_view key) const -> const Json& {
        for (std::size_t i = 0; i < keys.size(); ++i) if (keys[i] == key) return fields[i];
        static const Json missing;
        return missing;
    }
};

struct InvalidJson {};

/** Bounded recursive parser for grammar files, including JSON string escapes. */
class JsonParser {
public:
    explicit JsonParser(std::string_view text) : text_(text) {}
    auto parse() -> Json {
        auto value = parse_value(0);
        whitespace();
        if (!text_.empty()) throw InvalidJson{};
        return value;
    }

private:
    auto accept(char c) -> bool {
        whitespace();
        if (!text_.starts_with(c)) return false;
        text_.remove_prefix(1);
        return true;
    }
    void whitespace() {
        const auto at = text_.find_first_not_of(" \n\r\t");
        text_.remove_prefix(at == std::string_view::npos ? text_.size() : at);
    }
    auto consume() -> char {
        if (text_.empty()) throw InvalidJson{};
        const auto c = text_.front();
        text_.remove_prefix(1);
        return c;
    }
    auto u16() -> char32_t {
        char32_t value = 0;
        for (unsigned i = 0; i != 4; ++i) {
            const auto c = consume();
            if ('0' <= c && c <= '9') value = value * 16 + (c - '0');
            else if ('a' <= c && c <= 'f') value = value * 16 + (c - 'a' + 10);
            else if ('A' <= c && c <= 'F') value = value * 16 + (c - 'A' + 10);
            else throw InvalidJson{};
        }
        return value;
    }
    auto string() -> std::string {
        if (!accept('"')) throw InvalidJson{};
        std::string output;
        for (;;) {
            auto c = consume();
            if (static_cast<unsigned char>(c) < 0x20) throw InvalidJson{};
            if (c == '"') return output;
            if (c != '\\') { output += c; continue; }
            c = consume();
            switch (c) {
            case '"': case '\\': case '/': output += c; break;
            case 'b': output += '\b'; break;
            case 'f': output += '\f'; break;
            case 'n': output += '\n'; break;
            case 'r': output += '\r'; break;
            case 't': output += '\t'; break;
            case 'u': {
                auto cp = u16();
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    if (consume() != '\\' || consume() != 'u') throw InvalidJson{};
                    const auto low = u16();
                    if (low < 0xDC00 || low > 0xDFFF) throw InvalidJson{};
                    cp = 0x10000 + ((cp - 0xD800) << 10U) + low - 0xDC00;
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) throw InvalidJson{};
                output += ttytk::utf8::encode(cp);
                break;
            }
            default: throw InvalidJson{};
            }
        }
    }
    void number() {
        if (text_.starts_with('-')) consume();
        auto digits = [&] {
            auto n = text_.find_first_not_of("0123456789");
            if (text_.empty() || n == 0) throw InvalidJson{};
            text_.remove_prefix(n == std::string_view::npos ? text_.size() : n);
        };
        if (text_.starts_with('0')) consume();
        else digits();
        if (text_.starts_with('.')) { consume(); digits(); }
        if (text_.starts_with('e') || text_.starts_with('E')) {
            consume();
            if (text_.starts_with('+') || text_.starts_with('-')) consume();
            digits();
        }
    }
    auto parse_value(unsigned depth) -> Json {
        if (depth >= 64) throw InvalidJson{};
        Json result;
        if (accept('{')) {
            if (accept('}')) return result;
            do {
                auto key = string();
                if (!accept(':')) throw InvalidJson{};
                result.keys.push_back(std::move(key));
                result.fields.push_back(parse_value(depth + 1));
            } while (accept(','));
            if (!accept('}')) throw InvalidJson{};
        } else if (accept('[')) {
            if (accept(']')) return result;
            do { result.array.push_back(parse_value(depth + 1)); } while (accept(','));
            if (!accept(']')) throw InvalidJson{};
        } else if (text_.starts_with('"')) result.text = string();
        else if (text_.starts_with("true") || text_.starts_with("null")) text_.remove_prefix(4);
        else if (text_.starts_with("false")) text_.remove_prefix(5);
        else number();
        return result;
    }
    std::string_view text_;
};

inline void resolve(const Json& node, const Json& repository, const Scope& root, std::vector<Rule>& rules,
                    std::unordered_set<const Json*>& open, std::size_t& budget) {
    if (budget == 0 || open.size() >= 64 || !open.insert(&node).second) return;
    --budget;
    const auto& include = node.field("include").text;
    const auto& match = node.field("match").text;
    const auto& begin = node.field("begin").text;
    const auto& end = node.field("end").text;
    const auto& name = node.field("name").text;
    if (include.starts_with('#'))
        resolve(repository.field(std::string_view(include).substr(1)), repository, root, rules, open, budget);
    else if (!match.empty() || (!begin.empty() && !end.empty()))
        rules.push_back({match, begin, end, name.empty() ? root : Scope{name}});
    else for (const auto& nested : node.field("patterns").array)
        resolve(nested, repository, root, rules, open, budget);
    open.erase(&node);
}

}  // namespace detail

inline auto Scope::intern(std::string name) -> std::shared_ptr<const std::string> {
    if (name.empty()) return {};
    static std::mutex mutex;
    static std::unordered_map<std::string, std::weak_ptr<const std::string>> names;
    static std::size_t misses = 0;
    const std::lock_guard lock{mutex};
    if (const auto found = names.find(name); found != names.end())
        if (auto existing = found->second.lock()) return existing;
    if (++misses % 1024 == 0)
        std::erase_if(names, [](const auto& entry) { return entry.second.expired(); });
    auto shared = std::make_shared<const std::string>(std::move(name));
    names[*shared] = shared;
    return shared;
}

/** Load TextMate JSON (or native JSON with id, globs and keywords). */
[[nodiscard]] inline auto load_tm(const std::filesystem::path& path) -> ttytk::Result<Grammar> {
    std::ifstream input(path, std::ios::binary);
    if (!input) return ttytk::Error{1, "unable to open grammar"};
    std::string source{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    if (input.bad()) return ttytk::Error{2, "unable to read grammar"};
    detail::Json document;
    try {
        auto view = std::string_view(source);
        if (view.starts_with("\xEF\xBB\xBF")) view.remove_prefix(3);
        document = detail::JsonParser{view}.parse();
    } catch (const detail::InvalidJson&) {
        return ttytk::Error{2, "invalid grammar JSON"};
    }
    Grammar grammar;
    grammar.id = document.field("id").text;
    const auto& scope = document.field("scopeName").text;
    if (grammar.id.empty()) grammar.id = scope;
    if (grammar.id.empty()) return ttytk::Error{3, "grammar has no id or scopeName"};
    grammar.name = document.field("name").text;
    for (const auto& type : document.field("fileTypes").array) {
        if (type.text.empty()) continue;
        grammar.filename_globs.push_back(type.text);
        if (type.text.find_first_of("*?") == std::string::npos)
            grammar.filename_globs.push_back("*." + type.text);
    }
    for (const auto& type : document.field("globs").array)
        if (!type.text.empty()) grammar.filename_globs.push_back(type.text);
    for (const auto& keyword : document.field("keywords").array)
        if (!keyword.text.empty()) grammar.keywords.push_back(keyword.text);
    grammar.root_scope = Scope{scope.empty() ? grammar.id : scope};
    grammar.first_line_match = document.field("firstLineMatch").text;
    std::unordered_set<const detail::Json*> open;
    std::size_t budget = 8192;
    detail::resolve(document, document.field("repository"), grammar.root_scope, grammar.rules, open, budget);
    return grammar;
}

class Registry {
public:
    [[nodiscard]] static auto global() -> Registry& {
        static Registry registry;
        return registry;
    }

    void add(Grammar grammar) { grammars_.push_back(std::move(grammar)); }

    void load_dir(const std::filesystem::path& directory) {
        auto path = directory;
        if (directory.string().starts_with("~/"))
            if (const auto* home = std::getenv("HOME")) path = std::filesystem::path(home) / directory.string().substr(2);
        std::error_code error;
        std::vector<std::filesystem::path> paths;
        for (std::filesystem::directory_iterator it(path, error), end; !error && it != end; it.increment(error)) {
            const auto& entry = *it;
            std::error_code type_error;
            if (!entry.is_regular_file(type_error)) continue;
            const auto extension = entry.path().extension();
            if (extension != ".json" && extension != ".tmLanguage") continue;
            paths.push_back(entry.path());
        }
        std::sort(paths.begin(), paths.end());
        for (const auto& file : paths)
            if (auto grammar = load_tm(file)) add(std::move(grammar).value());
    }

    [[nodiscard]] auto by_filename(const std::string_view filename) const -> const Grammar* {
        for (const auto& grammar : grammars_) {
            if (grammar.matches_filename(filename)) return &grammar;
        }
        return nullptr;
    }

    [[nodiscard]] auto by_id(const std::string_view id) const -> const Grammar* {
        for (const auto& grammar : grammars_)
            if (grammar.id == id || grammar.root_scope.name() == id) return &grammar;
        for (const auto& grammar : grammars_)
            if ((grammar.id.starts_with("source.") && std::string_view(grammar.id).substr(7) == id) ||
                (grammar.root_scope.name().starts_with("source.") && grammar.root_scope.name().substr(7) == id))
                return &grammar;
        return nullptr;
    }

    [[nodiscard]] auto by_first_line(const std::string_view first_line) const -> const Grammar* {
        if (first_line.empty()) return nullptr;
        const search::detail::Input input{first_line, 0};
        for (const auto& grammar : grammars_) {
            if (grammar.first_line_match.empty()) continue;
            const search::detail::Regex regex{grammar.first_line_match, true};
            search::detail::Regex::Failures failures;
            for (std::size_t at = 0; at <= input.values.size(); ++at)
                if (input.breaks[at] && regex.run(input, at, &failures)) return &grammar;
        }
        return nullptr;
    }

private:
    std::deque<Grammar> grammars_;
};

[[nodiscard]] inline auto detect(const Registry& registry, const std::string_view filename,
                                 const std::string_view first_line = {}) -> const Grammar* {
    // The editor directive wins over both the filename and the interpreter.
    if (const auto opening = first_line.find("-*-"); opening != std::string_view::npos) {
        if (const auto closing = first_line.find("-*-", opening + 3); closing != std::string_view::npos) {
            auto mode = first_line.substr(opening + 3, closing - opening - 3);
            if (const auto assignment = mode.find("mode:"); assignment != std::string_view::npos)
                mode.remove_prefix(assignment + 5);
            const auto begin = mode.find_first_not_of(" \t");
            if (begin != std::string_view::npos) {
                mode.remove_prefix(begin);
                if (const auto* grammar = registry.by_id(mode.substr(0, mode.find_first_of(" \t;")))) return grammar;
            }
        }
    }
    if (const auto* grammar = registry.by_filename(filename)) return grammar;
    if (const auto* grammar = registry.by_first_line(first_line)) return grammar;
    if (first_line.starts_with("#!")) {
        auto command = first_line.substr(2);
        bool env = false;
        for (auto begin = command.find_first_not_of(" \t"); begin != std::string_view::npos;
             begin = command.find_first_not_of(" \t")) {
            command.remove_prefix(begin);
            const auto end = command.find_first_of(" \t\r");
            auto token = command.substr(0, end);
            if (const auto slash = token.find_last_of('/'); slash != std::string_view::npos) token.remove_prefix(slash + 1);
            if (token == "env") env = true;
            else if (!env || (!token.starts_with('-') && token.find('=') == std::string_view::npos)) {
                if (const auto* grammar = registry.by_id(token)) return grammar;
                const auto digit = token.find_first_of("0123456789");
                if (digit != std::string_view::npos)
                    if (const auto* grammar = registry.by_id(token.substr(0, digit))) return grammar;
                break;
            }
            if (end == std::string_view::npos || command[end] == '\r') break;
            command.remove_prefix(end + 1);
        }
    }
    return nullptr;
}

}  // namespace ttyutils::syntax
