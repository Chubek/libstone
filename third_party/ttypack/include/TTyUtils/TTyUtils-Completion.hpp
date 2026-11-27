#pragma once

#include <TTyUtils/TTyUtils-Search.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <future>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

namespace ttyutils::completion {

struct Candidate {
    std::string text{};
    std::string display{};
    int score{};
};

[[nodiscard]] inline auto rank(const std::string_view query, const std::string_view candidate) -> int {
    if (candidate.starts_with(query))
        return 2000 - static_cast<int>(std::min(candidate.size() - query.size(), std::size_t{999}));
    int score = 0;
    std::size_t at = 0;
    for (const auto character : query) {
        const auto found = candidate.find(character, at);
        if (found == std::string_view::npos) return -1;
        score = std::min(1000, score + (found == at ? 10 : 1));
        at = found + 1;
    }
    return score;
}

class Source {
public:
    virtual ~Source() = default;
    [[nodiscard]] virtual auto complete(std::string_view query) -> std::vector<Candidate> = 0;
};

class Engine {
public:
    void add(std::unique_ptr<Source> source) {
        if (source) sources_.push_back(std::make_shared<Provider>(std::move(source)));
    }

    [[nodiscard]] auto query(const std::string_view text, const std::size_t page = 0,
                             const std::size_t page_size = 64) -> std::vector<Candidate> {
        return query_for(text, std::chrono::milliseconds{50}, page, page_size);
    }

    /// Collect results up to a shared deadline. A still-running provider is skipped
    /// by subsequent queries so repeated keystrokes cannot queue unbounded work.
    [[nodiscard]] auto query_for(const std::string_view text, const std::chrono::milliseconds timeout,
                                 const std::size_t page = 0, const std::size_t page_size = 64)
        -> std::vector<Candidate> {
        if (page_size == 0 || timeout <= std::chrono::milliseconds::zero()) return {};
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        std::vector<std::future<std::vector<Candidate>>> pending;
        pending.reserve(sources_.size());
        for (const auto& source : sources_) {
            if (std::chrono::steady_clock::now() >= deadline) break;
            if (source->busy.exchange(true)) continue;
            try {
                std::packaged_task<std::vector<Candidate>()> work{[source, query = std::string(text)] {
                    struct Reset { Provider& provider; ~Reset() { provider.busy = false; } } reset{*source};
                    return source->source->complete(query);
                }};
                auto future = work.get_future();
                std::thread(std::move(work)).detach();
                pending.push_back(std::move(future));
            } catch (...) {
                source->busy = false;
            }
        }
        std::vector<Candidate> result;
        for (auto& future : pending) {
            if (future.wait_until(deadline) != std::future_status::ready) continue;
            try {
                auto candidates = future.get();
                result.insert(result.end(), std::make_move_iterator(candidates.begin()),
                              std::make_move_iterator(candidates.end()));
            } catch (...) {} // One provider must not suppress results from other providers.
        }
        for (auto& candidate : result) {
            candidate.score = std::max(candidate.score, rank(text, candidate.text));
            if (candidate.display.empty()) candidate.display = candidate.text;
        }
        std::sort(result.begin(), result.end(), [](const Candidate& left, const Candidate& right) {
            if (left.score != right.score) return left.score > right.score;
            if (left.text != right.text) return left.text < right.text;
            return left.display < right.display;
        });
        std::unordered_set<std::string_view> seen;
        std::vector<Candidate> unique;
        unique.reserve(result.size());
        for (const auto& candidate : result)
            if (seen.insert(candidate.text).second) unique.push_back(candidate);
        if (page > unique.size() / page_size) return {};
        const auto begin = page * page_size;
        const auto end = begin + std::min(page_size, unique.size() - begin);
        return {unique.begin() + static_cast<std::ptrdiff_t>(begin),
                unique.begin() + static_cast<std::ptrdiff_t>(end)};
    }

    /// Longest common UTF-8 prefix; callers should supply the unpaged candidate set.
    [[nodiscard]] static auto common_prefix(const std::vector<Candidate>& candidates) -> std::string {
        if (candidates.empty()) return {};
        const auto& first = candidates.front().text;
        auto size = first.size();
        for (const auto& candidate : candidates)
            size = static_cast<std::size_t>(
                std::mismatch(first.begin(), first.begin() + static_cast<std::ptrdiff_t>(size),
                              candidate.text.begin(), candidate.text.end()).first - first.begin());
        while (size < first.size() && size != 0 && (static_cast<unsigned char>(first[size]) & 0xc0U) == 0x80U)
            --size;
        return first.substr(0, size);
    }

private:
    struct Provider {
        explicit Provider(std::unique_ptr<Source> value) : source(std::move(value)) {}
        std::unique_ptr<Source> source;
        std::atomic<bool> busy{};
    };
    std::vector<std::shared_ptr<Provider>> sources_;
};

class CallbackSource final : public Source {
public:
    using Callback = std::function<std::vector<Candidate>(std::string_view)>;
    /// The callback runs on a worker thread and may outlive its Engine after a timeout.
    /// Capture state by value or shared ownership, rather than borrowing local variables.
    explicit CallbackSource(Callback callback) : callback_(std::move(callback)) {}
    [[nodiscard]] auto complete(const std::string_view query) -> std::vector<Candidate> override {
        return callback_ ? callback_(query) : std::vector<Candidate>{};
    }

private:
    Callback callback_;
};

class FilenameSource final : public Source {
public:
    explicit FilenameSource(std::filesystem::path root = ".") : root_(std::move(root)) {}
    [[nodiscard]] auto complete(const std::string_view query) -> std::vector<Candidate> override {
        std::vector<Candidate> result;
        const std::filesystem::path prefix{query};
        const auto typed_parent = prefix.parent_path();
        const auto parent = prefix.is_absolute() ? typed_parent : root_ / typed_parent;
        const auto stem = prefix.filename().string();
        std::error_code error;
        for (auto it = std::filesystem::directory_iterator(parent, error);
             !error && it != std::filesystem::directory_iterator{}; it.increment(error)) {
            const auto& entry = *it;
            const auto name = entry.path().filename().string();
            if (!name.starts_with(stem)) continue;
            auto path = (typed_parent / name).string();
            std::error_code type_error;
            if (entry.is_directory(type_error)) path += std::filesystem::path::preferred_separator;
            result.push_back({std::move(path), name, 0});
        }
        return result;
    }

private:
    std::filesystem::path root_;
};

class WordSource final : public Source {
public:
    explicit WordSource(std::vector<std::string> words) : words_(std::move(words)) {}
    [[nodiscard]] auto complete(const std::string_view query) -> std::vector<Candidate> override {
        std::vector<Candidate> result;
        for (const auto& word : words_) if (rank(query, word) >= 0) result.push_back({word, word, 0});
        return result;
    }

private:
    std::vector<std::string> words_;
};

}  // namespace ttyutils::completion
