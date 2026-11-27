#pragma once

#include <TTyTk/TTyTk-Core.hpp>
#include <TTyTk/TTyTk-Grampheme.hpp>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ttyutils::textbuf {

/** A slice of the original or append-only edit storage. */
struct Piece {
    enum class Source { original, added };
    Source source{Source::original};
    std::size_t offset{};
    std::size_t length{};
};

/** Index of logical lines and grapheme boundaries within each line. */
class LineIndex {
public:
    struct Position {
        std::size_t line{};
        std::size_t col{};
        [[nodiscard]] friend constexpr auto operator==(const Position&, const Position&) -> bool = default;
    };
    struct Range {
        Position begin{};
        Position end{};
    };

    LineIndex() : LineIndex("") {}
    explicit LineIndex(const std::string_view text) { rebuild(text); }

    void rebuild(const std::string_view text) {
        auto data = std::make_shared<Data>();
        auto start = text.starts_with("\xEF\xBB\xBF") ? std::size_t{3} : std::size_t{0};
        for (;;) {
            const auto lf = text.find('\n', start);
            auto end = lf == std::string_view::npos ? text.size() : lf;
            if (lf != std::string_view::npos && end > start && text[end - 1] == '\r') --end;
            data->rows.push_back(data->columns.size());
            data->columns.push_back(start);
            const auto line = text.substr(start, end - start);
            for (std::size_t byte = 0; byte < line.size();) {
                byte = ttytk::grapheme::next_break(line, byte);
                data->columns.push_back(start + byte);
            }
            if (lf == std::string_view::npos) break;
            start = lf + 1;
        }
        data->rows.push_back(data->columns.size());
        data_ = std::move(data);
    }

    [[nodiscard]] auto lines() const noexcept -> std::size_t { return data_->rows.size() - 1; }

    [[nodiscard]] auto columns(const std::size_t line) const noexcept -> std::size_t {
        return line < lines() ? data_->rows[line + 1] - data_->rows[line] - 1 : 0;
    }

    [[nodiscard]] auto offset(const Position position, const std::string_view) const noexcept -> std::size_t {
        if (position.line >= lines()) return data_->columns.back();
        return data_->columns[data_->rows[position.line] + std::min(position.col, columns(position.line))];
    }

    /** Map arbitrary byte positions to their containing grapheme, or the end of line. */
    [[nodiscard]] auto position(const std::size_t byte, const std::string_view) const noexcept -> Position {
        const auto boundary = std::upper_bound(data_->columns.begin(), data_->columns.end(), byte);
        const auto entry = boundary == data_->columns.begin() ? 0 :
            static_cast<std::size_t>(boundary - data_->columns.begin() - 1);
        const auto row = std::upper_bound(data_->rows.begin(), data_->rows.end(), entry);
        const auto line = static_cast<std::size_t>(row - data_->rows.begin() - 1);
        return {line, entry - data_->rows[line]};
    }

    [[nodiscard]] auto line(const std::size_t number, const std::string_view text) const -> std::string_view {
        if (number >= lines()) return {};
        const auto begin = data_->columns[data_->rows[number]];
        const auto end = data_->columns[data_->rows[number + 1] - 1];
        return text.substr(begin, end - begin);
    }

private:
    struct Data {
        std::vector<std::size_t> rows;
        std::vector<std::size_t> columns;
    };
    std::shared_ptr<const Data> data_;
};

/** An immutable, inexpensive view of a specific buffer revision. */
class Snapshot {
public:
    Snapshot() : text_(std::make_shared<const std::string>()) {}

    [[nodiscard]] auto text() const noexcept -> std::string_view { return *text_; }
    [[nodiscard]] auto line_index() const noexcept -> LineIndex { return index_; }
    [[nodiscard]] auto line(std::size_t number) const -> std::string_view { return index_.line(number, text()); }
    [[nodiscard]] auto generation() const noexcept -> std::size_t { return generation_; }

private:
    friend class Buffer;
    Snapshot(std::shared_ptr<const std::string> text, LineIndex index, const std::size_t generation)
        : text_(std::move(text)), index_(std::move(index)), generation_(generation) {}

    std::shared_ptr<const std::string> text_;
    LineIndex index_{};
    std::size_t generation_{};
};

/** Piece-table editor. Logical positions exclude the BOM and line endings. */
class Buffer {
public:
    using Position = LineIndex::Position;

    enum class Eol { lf, crlf, mixed };

    struct Edit {
        Snapshot before;
        std::size_t offset;
        std::size_t erased;
        std::size_t inserted;
    };

    using EditListener = std::function<void(const Edit&)>;
    using EditSubscription = std::shared_ptr<EditListener>;

    Buffer() : Buffer(std::string{}) {}
    explicit Buffer(std::string text) : original_(std::make_shared<const std::string>(std::move(text))),
        text_(original_), index_(*text_) {
        if (!text_->empty()) pieces_.push_back({Piece::Source::original, 0, text_->size()});
        inspect_encoding();
    }

    Buffer(const Buffer& other)
        : original_(other.original_), added_(other.added_), pieces_(other.pieces_), text_(other.text_),
          index_(other.index_), eol_(other.eol_), bom_(other.bom_), generation_(other.generation_) {}

    auto operator=(const Buffer& other) -> Buffer& {
        if (this != &other) replace_bytes(0, size(), other.text());
        return *this;
    }

    [[nodiscard]] auto text() const noexcept -> std::string_view { return *text_; }
    [[nodiscard]] auto size() const noexcept -> std::size_t { return text_->size(); }
    [[nodiscard]] auto line_index() const noexcept -> LineIndex { return index_; }
    [[nodiscard]] auto eol() const noexcept -> Eol { return eol_; }
    [[nodiscard]] auto has_bom() const noexcept -> bool { return bom_; }
    [[nodiscard]] auto generation() const noexcept -> std::size_t { return generation_; }

    [[nodiscard]] auto snapshot() const -> Snapshot {
        return Snapshot{text_, index_, generation_};
    }

    /** Listen to edits for as long as the returned subscription is held. */
    [[nodiscard]] auto observe_edits(EditListener listener) -> EditSubscription {
        auto subscription = std::make_shared<EditListener>(std::move(listener));
        listeners_.push_back(subscription);
        return subscription;
    }

    void insert(const Position position, const std::string_view value) {
        replace(position, position, value);
    }

    void erase(const Position first, const Position last) {
        replace(first, last, {});
    }

    void replace(const Position first, const Position last, const std::string_view value) {
        const auto begin = index_.offset(first, text());
        const auto end = index_.offset(last, text());
        if (end >= begin) replace_bytes(begin, end, value);
    }

    /** Replace raw bytes, including file metadata if selected. */
    void replace_bytes(std::size_t first, std::size_t last, const std::string_view value) {
        first = std::min(first, size());
        last = std::min(last, size());
        if (last < first || text().substr(first, last - first) == value) return;
        const auto before = snapshot();
        std::vector<Piece> pieces;
        auto append_piece = [&pieces](Piece piece) {
            if (piece.length == 0) return;
            if (!pieces.empty() && pieces.back().source == piece.source &&
                pieces.back().offset + pieces.back().length == piece.offset)
                pieces.back().length += piece.length;
            else
                pieces.push_back(piece);
        };
        auto append_range = [&](std::size_t begin, std::size_t end) {
            std::size_t at = 0;
            for (const auto& piece : pieces_) {
                if (at >= end) break;
                const auto first_in = std::max(begin, at);
                const auto last_in = std::min(end, at + piece.length);
                if (first_in < last_in)
                    append_piece({piece.source, piece.offset + first_in - at, last_in - first_in});
                at += piece.length;
            }
        };
        append_range(0, first);
        append_piece({Piece::Source::added, added_.size(), value.size()});
        append_range(last, size());
        if (!value.empty()) added_.append(value);

        auto materialized = std::make_shared<std::string>();
        materialized->reserve(size() - (last - first) + value.size());
        for (const auto& piece : pieces) {
            const auto& storage = piece.source == Piece::Source::original ? *original_ : added_;
            materialized->append(storage, piece.offset, piece.length);
        }
        LineIndex index{*materialized};
        pieces_ = std::move(pieces);
        text_ = std::move(materialized);
        index_ = std::move(index);
        ++generation_;
        inspect_encoding();

        std::vector<EditSubscription> live;
        std::erase_if(listeners_, [&live](const auto& listener) {
            if (auto subscription = listener.lock()) {
                live.push_back(std::move(subscription));
                return false;
            }
            return true;
        });
        for (const auto& listener : live) (*listener)({before, first, last - first, value.size()});
    }

    [[nodiscard]] auto line(const std::size_t number) const -> std::string_view {
        return index_.line(number, text());
    }

private:
    void inspect_encoding() noexcept {
        bom_ = text_->starts_with("\xEF\xBB\xBF");
        bool crlf = false;
        bool lf = false;
        for (auto at = text_->find('\n'); at != std::string::npos; at = text_->find('\n', at + 1)) {
            if (at != 0 && (*text_)[at - 1] == '\r') crlf = true;
            else lf = true;
            if (crlf && lf) break;
        }
        eol_ = crlf ? (lf ? Eol::mixed : Eol::crlf) : Eol::lf;
    }

    std::shared_ptr<const std::string> original_;
    std::string added_;
    std::vector<Piece> pieces_;
    std::shared_ptr<const std::string> text_;
    LineIndex index_;
    Eol eol_{Eol::lf};
    bool bom_{};
    std::size_t generation_{};
    std::vector<std::weak_ptr<EditListener>> listeners_;
};

[[nodiscard]] inline auto load_file(const std::filesystem::path& path) -> ttytk::Result<Buffer> {
    std::ifstream input(path, std::ios::binary);
    if (!input) return ttytk::Error{1, "unable to open text file"};
    std::string text{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    if (!input.good() && !input.eof()) return ttytk::Error{2, "unable to read text file"};
    return Buffer{std::move(text)};
}

[[nodiscard]] inline auto save_file(const Buffer& buffer, const std::filesystem::path& path) -> ttytk::Result<void> {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) return ttytk::Error{1, "unable to open text file for writing"};
    output.write(buffer.text().data(), static_cast<std::streamsize>(buffer.text().size()));
    output.close();
    if (!output) return ttytk::Error{2, "unable to write text file"};
    return {};
}

}  // namespace ttyutils::textbuf
