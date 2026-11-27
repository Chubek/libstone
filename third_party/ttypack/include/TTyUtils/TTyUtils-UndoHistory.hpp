#pragma once

#include <TTyUtils/TTyUtils-TextBuffer.hpp>
#include <TTyUtils/TTyUtils-TextCursor.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ttyutils::undo {

struct TreeNode {
    std::string before{};
    std::string after{};
    std::size_t parent{};
    std::vector<std::size_t> children{};
    std::string label{};
    std::vector<textcur::Cursor> cursors_before{};
    std::vector<textcur::Cursor> cursors_after{};
};

struct Checkpoint {
    std::string name{};
    std::size_t node{};
};

class History;

class Transaction {
public:
    explicit Transaction(History& history);
    Transaction(const Transaction&) = delete;
    auto operator=(const Transaction&) -> Transaction& = delete;
    Transaction(Transaction&& other) noexcept : history_(std::exchange(other.history_, nullptr)) {}
    auto operator=(Transaction&& other) noexcept -> Transaction& {
        if (this != &other) {
            finish();
            history_ = std::exchange(other.history_, nullptr);
        }
        return *this;
    }
    ~Transaction() { finish(); }

    void commit();

private:
    void finish() noexcept {
        try { commit(); } catch (...) {}
    }
    History* history_{};
};

class History {
public:
    explicit History(textbuf::Buffer& buffer) : buffer_(&buffer) {
        nodes_.push_back(TreeNode{.after = std::string(buffer.text())});
    }
    History(textbuf::Buffer& buffer, textcur::CursorSet& cursors) : History(buffer) {
        cursors_ = &cursors;
        nodes_.front().cursors_after = cursors.cursors();
    }

    [[nodiscard]] auto transaction() -> Transaction { return Transaction{*this}; }
    /** Group nearby, consecutive single-cursor insertions made by typing transactions. */
    [[nodiscard]] auto typing_transaction() -> Transaction {
        auto result = transaction();
        if (depth_ == 1) typing_ = true;
        return result;
    }
    [[nodiscard]] auto can_undo() const noexcept -> bool { return current_ != 0; }
    [[nodiscard]] auto can_redo() const noexcept { return !nodes_[current_].children.empty(); }
    [[nodiscard]] auto nodes() const noexcept -> const std::vector<TreeNode>& { return nodes_; }

    void begin() {
        if (depth_ == 0) {
            before_ = std::string(buffer_->text());
            before_cursors_ = cursors_ ? cursors_->cursors() : std::vector<textcur::Cursor>{};
            typing_ = false;
        }
        ++depth_;
    }

    void commit() {
        if (depth_ == 0 || --depth_ != 0) return;
        auto after = std::string(buffer_->text());
        if (after == before_) return;
        auto after_cursors = cursors_ ? cursors_->cursors() : std::vector<textcur::Cursor>{};
        const auto now = Clock::now();
        const auto prefix = static_cast<std::size_t>(std::mismatch(before_.begin(), before_.end(),
                                                                  after.begin(), after.end()).first - before_.begin());
        // With identical neighboring characters the longest common prefix
        // may overshoot the insertion; an attached cursor disambiguates it.
        const auto start = before_cursors_.size() == 1
            ? textbuf::LineIndex{before_}.offset(before_cursors_[0].position(), before_) : prefix;
        const auto inserted = after.size() > before_.size() ? after.size() - before_.size() : 0;
        const auto typed = typing_ && before_cursors_.size() <= 1 && inserted > 0 && start <= prefix &&
            std::string_view(after).substr(start + inserted) == std::string_view(before_).substr(start) &&
            after.find('\n', start) >= start + inserted;
        if (typed && group_active_ && now - last_typing_ <= std::chrono::milliseconds{500} &&
            start == last_end_ && nodes_[current_].children.empty() && nodes_[current_].after == before_) {
            nodes_[current_].after = std::move(after);
            nodes_[current_].cursors_after = std::move(after_cursors);
            last_typing_ = now;
            last_end_ = start + inserted;
            return;
        }
        const auto index = nodes_.size();
        nodes_[current_].children.reserve(nodes_[current_].children.size() + 1);
        nodes_.push_back(TreeNode{.before = std::move(before_), .after = std::move(after), .parent = current_,
            .cursors_before = std::move(before_cursors_), .cursors_after = std::move(after_cursors)});
        nodes_[current_].children.push_back(index);
        current_ = index;
        group_active_ = typed;
        last_typing_ = now;
        last_end_ = start + inserted;
    }

    auto undo() -> bool {
        if (depth_ != 0 || !can_undo()) return false;
        set_text(nodes_[current_].before, nodes_[current_].cursors_before);
        current_ = nodes_[current_].parent;
        group_active_ = false;
        return true;
    }

    auto redo_branch(const std::size_t branch = 0) -> bool {
        if (depth_ != 0 || branch >= nodes_[current_].children.size()) return false;
        const auto next = nodes_[current_].children[branch];
        set_text(nodes_[next].after, nodes_[next].cursors_after);
        current_ = next;
        group_active_ = false;
        return true;
    }

    void checkpoint(std::string name) {
        group_active_ = false;
        checkpoints_.push_back({std::move(name), current_});
    }
    [[nodiscard]] auto checkpoints() const noexcept -> const std::vector<Checkpoint>& { return checkpoints_; }

    /** Save branch order, the current revision, and cursor selections. */
    auto save(const std::filesystem::path& path) const -> bool {
        if (depth_ != 0) return false;
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << "TTyUtils-undo-1\n" << nodes_.size() << ' ' << current_ << ' ' << checkpoints_.size() << '\n';
        const auto write_cursors = [&](const auto& cursors) {
            output << cursors.size() << '\n';
            for (const auto& cursor : cursors) {
                const auto [anchor, active] = cursor.selection();
                output << anchor.line << ' ' << anchor.col << ' ' << active.line << ' ' << active.col << '\n';
            }
        };
        for (const auto& node : nodes_) {
            output << node.parent << ' ' << std::quoted(node.before) << ' ' << std::quoted(node.after)
                   << ' ' << std::quoted(node.label) << '\n';
            write_cursors(node.cursors_before);
            write_cursors(node.cursors_after);
        }
        for (const auto& point : checkpoints_) output << point.node << ' ' << std::quoted(point.name) << '\n';
        output.close();
        return static_cast<bool>(output);
    }

    /** Load history only if its current revision matches the open document. */
    auto load(const std::filesystem::path& path) -> bool {
        if (depth_ != 0) return false;
        std::ifstream input(path, std::ios::binary);
        std::string marker;
        std::size_t count = 0, current = 0, marks = 0;
        if (!(input >> marker >> count >> current >> marks) || marker != "TTyUtils-undo-1" ||
            count == 0 || current >= count) return false;
        const auto quoted = [&](std::string& value) -> bool {
            input >> std::ws;
            return input.peek() == '"' && static_cast<bool>(input >> std::quoted(value));
        };
        std::vector<TreeNode> nodes;
        std::vector<Checkpoint> checkpoints;
        const auto read_cursors = [&](auto& cursors) {
            std::size_t length = 0;
            if (!(input >> length)) return false;
            for (; length != 0; --length) {
                textcur::Selection selection;
                if (!(input >> selection.anchor.line >> selection.anchor.col >>
                      selection.active.line >> selection.active.col)) return false;
                cursors.push_back(textcur::Cursor::from_selection(selection));
            }
            return true;
        };
        for (std::size_t i = 0; i < count; ++i) {
            TreeNode node;
            if (!(input >> node.parent) || (i == 0 ? node.parent != 0 : node.parent >= i) ||
                !quoted(node.before) || !quoted(node.after) || !quoted(node.label) ||
                !read_cursors(node.cursors_before) || !read_cursors(node.cursors_after)) return false;
            if (i != 0) nodes[node.parent].children.push_back(i);
            nodes.push_back(std::move(node));
        }
        for (std::size_t i = 0; i < marks; ++i) {
            Checkpoint point;
            if (!(input >> point.node) || point.node >= count || !quoted(point.name)) return false;
            checkpoints.push_back(std::move(point));
        }
        input >> std::ws;
        if (!input.eof() || input.bad() || nodes[current].after != buffer_->text()) return false;
        if (cursors_) cursors_->restore(nodes[current].cursors_after);
        nodes_ = std::move(nodes);
        checkpoints_ = std::move(checkpoints);
        current_ = current;
        group_active_ = false;
        return true;
    }

private:
    friend class Transaction;

    void set_text(const std::string_view value, const std::vector<textcur::Cursor>& cursors) {
        buffer_->replace_bytes(0, buffer_->size(), value);
        if (cursors_) cursors_->restore(cursors);
    }

    textbuf::Buffer* buffer_{};
    textcur::CursorSet* cursors_{};
    std::vector<TreeNode> nodes_;
    std::vector<Checkpoint> checkpoints_;
    std::size_t current_{};
    std::size_t depth_{};
    std::string before_{};
    std::vector<textcur::Cursor> before_cursors_{};
    using Clock = std::chrono::steady_clock;
    Clock::time_point last_typing_{};
    std::size_t last_end_{};
    bool typing_{};
    bool group_active_{};
};

inline Transaction::Transaction(History& history) : history_(&history) { history_->begin(); }
inline void Transaction::commit() {
    if (auto* history = std::exchange(history_, nullptr)) history->commit();
}

}  // namespace ttyutils::undo
