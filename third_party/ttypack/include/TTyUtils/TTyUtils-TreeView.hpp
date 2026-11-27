#pragma once

#include <TTyUtils/TTyUtils-ListView.hpp>

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace ttyutils::ui {

struct TreeNodeRef {
    std::string path{};
    std::string label{};
};

class TreeModel {
public:
    virtual ~TreeModel() = default;
    [[nodiscard]] virtual auto roots() const -> std::vector<TreeNodeRef> = 0;
    [[nodiscard]] virtual auto children(const std::string& path) const -> std::vector<TreeNodeRef> = 0;
};

enum class ExpandPolicy { Single, Multiple, All };

class TreeView : public Widget, public Scrollable {
public:
    explicit TreeView(std::shared_ptr<TreeModel> model = {}) : model_(std::move(model)) {}
    /** Replace data while retaining expansion and selection by their unique node paths. */
    void set_model(std::shared_ptr<TreeModel> model) { model_ = std::move(model); refresh(); }
    void refresh() { cached_.clear(); dirty_ = true; }
    void set_policy(const ExpandPolicy policy) noexcept {
        if (policy_ == policy) return;
        policy_ = policy;
        if (policy == ExpandPolicy::Single && expanded_.size() > 1) expanded_.clear();
        all_ = policy == ExpandPolicy::All;
        collapsed_.clear();
        dirty_ = true;
    }
    void expand(const std::string_view path) {
        if (policy_ == ExpandPolicy::Single && !all_) {
            // Preserve parents so the target remains in the visible projection.
            const auto keep = [&path](const std::string& node) {
                return path.starts_with(node) &&
                       (path.size() == node.size() || node.ends_with('/') || path[node.size()] == '/');
            };
            std::erase_if(expanded_, [&](const auto& node) { return !keep(node); });
        }
        collapsed_.erase(std::string(path));
        expanded_.insert(std::string(path));
        dirty_ = true;
    }
    void collapse(const std::string_view path) {
        if (all_) collapsed_.insert(std::string(path));
        expanded_.erase(std::string(path));
        dirty_ = true;
    }
    void expand_all() {
        all_ = true;
        collapsed_.clear();
        dirty_ = true;
    }
    void collapse_all() {
        all_ = false;
        expanded_.clear();
        collapsed_.clear();
        dirty_ = true;
    }
    [[nodiscard]] auto expanded(const std::string_view path) const -> bool {
        const std::string key{path};
        return all_ ? !collapsed_.contains(key) : expanded_.contains(key);
    }
    void on_choose(std::function<void(const TreeNodeRef&)> callback) { choose_ = std::move(callback); }
    /** Select a row in the flattened visible projection. */
    void select(const std::size_t row) {
        project();
        if (row >= rows_.size()) return;
        selected_ = rows_[row].node.path;
        const auto page = std::max(visible_length(), std::size_t{1});
        if (row < offset()) scroll_to(row);
        else if (row - offset() >= page) scroll_to(row - page + 1);
    }
    void choose() {
        project();
        if (rows_.empty()) return;
        const auto selected = selected_index();
        const auto callback = choose_;
        const auto node = rows_[selected].node;
        if (callback) callback(node);
    }
    [[nodiscard]] auto length() const -> std::size_t override { project(); return rows_.size(); }
    [[nodiscard]] auto visible_length() const -> std::size_t override {
        return static_cast<std::size_t>(std::max(0, rect().height));
    }
    [[nodiscard]] auto offset() const -> std::size_t override { return std::min(offset_, max_offset()); }
    void scroll_to(const std::size_t row) override { offset_ = std::min(row, max_offset()); }

    auto handle(const ttytk::input::Event& event) -> bool override {
        if (length() == 0) return false;
        if (const auto* mouse = std::get_if<ttytk::input::Mouse>(&event)) {
            int x{}, y{};
            if (mouse->button != 0 || !mouse->pressed || mouse->motion ||
                !mouse_cell(*mouse, x, y) || !rect().contains(x, y)) return false;
            const auto first = offset();
            const auto relative = static_cast<std::size_t>(y - rect().y);
            if (relative >= rows_.size() - first) return false;
            select(first + relative);
            return true;
        }
        const auto* key = std::get_if<ttytk::input::Key>(&event);
        if (!key || key->event_type == 3 || key->modifiers != 0) return false;
        const auto selected = selected_index();
        switch (key->codepoint) {
        case 57350: {
            const auto path = rows_[selected].node.path;
            if (expanded(path)) collapse(path);
            else if (rows_[selected].depth > 0) {
                auto parent = selected;
                while (parent > 0 && rows_[parent].depth >= rows_[selected].depth) --parent;
                select(parent);
            }
            return true;
        }
        case 57351: {
            const auto path = rows_[selected].node.path;
            if (!expanded(path)) expand(path);
            else if (selected + 1 < rows_.size() && rows_[selected + 1].depth > rows_[selected].depth) select(selected + 1);
            return true;
        }
        case 57352: if (selected > 0) select(selected - 1); return true;
        case 57353: if (selected + 1 < rows_.size()) select(selected + 1); return true;
        case 57356: select(0); return true;
        case 57357: select(rows_.size() - 1); return true;
        case U'\r': if (key->event_type == 1) choose(); return true;
        default: return false;
        }
    }

    void paint(Canvas& canvas) override {
        if (rect().width <= 0 || rect().height <= 0) return;
        const auto first = offset();
        const auto shown = std::min(visible_length(), rows_.size() - first);
        for (std::size_t i = 0; i < shown; ++i) {
            const auto& row = rows_[first + i];
            std::string label;
            for (std::size_t level = 0; level < std::min(row.depth, static_cast<std::size_t>(rect().width / 2)); ++level)
                label += canvas.unicode() ? "│ " : "| ";
            label += expanded(row.node.path) ? (canvas.unicode() ? "▾ " : "- ") :
                                               (canvas.unicode() ? "▸ " : "+ ");
            label += row.node.label;
            canvas.text(rect().x, rect().y + static_cast<int>(i), fit_text(label, rect().width), style());
        }
    }

private:
    struct Row { TreeNodeRef node; std::size_t depth; };
    [[nodiscard]] auto selected_index() const -> std::size_t {
        const auto found = std::find_if(rows_.begin(), rows_.end(), [this](const auto& row) {
            return row.node.path == selected_;
        });
        return found == rows_.end() ? 0 : static_cast<std::size_t>(found - rows_.begin());
    }
    void project() const {
        if (!dirty_) return;
        rows_.clear();
        if (model_) {
            std::vector<Row> pending;
            std::unordered_set<std::string> visited;
            const auto append = [&pending](const std::vector<TreeNodeRef>& nodes, const std::size_t depth) {
                for (auto it = nodes.rbegin(); it != nodes.rend(); ++it) pending.push_back({*it, depth});
            };
            append(model_->roots(), 0);
            while (!pending.empty()) {
                auto row = std::move(pending.back());
                pending.pop_back();
                if (!visited.insert(row.node.path).second) continue;
                if (expanded(row.node.path)) {
                    auto found = cached_.find(row.node.path);
                    if (found == cached_.end())
                        found = cached_.emplace(row.node.path, model_->children(row.node.path)).first;
                    append(found->second, row.depth + 1);
                }
                rows_.push_back(std::move(row));
            }
        }
        dirty_ = false;
    }
    std::shared_ptr<TreeModel> model_;
    ExpandPolicy policy_{ExpandPolicy::Multiple};
    std::unordered_set<std::string> expanded_;
    std::unordered_set<std::string> collapsed_;
    bool all_{};
    mutable bool dirty_{true};
    mutable std::unordered_map<std::string, std::vector<TreeNodeRef>> cached_;
    mutable std::vector<Row> rows_;
    std::string selected_;
    std::size_t offset_{};
    std::function<void(const TreeNodeRef&)> choose_{};
};

}  // namespace ttyutils::ui
