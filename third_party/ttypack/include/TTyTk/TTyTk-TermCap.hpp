#pragma once

#include <TTyTk/TTyTk-Core.hpp>
#include <TTyTk/TTyTk-TermInfo.hpp>

#include <charconv>
#include <cstdlib>
#include <fstream>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace ttytk::termcap {

/** A resolved termcap description. Lookups accept termcap or terminfo names. */
struct Entry {
    std::string names{};
    std::unordered_map<std::string, bool> flags{};
    std::unordered_map<std::string, int> numbers{};
    std::unordered_map<std::string, std::string> strings{};

    [[nodiscard]] auto flag(std::string_view name) const -> bool;
    [[nodiscard]] auto num(std::string_view name) const -> std::optional<int>;
    [[nodiscard]] auto str(std::string_view name) const -> std::optional<std::string_view>;
};

namespace detail {
inline auto translate(std::string_view from, std::string_view to,
                      const std::string_view name) -> std::string_view {
    while (!from.empty() && !to.empty()) {
        const auto in = terminfo::detail::next_name(from);
        const auto out = terminfo::detail::next_name(to);
        if (in == name) return out;
    }
    return name;
}

inline auto trim(std::string_view field) -> std::string_view {
    const auto start = field.find_first_not_of(" \t\r");
    if (start == std::string_view::npos) return {};
    return field.substr(start, field.find_last_not_of(" \t\r") - start + 1);
}

inline auto unescape(const std::string_view input) -> std::string {
    std::string out;
    for (std::size_t i = 0; i < input.size();) {
        const char c = input[i++];
        if (c == '^' && i < input.size()) {
            const auto next = static_cast<unsigned char>(input[i++]);
            out += next == '?' ? '\x7f' : static_cast<char>(next & 31U);
        } else if (c == '\\' && i < input.size()) {
            const char next = input[i++];
            switch (next) {
                case 'E': case 'e': out += '\x1b'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 's': out += ' '; break;
                default:
                    if (next >= '0' && next <= '7') {
                        unsigned value = static_cast<unsigned>(next - '0');
                        for (unsigned j = 1; j < 3 && i < input.size() &&
                             input[i] >= '0' && input[i] <= '7'; ++j)
                            value = value * 8 + static_cast<unsigned>(input[i++] - '0');
                        out += static_cast<char>(value & 255U);
                    } else out += next;
            }
        } else out += c;
    }
    return out;
}

inline auto logical_lines(std::string_view text) -> std::vector<std::string> {
    std::vector<std::string> lines;
    std::string pending;
    while (!text.empty()) {
        const auto end = text.find('\n');
        auto line = text.substr(0, end);
        text.remove_prefix(end == std::string_view::npos ? text.size() : end + 1);
        if (line.ends_with('\r')) line.remove_suffix(1);
        const auto start = line.find_first_not_of(" \t");
        if (start == std::string_view::npos || line[start] == '#') continue;
        if (!pending.empty()) line.remove_prefix(start);
        std::size_t escapes = 0;
        while (escapes < line.size() && line[line.size() - escapes - 1] == '\\') ++escapes;
        if ((escapes & 1U) != 0U && end != std::string_view::npos) {
            pending += line.substr(0, line.size() - 1);
        } else {
            pending += line;
            lines.push_back(std::exchange(pending, {}));
        }
    }
    if (!pending.empty()) lines.push_back(std::move(pending));
    return lines;
}

inline auto field(std::string_view& line) -> std::string_view {
    std::size_t end = 0;
    for (; end < line.size(); ++end) {
        if (line[end] == ':') break;
        if (line[end] == '\\' && end + 1 < line.size()) ++end;
    }
    const auto value = line.substr(0, end);
    line.remove_prefix(end + (end != line.size() ? 1 : 0));
    return value;
}

inline auto matches(std::string_view names, const std::string_view name) -> bool {
    while (!names.empty()) {
        const auto end = names.find('|');
        const auto alias = names.substr(0, end);
        if (alias == name) return true;
        names.remove_prefix(end == std::string_view::npos ? names.size() : end + 1);
    }
    return false;
}

struct Parsed {
    Entry entry;
    std::unordered_set<std::string> seen;
    std::string parent;
};

inline auto parse(std::string_view line) -> Result<Parsed> {
    if (line.find(':') == std::string_view::npos) return Error{EINVAL, "invalid termcap entry"};
    Parsed out;
    out.entry.names = trim(field(line));
    if (out.entry.names.empty()) return Error{EINVAL, "empty termcap names"};
    while (!line.empty()) {
        auto cap = field(line);
        const auto start = cap.find_first_not_of(" \t");
        if (start == std::string_view::npos) continue;
        cap.remove_prefix(start);
        if (cap.size() < 2) return Error{EINVAL, "invalid termcap capability"};
        const std::string key(cap.substr(0, 2));
        if (!out.seen.insert(key).second) continue;
        if (cap.size() == 2) out.entry.flags[key] = true;
        else if (cap[2] == '@') continue;
        else if (cap[2] == '#') {
            auto value = trim(cap.substr(3));
            if (value.empty()) return Error{EINVAL, "missing termcap number"};
            const int base = value.size() > 1 && value.front() == '0' ? 8 : 10;
            int n = 0;
            const auto result = std::from_chars(value.data(), value.data() + value.size(), n, base);
            if (result.ec != std::errc{} || result.ptr != value.data() + value.size())
                return Error{EINVAL, "invalid termcap number"};
            out.entry.numbers[key] = n;
        } else if (cap[2] == '=') {
            if (key == "tc") out.parent = trim(cap.substr(3));
            else out.entry.strings[key] = unescape(cap.substr(3));
        } else return Error{EINVAL, "invalid termcap capability type"};
    }
    return out;
}

inline auto resolve(const std::vector<std::string>& lines, const std::size_t index) -> Result<Entry> {
    auto first = parse(lines[index]);
    if (!first) return first.error();
    auto parsed = std::move(first).value();
    std::unordered_set<std::size_t> visited{index};
    while (!parsed.parent.empty()) {
        std::size_t parent = 0;
        while (parent < lines.size() && !matches(std::string_view{lines[parent]}.substr(0, lines[parent].find(':')),
                                                  parsed.parent)) ++parent;
        if (parent == lines.size()) return Error{ENOENT, "missing termcap parent"};
        if (!visited.insert(parent).second) return Error{ELOOP, "termcap inheritance loop"};
        auto next = parse(lines[parent]);
        if (!next) return next.error();
        auto inherited = std::move(next).value();
        for (const auto& [key, value] : inherited.entry.flags)
            if (!parsed.seen.contains(key)) parsed.entry.flags.emplace(key, value);
        for (const auto& [key, value] : inherited.entry.numbers)
            if (!parsed.seen.contains(key)) parsed.entry.numbers.emplace(key, value);
        for (auto& [key, value] : inherited.entry.strings)
            if (!parsed.seen.contains(key)) parsed.entry.strings.emplace(key, std::move(value));
        parsed.seen.merge(inherited.seen);
        parsed.parent = std::move(inherited.parent);
    }
    return std::move(parsed.entry);
}
}  // namespace detail

inline auto Entry::flag(const std::string_view name) const -> bool {
    const auto code = detail::translate(terminfo::detail::bool_names, terminfo::detail::bool_codes, name);
    const auto it = flags.find(std::string{code});
    return it != flags.end() && it->second;
}
inline auto Entry::num(const std::string_view name) const -> std::optional<int> {
    const auto code = detail::translate(terminfo::detail::num_names, terminfo::detail::num_codes, name);
    const auto it = numbers.find(std::string{code});
    return it == numbers.end() ? std::nullopt : std::optional{it->second};
}
inline auto Entry::str(const std::string_view name) const -> std::optional<std::string_view> {
    const auto code = detail::translate(terminfo::detail::str_names, terminfo::detail::str_codes, name);
    const auto it = strings.find(std::string{code});
    return it == strings.end() ? std::nullopt : std::optional<std::string_view>{it->second};
}

/** Loads the first logical entry and resolves tc= within the supplied text. */
[[nodiscard]] inline auto load_from(const std::string_view text) -> Result<Entry> {
    const auto lines = detail::logical_lines(text);
    return lines.empty() ? Result<Entry>{Error{EINVAL, "empty termcap database"}} : detail::resolve(lines, 0);
}

/** Looks up an alias in TERMCAP or the system termcap file. */
[[nodiscard]] inline auto load(const std::string& name) -> Result<Entry> {
    if (name.empty()) return Error{EINVAL, "empty terminal name"};
    const char* env = std::getenv("TERMCAP");
    const bool path_override = env && env[0] == '/';
    auto lines = env && !path_override ? detail::logical_lines(env) : std::vector<std::string>{};
    if (!lines.empty()) {
        const char* term = std::getenv("TERM");
        if (term && name != term) lines.clear();
    }
    std::ifstream file(path_override ? env : "/etc/termcap");
    if (file) {
        const std::string database(std::istreambuf_iterator<char>{file}, {});
        auto disk = detail::logical_lines(database);
        for (auto& line : disk) lines.push_back(std::move(line));
    }
    for (std::size_t i = 0; i < lines.size(); ++i)
        if (detail::matches(std::string_view{lines[i]}.substr(0, lines[i].find(':')), name))
            return detail::resolve(lines, i);
    return Error{ENOENT, "termcap entry not found"};
}

/** Expands legacy cursor addressing (row then column unless %r swaps them).
 *  Leading padding specifications are preserved for the caller's output policy.
 */
[[nodiscard]] inline auto tgoto(const std::string_view pattern, const int column, const int row) -> std::string {
    std::int64_t values[]{row, column};
    std::size_t index = 0;
    std::string out;
    for (std::size_t i = 0; i < pattern.size();) {
        const char c = pattern[i++];
        if (c != '%' || i == pattern.size()) {
            out += c;
            continue;
        }
        const char op = pattern[i++];
        auto& value = values[index % 2];
        switch (op) {
            case 'p': return terminfo::tparm(pattern, row, column);
            case 'r': std::swap(values[0], values[1]); break;
            case 'i': ++values[0]; ++values[1]; break;
            case 'n': values[0] ^= 0140; values[1] ^= 0140; break;
            case 'm': values[0] ^= 0177; values[1] ^= 0177; break;
            case '%': out += '%'; break;
            case 'B': value = (value / 10) * 16 + value % 10; break;
            case 'D': value -= 2 * (value % 16); break;
            case '>':
                if (pattern.size() - i >= 2) {
                    if (value > static_cast<unsigned char>(pattern[i]))
                        value += static_cast<unsigned char>(pattern[i + 1]);
                    i += 2;
                }
                break;
            case '+':
                if (i == pattern.size()) break;
                value += static_cast<unsigned char>(pattern[i++]);
                [[fallthrough]];
            case '.': out += static_cast<char>(value); ++index; break;
            case 'd': case '2': case '3': {
                auto number = std::to_string(value);
                if (op != 'd' && number.size() < static_cast<std::size_t>(op - '0'))
                    number.insert(value < 0 ? 1 : 0, static_cast<std::size_t>(op - '0') - number.size(), '0');
                out += number;
                ++index;
                break;
            }
            default: break;
        }
    }
    return out;
}

/** Converts available standard capabilities to their corresponding termcap codes. */
[[nodiscard]] inline auto from_terminfo(const terminfo::Entry& entry) -> Entry {
    Entry converted;
    converted.names = entry.names;
    const auto map = [](std::string_view codes, const std::size_t size, auto put) {
        for (std::size_t i = 0; i < size && !codes.empty(); ++i)
            put(std::string{terminfo::detail::next_name(codes)}, i);
    };
    map(terminfo::detail::bool_codes, entry.flags.size(), [&](auto key, auto i) {
        if (entry.flag(i)) converted.flags.emplace(std::move(key), true);
    });
    map(terminfo::detail::num_codes, entry.numbers.size(), [&](auto key, auto i) {
        if (auto value = entry.num(i)) converted.numbers.emplace(std::move(key), *value);
    });
    map(terminfo::detail::str_codes, entry.strings.size(), [&](auto key, auto i) {
        if (auto value = entry.str(i)) converted.strings.emplace(std::move(key), *value);
    });
    return converted;
}
}  // namespace ttytk::termcap
