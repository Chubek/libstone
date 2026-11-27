#pragma once

#include <TTyTk/TTyTk-Automaton.hpp>
#include <TTyTk/TTyTk-CharWidth.hpp>
#include <TTyTk/TTyTk-ColorTools.hpp>
#include <TTyTk/TTyTk-Grampheme.hpp>
#include <TTyUtils/TTyUtils-ScrollBar.hpp>
#include <TTyUtils/TTyUtils-Search.hpp>
#include <TTyUtils/TTyUtils-StatusBar.hpp>
#include <TTyUtils/TTyUtils-Widget.hpp>
#include <TTyUtils/TTyUtils-WordWrap.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <istream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/stat.h>
#endif

namespace ttyutils::pager {

class Source {
public:
    Source() = default;
    explicit Source(std::string text) : text_(std::move(text)) {}
    [[nodiscard]] static auto stream(std::string text) -> Source { return Source{std::move(text)}; }
    /// Borrow a stream. The caller controls when it is safe to pull more bytes.
    [[nodiscard]] static auto stream(std::istream& input) -> Source {
        Source source;
        source.stream_ = &input;
        return source;
    }
    /// Open lazily, reading only regular files, never blocking on a FIFO.
    [[nodiscard]] static auto file(std::filesystem::path path) -> Source {
        Source source;
        source.path_ = std::move(path);
        return source;
    }
    [[nodiscard]] auto text() const noexcept -> std::string_view { return text_; }

    struct Update { std::size_t bytes{}; bool restarted{}; };

    /// Read a bounded chunk. For a borrowed blocking stream this can block.
    [[nodiscard]] auto poll(const std::size_t limit = 64 * 1024) -> Update {
        if (!limit) return {};
        Update update;
        if (stream_) {
            update.bytes = read(*stream_, limit);
        } else if (!path_.empty()) {
            std::error_code error;
            if (!std::filesystem::is_regular_file(path_, error)) return {};
            const auto size = std::filesystem::file_size(path_, error);
            if (error) return {};
            update.restarted = size < file_offset_;
#if defined(__unix__) || defined(__APPLE__)
            struct stat info{};
            if (::stat(path_.c_str(), &info) != 0 || !S_ISREG(info.st_mode)) return {};
            if (identified_ && (device_ != info.st_dev || inode_ != info.st_ino)) update.restarted = true;
            device_ = info.st_dev;
            inode_ = info.st_ino;
            identified_ = true;
#endif
            if (update.restarted) { file_offset_ = 0; text_.clear(); }
            if (size <= file_offset_) return update;
            std::ifstream input(path_, std::ios::binary);
            if (!input || !input.seekg(static_cast<std::streamoff>(file_offset_))) return update;
            update.bytes = read(input, static_cast<std::size_t>(std::min<std::uintmax_t>(limit, size - file_offset_)));
            file_offset_ += update.bytes;
        }
        return update;
    }
    void append(const std::string_view bytes) { text_ += bytes; }

private:
    auto read(std::istream& input, std::size_t limit) -> std::size_t {
        std::array<char, 4096> bytes{};
        std::size_t count = 0;
        while (limit && input) {
            const auto step = std::min(limit, bytes.size());
            input.read(bytes.data(), static_cast<std::streamsize>(step));
            const auto received = static_cast<std::size_t>(input.gcount());
            text_.append(bytes.data(), received);
            count += received;
            if (received < step) break;
            limit -= received;
        }
        return count;
    }

    std::string text_;
    std::filesystem::path path_;
    std::istream* stream_{};
    std::uintmax_t file_offset_{};
#if defined(__unix__) || defined(__APPLE__)
    dev_t device_{};
    ino_t inode_{};
    bool identified_{};
#endif
};

struct Marks {
    std::vector<std::size_t> rows{};
};

class Pager : public ui::Widget, public ui::Scrollable {
public:
    explicit Pager(Source source = {}) : source_(std::move(source)) { update_rows(true); }
    void set_source(Source source) {
        source_ = std::move(source); offset_ = 0; matched_.reset(); update_rows(true);
        invalidate(rect());
    }
    void follow(const bool value) {
        offset_ = value ? max_offset() : offset();
        follow_ = value;
        invalidate(rect());
    }
    [[nodiscard]] auto following() const noexcept -> bool { return follow_; }
    void set_marks(Marks marks) { marks_ = std::move(marks); }
    [[nodiscard]] auto marks() const noexcept -> const Marks& { return marks_; }
    [[nodiscard]] auto jump_mark(const std::size_t index) -> bool {
        if (index >= marks_.rows.size() || marks_.rows[index] >= length()) return false;
        scroll_to(marks_.rows[index]); return true;
    }
    void set_horizontal(const std::size_t column) { horizontal_ = column; invalidate(rect()); }
    void set_wrap(const bool value) { wrap_ = value; invalidate(rect()); }
    [[nodiscard]] auto offset() const -> std::size_t override {
        return follow_ ? max_offset() : std::min(offset_, max_offset());
    }
    void scroll_to(const std::size_t offset) override {
        offset_ = std::min(offset, max_offset());
        if (offset_ < max_offset()) follow_ = false;
        invalidate(rect());
    }
    [[nodiscard]] auto length() const -> std::size_t override { return visible_.size(); }
    [[nodiscard]] auto visible_length() const -> std::size_t override {
        return static_cast<std::size_t>(std::max(1, rect().height));
    }
    /// Pull more input explicitly from a file or caller-managed input stream.
    auto poll(const std::size_t limit = 64 * 1024) -> Source::Update {
        const auto update = source_.poll(limit);
        if (update.bytes || update.restarted) changed(update.restarted);
        return update;
    }
    void append(const std::string_view bytes) {
        if (bytes.empty()) return;
        source_.append(bytes);
        changed(false);
    }
    [[nodiscard]] auto lines() const -> std::vector<std::string_view> {
        std::vector<std::string_view> result;
        result.reserve(visible_.size());
        for (const auto index : visible_) result.push_back(rows_[index].text);
        return result;
    }

    /// Find the next or previous matching visible line, wrapping at the ends.
    [[nodiscard]] auto find(const search::Query& query, const bool forward = true) -> std::optional<std::size_t> {
        if (visible_.empty()) return std::nullopt;
        auto index = matched_.value_or(offset());
        const auto step = [&] {
            if (forward) index = index + 1 == length() ? 0 : index + 1;
            else index = index == 0 ? length() - 1 : index - 1;
        };
        if (matched_) step();
        for (std::size_t i = 0; i < length(); ++i, step()) {
            if (search::find_all(textbuf::Buffer(rows_[visible_[index]].text).snapshot(), query).empty()) continue;
            matched_ = index;
            scroll_to(index);
            return index;
        }
        matched_.reset();
        invalidate(rect());
        return std::nullopt;
    }

    void paint(ui::Canvas& canvas) override {
        const auto area = rect();
        if (area.width <= 0 || area.height <= 0) return;
        int y = 0;
        for (auto index = follow_ ? max_offset() : offset(); index < length() && y < area.height; ++index) {
            const auto& row = rows_[visible_[index]];
            auto base = line_style(row.text).value_or(style());
            if (matched_ == index) base.attributes |= 1U << 6;
            int x = 0;
            std::size_t col = 0;
            std::size_t at = 0;
            std::size_t span = 0;
            std::string run;
            theme::Style current;
            int origin = 0;
            const auto flush = [&] {
                if (run.empty()) return;
                canvas.text(area.x + origin, area.y + y, run, current);
                run.clear();
            };
            while (at < row.text.size() && y < area.height) {
                while (span + 1 < row.spans.size() && row.spans[span + 1].first <= at) ++span;
                const auto next = ttytk::grapheme::next_break(row.text, at);
                const auto glyph = std::string_view(row.text).substr(at, next - at);
                const auto cells = ttytk::charwidth::of_grapheme(glyph);
                at = next;
                if (!wrap_ && col < horizontal_) { col += static_cast<std::size_t>(cells); continue; }
                if (cells > area.width) continue;
                if (x + cells > area.width) {
                    flush();
                    if (!wrap_) break;
                    x = 0;
                    if (++y >= area.height) break;
                }
                auto rendition = base;
                const auto& sgr = row.spans[span].second;
                if (sgr.foreground) rendition.foreground = sgr.foreground;
                if (sgr.background) rendition.background = sgr.background;
                rendition.attributes |= sgr.attributes;
                if (run.empty() || current != rendition) { flush(); origin = x; current = rendition; }
                run += glyph;
                x += cells;
            }
            flush();
            ++y;
        }
    }

protected:
    [[nodiscard]] virtual auto accepts_line(std::string_view) const -> bool { return true; }
    [[nodiscard]] virtual auto line_style(std::string_view) const -> std::optional<theme::Style> { return {}; }
    virtual void source_changed(std::size_t, bool) {}
    void filter_changed() {
        visible_.clear();
        for (std::size_t i = 0; i < rows_.size(); ++i)
            if (accepts_line(rows_[i].text)) visible_.push_back(i);
        matched_.reset();
        offset_ = follow_ ? max_offset() : offset();
        invalidate(rect());
    }

private:
    struct Row {
        std::string text;
        std::vector<std::pair<std::size_t, theme::Style>> spans;
    };

    // ECMA-48 §8.3.117 and xterm's 16-, 256- and direct-color extensions.
    static void apply_sgr(std::string_view sequence, theme::Style& style) {
        if (sequence.empty() || sequence.back() != 'm') return;
        sequence.remove_suffix(1);
        std::vector<unsigned> params;
        for (;;) {
            const auto end = sequence.find(';');
            const auto field = sequence.substr(0, end);
            unsigned value = 0;
            if (!field.empty()) {
                const auto parsed = std::from_chars(field.data(), field.data() + field.size(), value);
                if (parsed.ec != std::errc{} || parsed.ptr != field.data() + field.size()) return;
            }
            params.push_back(value);
            if (end == std::string_view::npos) break;
            sequence.remove_prefix(end + 1);
        }
        for (std::size_t i = 0; i < params.size(); ++i) {
            const auto code = params[i];
            if (code == 0) style = {};
            else if (code <= 9) style.attributes |= 1U << (code - 1U);
            else if (code == 22) style.attributes &= ~3U;
            else if (code == 25) style.attributes &= ~((1U << 4) | (1U << 5));
            else if (code == 23 || code == 24 || (code >= 27 && code <= 29))
                style.attributes &= ~(1U << (code - 21));
            else if (code == 39) style.foreground.reset();
            else if (code == 49) style.background.reset();
            else if (code >= 30 && code <= 37)
                style.foreground = ttytk::color::from_16(static_cast<std::uint8_t>(code - 30));
            else if (code >= 40 && code <= 47)
                style.background = ttytk::color::from_16(static_cast<std::uint8_t>(code - 40));
            else if (code >= 90 && code <= 97)
                style.foreground = ttytk::color::from_16(static_cast<std::uint8_t>(code - 82));
            else if (code >= 100 && code <= 107)
                style.background = ttytk::color::from_16(static_cast<std::uint8_t>(code - 92));
            else if ((code == 38 || code == 48) && i + 1 < params.size()) {
                auto& target = code == 38 ? style.foreground : style.background;
                const auto mode = params[++i];
                if (mode == 5 && i + 1 < params.size()) {
                    if (params[i + 1] <= 255)
                        target = ttytk::color::from_256(static_cast<std::uint8_t>(params[i + 1]));
                    ++i;
                } else if (mode == 2 && i + 3 < params.size()) {
                    if (params[i + 1] <= 255 && params[i + 2] <= 255 && params[i + 3] <= 255)
                        target = ttytk::color::Rgb{static_cast<std::uint8_t>(params[i + 1]),
                                                  static_cast<std::uint8_t>(params[i + 2]),
                                                  static_cast<std::uint8_t>(params[i + 3])};
                    i += 3;
                } else break;
            }
        }
    }
    void changed(const bool restarted) {
        const auto previous = length();
        if (restarted) { offset_ = 0; matched_.reset(); }
        update_rows(restarted);
        if (follow_) offset_ = max_offset();
        source_changed(previous, restarted);
        invalidate(rect());
    }
    void update_rows(const bool reset) {
        if (reset) {
            rows_.clear(); visible_.clear(); complete_ = 0; next_ = 0; last_style_ = {};
        }
        if (rows_.size() > complete_) {
            rows_.pop_back();
            if (!visible_.empty() && visible_.back() == complete_) visible_.pop_back();
        }
        const auto text = source_.text();
        while (next_ < text.size()) {
            const auto end = text.find('\n', next_);
            auto row_style = last_style_;
            Row row;
            row.spans.emplace_back(0, row_style);
            ttytk::Automaton parser;
            parser.on_print = [&](const std::string_view value) { row.text += value; };
            parser.on_control = [&](const unsigned char control) {
                if (control == '\t') row.text.append(8 - (ttytk::charwidth::of_string(row.text) % 8), ' ');
            };
            parser.on_csi = [&](const std::string_view sequence) {
                const auto previous = row_style;
                apply_sgr(sequence, row_style);
                if (row_style != previous) row.spans.emplace_back(row.text.size(), row_style);
            };
            parser.feed(text.substr(next_, end == std::string_view::npos ? end : end - next_));
            if (accepts_line(row.text)) visible_.push_back(rows_.size());
            rows_.push_back(std::move(row));
            if (end == std::string_view::npos) break;
            ++complete_;
            next_ = end + 1;
            last_style_ = row_style;
        }
        if (matched_ && *matched_ >= length()) matched_.reset();
    }

    Source source_;
    Marks marks_;
    std::vector<Row> rows_;
    std::vector<std::size_t> visible_;
    std::optional<std::size_t> matched_;
    std::size_t complete_{};
    std::size_t next_{};
    theme::Style last_style_;
    std::size_t offset_{};
    std::size_t horizontal_{};
    bool follow_{};
    bool wrap_{};
};

inline auto page_file(const std::filesystem::path& path) -> Pager {
    auto source = Source::file(path);
    (void)source.poll();
    return Pager{std::move(source)};
}

inline auto page_stream(std::istream& input) -> Pager {
    return Pager{Source::stream(input)};
}

}  // namespace ttyutils::pager
