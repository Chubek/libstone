#pragma once

#include <TTyUtils/TTyUtils-Config.hpp>
#include <TTyUtils/TTyUtils-Dialog.hpp>
#include <TTyUtils/TTyUtils-ListView.hpp>
#include <TTyUtils/TTyUtils-TextInput.hpp>
#include <TTyUtils/TTyUtils-TreeView.hpp>

#include <filesystem>
#include <algorithm>
#include <functional>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ttyutils::ui {

enum class PickMode { Open, Save, Directory };

struct FileFilter {
    std::vector<std::string> globs{};
};

struct Bookmark {
    std::string name{};
    std::filesystem::path path{};
};

class FilePicker : public Dialog {
public:
    FilePicker() : Dialog("File picker") {}
    FilePicker& mode(const PickMode mode) noexcept { mode_ = mode; return *this; }
    FilePicker& filter(FileFilter filter) { filter_ = std::move(filter); return *this; }
    FilePicker& filter(std::initializer_list<std::string> globs) {
        return filter(FileFilter{std::vector<std::string>(globs)});
    }
    void set_directory(std::filesystem::path directory) {
        directory_ = std::move(directory);
        selection_.clear();
        set_body(directory_.string());
    }
    [[nodiscard]] auto directory() const noexcept -> const std::filesystem::path& { return directory_; }
    void set_hidden(const bool visible) noexcept { hidden_ = visible; }
    void set_selection(std::filesystem::path path) { selection_ = std::move(path); }
    void set_overwrite_confirm(std::function<bool(const std::filesystem::path&)> confirm) {
        overwrite_ = std::move(confirm);
    }
    void add_bookmark(Bookmark bookmark) { bookmarks_.push_back(std::move(bookmark)); }
    [[nodiscard]] auto bookmarks() const noexcept -> const std::vector<Bookmark>& { return bookmarks_; }
    auto goto_bookmark(const std::size_t index) -> bool {
        if (index >= bookmarks_.size()) return false;
        std::error_code error;
        if (!std::filesystem::is_directory(bookmarks_[index].path, error)) return false;
        set_directory(bookmarks_[index].path);
        return true;
    }

    /** List navigable directories and eligible filenames; filesystem failures are reported in error. */
    [[nodiscard]] auto entries(std::error_code& error) const -> std::vector<std::filesystem::path> {
        std::vector<std::filesystem::path> result;
        const std::filesystem::directory_iterator end;
        for (std::filesystem::directory_iterator it(directory_, error); !error && it != end; it.increment(error)) {
            const auto name = it->path().filename().string();
            if (!hidden_ && name.starts_with('.')) continue;
            std::error_code status_error;
            const auto status = it->status(status_error);
            if (status_error) continue;
            if (std::filesystem::is_directory(status) ||
                (mode_ != PickMode::Directory && std::filesystem::is_regular_file(status) && matches(name)))
                result.push_back(it->path());
        }
        std::sort(result.begin(), result.end());
        return result;
    }

    /** Validate the explicitly selected path. In Save mode existing files require approval. */
    [[nodiscard]] auto show() const -> std::optional<std::filesystem::path> {
        if (result() == DialogResult::Cancel || (selection_.empty() && mode_ != PickMode::Directory))
            return std::nullopt;
        auto candidate = selection_.empty() ? directory_ : directory_ / selection_;
        std::error_code error;
        if (mode_ == PickMode::Directory)
            return std::filesystem::is_directory(candidate, error) ? std::optional{candidate} : std::nullopt;
        if (!matches(candidate.filename().string())) return std::nullopt;
        if (mode_ == PickMode::Open)
            return std::filesystem::is_regular_file(candidate, error) ? std::optional{candidate} : std::nullopt;
        const auto parent = candidate.has_parent_path() ? candidate.parent_path() : std::filesystem::path{"."};
        if (!std::filesystem::is_directory(parent, error)) return std::nullopt;
        const auto status = std::filesystem::symlink_status(candidate, error);
        if (status.type() == std::filesystem::file_type::not_found &&
            (!error || error == std::errc::no_such_file_or_directory)) return candidate;
        if (error || !std::filesystem::is_regular_file(candidate, error)) return std::nullopt;
        const auto confirm = overwrite_;
        return confirm && confirm(candidate) ? std::optional{candidate} : std::nullopt;
    }

private:
    [[nodiscard]] static auto glob(const std::string_view pattern, const std::string_view name) -> bool {
        auto p = std::size_t{0};
        auto n = std::size_t{0};
        auto star = std::string_view::npos;
        auto retry = std::size_t{0};
        while (n < name.size()) {
            if (p < pattern.size() && pattern[p] == '*') { star = p++; retry = n; }
            else if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == name[n])) { ++p; ++n; }
            else if (star != std::string_view::npos) { p = star + 1; n = ++retry; }
            else return false;
        }
        while (p < pattern.size() && pattern[p] == '*') ++p;
        return p == pattern.size();
    }
    [[nodiscard]] auto matches(const std::string_view filename) const -> bool {
        return filter_.globs.empty() || std::any_of(filter_.globs.begin(), filter_.globs.end(),
            [filename](const auto& pattern) { return glob(pattern, filename); });
    }
    PickMode mode_{PickMode::Open};
    FileFilter filter_{};
    std::filesystem::path directory_{"."};
    std::filesystem::path selection_;
    std::vector<Bookmark> bookmarks_;
    std::function<bool(const std::filesystem::path&)> overwrite_;
    bool hidden_{};
};

}  // namespace ttyutils::ui
