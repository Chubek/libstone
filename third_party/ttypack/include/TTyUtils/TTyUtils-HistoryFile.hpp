#pragma once

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace ttyutils::hist {

struct Entry {
    std::string text{};
    std::string timestamp{};
    std::string cwd{};
    int status{};
};

enum class Merge { all, adjacent, none };

[[nodiscard]] inline auto dedup(std::vector<Entry> entries, const Merge policy = Merge::adjacent) -> std::vector<Entry> {
    if (policy == Merge::none) return entries;
    std::vector<Entry> result;
    for (auto& entry : entries) {
        const auto duplicate = policy == Merge::adjacent
                                   ? (!result.empty() && result.back().text == entry.text)
                                   : std::any_of(result.begin(), result.end(), [&entry](const Entry& prior) {
                                         return prior.text == entry.text;
                                     });
        if (!duplicate) result.push_back(std::move(entry));
    }
    return result;
}

class History {
public:
    explicit History(std::filesystem::path path, const Merge merge = Merge::adjacent)
        : path_(std::move(path)), merge_(merge) {
        if (path_.string().starts_with("~/")) {
            if (const auto* home = std::getenv("HOME"))
                path_ = std::filesystem::path(home) / path_.string().substr(2);
        }
    }

    [[nodiscard]] auto entries() const noexcept -> const std::vector<Entry>& { return entries_; }
    /// Suppress new entries containing this literal pattern.
    void ignore(std::string pattern) { if (!pattern.empty()) ignores_.push_back(std::move(pattern)); }

    /// Reload complete records without locking out concurrent appends.
    [[nodiscard]] auto load() -> bool {
        std::ifstream input(path_, std::ios::binary);
        if (!input) {
            std::error_code error;
            if (std::filesystem::exists(path_, error) || error) return false;
            entries_.clear();
            return true;
        }
        std::vector<Entry> entries;
        std::string line;
        while (std::getline(input, line)) {
            if (input.eof()) break;  // A concurrent writer may still be writing this record.
            Entry entry;
            const bool encoded = line.starts_with("\tTTyHistory1\t");
            const auto fields = std::string_view(line).substr(encoded ? 13 : 0);
            const auto one = fields.find('\t');
            const auto two = one == std::string::npos ? std::string::npos : fields.find('\t', one + 1);
            const auto three = two == std::string::npos ? std::string::npos : fields.find('\t', two + 1);
            if (three == std::string::npos) {
                if (encoded) return false;
                entry.text = line;
            } else {
                const auto status = fields.substr(two + 1, three - two - 1);
                const auto parsed = std::from_chars(status.data(), status.data() + status.size(), entry.status);
                if (encoded) {
                    if (parsed.ec != std::errc{} || parsed.ptr != status.data() + status.size() ||
                        !unhex(fields.substr(0, one), entry.timestamp) ||
                        !unhex(fields.substr(one + 1, two - one - 1), entry.cwd) ||
                        !unhex(fields.substr(three + 1), entry.text)) return false;
                } else {
                    entry.timestamp = fields.substr(0, one);
                    entry.cwd = fields.substr(one + 1, two - one - 1);
                    entry.text = fields.substr(three + 1);
                }
            }
            entries.push_back(std::move(entry));
        }
        if (input.bad() || !input.eof()) return false;
        entries_ = dedup(std::move(entries), merge_);
        return true;
    }

    /// Append one complete record; POSIX writers serialize records across processes.
    [[nodiscard]] auto append(Entry entry) -> bool {
        if (entry.text.empty() || entry.text.starts_with(' ') ||
            std::any_of(ignores_.begin(), ignores_.end(), [&](const auto& pattern) {
                return entry.text.find(pattern) != std::string::npos;
            })) return true;
        if (entry.timestamp.empty()) {
            entry.timestamp = std::to_string(std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        }
        const auto record = "\tTTyHistory1\t" + hex(entry.timestamp) + '\t' + hex(entry.cwd) +
                            '\t' + std::to_string(entry.status) + '\t' + hex(entry.text) + '\n';
#if defined(__unix__) || defined(__APPLE__)
        const int fd = ::open(path_.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
        if (fd < 0) return false;
        int locked;
        do { locked = ::flock(fd, LOCK_EX); } while (locked < 0 && errno == EINTR);
        bool ok = locked == 0;
        for (std::size_t at = 0; ok && at < record.size();) {
            const auto size = std::min(record.size() - at,
                                       static_cast<std::size_t>((std::numeric_limits<ssize_t>::max)()));
            const auto written = ::write(fd, record.data() + at, size);
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0) ok = false;
            else at += static_cast<std::size_t>(written);
        }
        if (::close(fd) < 0 || !ok) return false;
#else
        std::ofstream output(path_, std::ios::binary | std::ios::app);
        if (!output) return false;
        output << record;
        output.close();
        if (!output) return false;
#endif
        if (merge_ == Merge::none ||
            (merge_ == Merge::adjacent ? entries_.empty() || entries_.back().text != entry.text
                                       : std::none_of(entries_.begin(), entries_.end(), [&](const Entry& prior) {
                                             return prior.text == entry.text;
                                         }))) entries_.push_back(std::move(entry));
        return true;
    }

    [[nodiscard]] auto match_prefix(const std::string_view prefix) const -> std::vector<Entry> {
        std::vector<Entry> result;
        for (const auto& entry : entries_) {
            if (entry.text.starts_with(prefix)) result.push_back(entry);
        }
        return result;
    }

private:
    static auto hex(const std::string_view value) -> std::string {
        constexpr std::string_view digits = "0123456789abcdef";
        std::string output;
        for (const unsigned char byte : value) {
            output += digits[byte >> 4U];
            output += digits[byte & 15U];
        }
        return output;
    }

    static auto unhex(const std::string_view value, std::string& output) -> bool {
        constexpr std::string_view digits = "0123456789abcdef";
        if (value.size() % 2) return false;
        for (std::size_t i = 0; i < value.size(); i += 2) {
            const auto hi = digits.find(value[i]);
            const auto lo = digits.find(value[i + 1]);
            if (hi == std::string_view::npos || lo == std::string_view::npos) return false;
            output += static_cast<char>((hi << 4U) | lo);
        }
        return true;
    }

    std::filesystem::path path_;
    Merge merge_;
    std::vector<Entry> entries_;
    std::vector<std::string> ignores_;
};

}  // namespace ttyutils::hist
