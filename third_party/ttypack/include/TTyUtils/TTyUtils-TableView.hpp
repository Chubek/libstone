#pragma once

#include <TTyUtils/TTyUtils-ListView.hpp>

#include <algorithm>
#include <functional>
#include <memory>
#include <numeric>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ttyutils::ui {

enum ColumnAlign { Left = 0, Right = 1, Center = 2 };
enum class SortOrder { Asc, Desc };

struct Column {
    std::string title{};
    int width{10};
    ColumnAlign align{Left};
};

class TableModel {
public:
    virtual ~TableModel() = default;
    [[nodiscard]] virtual auto rows() const -> std::size_t = 0;
    [[nodiscard]] virtual auto value(std::size_t row, std::size_t column) const -> std::string = 0;
};

class TableView : public Widget, public Scrollable {
public:
    using CellStyle = std::function<theme::Style(std::size_t, std::size_t, std::string_view)>;

    explicit TableView(std::shared_ptr<TableModel> model = {}) : model_(std::move(model)) {}
    void set_model(std::shared_ptr<TableModel> model) { model_ = std::move(model); offset_ = 0; refresh(); }
    void add_column(Column column) {
        model_columns_.push_back(columns_.size());
        columns_.push_back(std::move(column));
    }
    void resize_column(const std::size_t column, const int width) {
        if (column < columns_.size()) columns_[column].width = std::max(0, width);
    }
    /** Move a display column while retaining its association with the model. */
    void move_column(const std::size_t from, const std::size_t to) {
        if (from >= columns_.size() || to >= columns_.size()) return;
        const auto move = [from, to](auto& items) {
            auto first = items.begin();
            if (from < to) std::rotate(first + from, first + from + 1, first + to + 1);
            else if (from > to) std::rotate(first + to, first + from, first + from + 1);
        };
        move(columns_);
        move(model_columns_);
    }
    void sort_by(const std::size_t column, const SortOrder order = SortOrder::Asc, const bool append = false) {
        if (column >= columns_.size()) return;
        if (!append) sort_.clear();
        const auto key = model_columns_[column];
        const auto found = std::find_if(sort_.begin(), sort_.end(), [key](const auto& pair) { return pair.first == key; });
        if (found == sort_.end()) sort_.emplace_back(key, order);
        else found->second = order;
        refresh();
    }
    void sort_by(const std::string_view title, const SortOrder order = SortOrder::Asc, const bool append = false) {
        const auto found = std::find_if(columns_.begin(), columns_.end(), [title](const auto& column) {
            return column.title == title;
        });
        if (found != columns_.end()) sort_by(static_cast<std::size_t>(found - columns_.begin()), order, append);
    }
    void set_cell_style(CellStyle callback) { cell_style_ = std::move(callback); }
    void set_header_style(theme::Style style) { header_style_ = std::move(style); }
    /** Recompute the active sort after the model changes. Unsorted paint only reads visible rows. */
    void refresh() {
        sorted_.clear();
        if (!model_ || sort_.empty()) return;
        const auto count = model_->rows();
        sorted_.resize(count);
        std::iota(sorted_.begin(), sorted_.end(), std::size_t{0});
        std::vector<std::vector<std::string>> keys;
        for (const auto& [column, order] : sort_) {
            (void)order;
            auto& values = keys.emplace_back();
            values.reserve(count);
            for (std::size_t i = 0; i < count; ++i) values.push_back(model_->value(i, column));
        }
        std::stable_sort(sorted_.begin(), sorted_.end(), [&](const auto lhs, const auto rhs) {
            for (std::size_t key = 0; key < keys.size(); ++key) {
                if (keys[key][lhs] == keys[key][rhs]) continue;
                return sort_[key].second == SortOrder::Asc ? keys[key][lhs] < keys[key][rhs] :
                                                            keys[key][rhs] < keys[key][lhs];
            }
            return false;
        });
    }
    [[nodiscard]] auto columns() const noexcept -> const std::vector<Column>& { return columns_; }
    [[nodiscard]] auto length() const -> std::size_t override {
        return !model_ ? 0 : sort_.empty() ? model_->rows() : sorted_.size();
    }
    [[nodiscard]] auto visible_length() const -> std::size_t override {
        return rect().height > 1 ? static_cast<std::size_t>(rect().height - 1) : 0;
    }
    [[nodiscard]] auto offset() const -> std::size_t override { return std::min(offset_, max_offset()); }
    void scroll_to(const std::size_t row) override { offset_ = std::min(row, max_offset()); }

    auto handle(const ttytk::input::Event& event) -> bool override {
        const auto* mouse = std::get_if<ttytk::input::Mouse>(&event);
        int x{}, y{};
        if (!mouse || mouse->button != 0 || !mouse->pressed || mouse->motion ||
            !mouse_cell(*mouse, x, y) || y != rect().y || !rect().contains(x, y)) return false;
        auto column_x = x - rect().x;
        for (std::size_t column = 0; column < columns_.size(); ++column) {
            const auto width = std::max(0, columns_[column].width);
            if (column_x < width) {
                const auto key = model_columns_[column];
                const auto found = std::find_if(sort_.begin(), sort_.end(), [key](const auto& pair) {
                    return pair.first == key;
                });
                const auto order = found != sort_.end() && found->second == SortOrder::Asc ? SortOrder::Desc : SortOrder::Asc;
                sort_by(column, order, (mouse->modifiers & 4U) != 0);
                return true;
            }
            column_x -= width;
        }
        return false;
    }

    void paint(Canvas& canvas) override {
        if (rect().width <= 0 || rect().height <= 0) return;
        draw_row(canvas, rect().y, std::nullopt);
        const auto first = offset();
        const auto visible = std::min(visible_length(), length() - first);
        for (std::size_t i = 0; i < visible; ++i) {
            const auto row = sort_.empty() ? first + i : sorted_[first + i];
            draw_row(canvas, rect().y + 1 + static_cast<int>(i), row);
        }
    }

private:
    void draw_row(Canvas& canvas, const int y, const std::optional<std::size_t> row) const {
        int used = 0;
        for (std::size_t i = 0; i < columns_.size() && used < rect().width; ++i) {
            const auto& column = columns_[i];
            const auto width = std::clamp(column.width, 0, rect().width - used);
            if (width == 0) continue;
            const auto source = model_columns_[i];
            const auto value = row ? model_->value(*row, source) : column.title;
            const auto cell_style = row ? (cell_style_ ? cell_style_(*row, source, value) : style())
                                        : header_style_.value_or(style());
            std::string text;
            if (cell_width(value) > width) {
                text = fit_text(value, width - 1);
                text += canvas.unicode() ? "…" : ".";
            } else {
                text = value;
            }
            const auto gap = width - cell_width(text);
            const auto before = column.align == Right ? gap : column.align == Center ? gap / 2 : 0;
            text.insert(0, static_cast<std::size_t>(before), ' ');
            text.append(static_cast<std::size_t>(gap - before), ' ');
            canvas.text(rect().x + used, y, text, cell_style);
            used += width;
        }
    }
    std::shared_ptr<TableModel> model_;
    std::vector<Column> columns_;
    std::vector<std::size_t> model_columns_;
    std::vector<std::pair<std::size_t, SortOrder>> sort_;
    std::vector<std::size_t> sorted_;
    std::size_t offset_{};
    CellStyle cell_style_;
    std::optional<theme::Style> header_style_;
};

}  // namespace ttyutils::ui
