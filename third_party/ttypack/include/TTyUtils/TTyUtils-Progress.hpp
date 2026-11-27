#pragma once

#include <TTyUtils/TTyUtils-EventLoop.hpp>
#include <TTyUtils/TTyUtils-Widget.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ttyutils::ui {

class EtaModel {
public:
    /** Record completed work over an interval. Rates decay with a four-second time constant. */
    void update(const double amount, const std::chrono::steady_clock::duration elapsed) {
        if (elapsed.count() <= 0 || !std::isfinite(amount) || amount < 0.0) return;
        const auto seconds = std::chrono::duration<double>(elapsed).count();
        const auto sample = amount / seconds;
        if (!std::isfinite(sample)) return;
        rate_ = sampled_ ? std::lerp(rate_, sample, -std::expm1(-seconds / 4.0)) : sample;
        sampled_ = true;
    }
    [[nodiscard]] auto rate() const noexcept -> double { return rate_; }
    [[nodiscard]] auto remaining(const double amount) const noexcept -> double {
        return rate_ > 0.0 && amount > 0.0 ? amount / rate_ : 0.0;
    }

private:
    double rate_{};
    bool sampled_{};
};

class ProgressBar : public Widget {
public:
    void set_total(const double total) noexcept {
        total_ = std::isfinite(total) ? std::max(0.0, total) : 0.0;
        set_value(value_);
    }
    void set_value(const double value) noexcept {
        if (!std::isnan(value)) value_ = std::clamp(value, 0.0, total_);
    }
    [[nodiscard]] auto fraction() const noexcept -> double { return total_ == 0.0 ? 0.0 : value_ / total_; }
    void advance(const double amount) noexcept { set_value(value_ + amount); }
    void set_ascii(const bool value) noexcept { ascii_ = value; }

    /** Format one row without terminal escapes, also suitable for inline CLI output. */
    [[nodiscard]] auto line(const int width, const bool unicode = true) const -> std::string {
        if (width <= 0) return {};
        constexpr std::string_view blocks[]{"", "▏", "▎", "▍", "▌", "▋", "▊", "▉"};
        const auto cells = fraction() * width;
        const auto filled = static_cast<int>(cells);
        const auto partial = std::min(7, static_cast<int>((cells - filled) * 8));
        if (ascii_ || !unicode) return std::string(static_cast<std::size_t>(filled), '=') +
                           std::string(static_cast<std::size_t>(width - filled), ' ');
        std::string result;
        for (int i = 0; i < filled; ++i) result += "█";
        result += blocks[partial];
        result.append(static_cast<std::size_t>(width - filled - (partial != 0)), ' ');
        return result;
    }

    void paint(Canvas& canvas) override {
        if (rect().width > 0 && rect().height > 0)
            canvas.text(rect().x, rect().y, line(rect().width, canvas.unicode()), style());
    }
private:
    double total_{};
    double value_{};
    bool ascii_{};
};

class Spinner : public Widget {
public:
    void set_ascii(const bool value) noexcept { ascii_ = value; frame_ = 0; }
    void tick() noexcept { frame_ = (frame_ + 1) % (ascii_ ? 4U : 8U); }
    void paint(Canvas& canvas) override {
        constexpr std::string_view glyphs[]{"⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧"};
        if (rect().width > 0 && rect().height > 0)
            canvas.text(rect().x, rect().y, ascii_ || !canvas.unicode()
                        ? std::string_view{"|/-\\"}.substr(frame_ % 4, 1) : glyphs[frame_], style());
    }

private:
    std::size_t frame_{};
    bool ascii_{};
};

class MultiProgress : public Widget {
    struct Entry {
        std::string name;
        double total{};
        double value{};
        EtaModel eta{};
        std::chrono::steady_clock::time_point updated{std::chrono::steady_clock::now()};
    };

public:
    /** A task remains valid as other tasks are added, and becomes inert after its owner is destroyed. */
    class Task {
    public:
        void advance(const double amount) noexcept {
            const auto entry = entry_.lock();
            if (!entry || !std::isfinite(amount) || amount < 0.0) return;
            const auto next = std::min(entry->total, entry->value + amount);
            const auto now = std::chrono::steady_clock::now();
            if (now > entry->updated) {
                entry->eta.update(next - entry->value, now - entry->updated);
                entry->updated = now;
            }
            entry->value = next;
        }
        [[nodiscard]] auto value() const noexcept -> double {
            const auto entry = entry_.lock();
            return entry ? entry->value : 0.0;
        }
        [[nodiscard]] auto rate() const noexcept -> double {
            const auto entry = entry_.lock();
            return entry ? entry->eta.rate() : 0.0;
        }
        [[nodiscard]] auto remaining() const noexcept -> double {
            const auto entry = entry_.lock();
            return entry ? entry->eta.remaining(entry->total - entry->value) : 0.0;
        }

    private:
        friend class MultiProgress;
        explicit Task(const std::shared_ptr<Entry>& entry) : entry_(entry) {}
        std::weak_ptr<Entry> entry_;
    };

    Task add(std::string name, const double total) {
        entries_.push_back(std::make_shared<Entry>(Entry{std::move(name), std::isfinite(total) ? std::max(total, 0.0) : 0.0}));
        return Task{entries_.back()};
    }

    void paint(Canvas& canvas) override {
        if (rect().width <= 0 || rect().height <= 0) return;
        const auto rows = std::min(entries_.size(), static_cast<std::size_t>(rect().height));
        for (std::size_t i = 0; i < rows; ++i) {
            const auto& entry = *entries_[i];
            ProgressBar bar;
            bar.set_total(entry.total);
            bar.set_value(entry.value);
            const auto label = entry.name + ": ";
            const auto clipped = fit_text(label, rect().width);
            canvas.text(rect().x, rect().y + static_cast<int>(i), std::string(clipped) +
                        bar.line(rect().width - cell_width(clipped), canvas.unicode()), style());
        }
    }

private:
    std::vector<std::shared_ptr<Entry>> entries_;
};

}  // namespace ttyutils::ui
