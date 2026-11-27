#pragma once

#include <TTyTk/TTyTk-ColorTools.hpp>
#include <TTyTk/TTyTk-ImageProto.hpp>
#include <TTyUtils/TTyUtils-Widget.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <utility>

namespace ttyutils::imgview {

enum class FitMode { Contain, Cover, Actual };
enum class Backend { Auto, Kitty, Sixel, Iterm2, Cells };

class ImageView : public ui::Widget {
public:
    /// Load an encoded image for an iTerm2-compatible transport.
    /// For cell, Sixel or Kitty rendering, decode the file and call set_image().
    [[nodiscard]] auto load(const std::filesystem::path& path) -> bool {
        std::error_code error;
        if (!std::filesystem::is_regular_file(path, error)) return false;
        std::ifstream input(path, std::ios::binary);
        if (!input) return false;
        std::string data{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
        if (input.bad() || data.empty()) return false;
        data_ = std::move(data);
        image_ = {};
        path_ = path.string();
        invalidate(rect());
        return true;
    }
    /// Set a decoded row-major RGB image, leaving the current image unchanged on failure.
    [[nodiscard]] auto set_image(ttytk::Image image) -> bool {
        if (!image.width || !image.height || image.pixels.size() / image.width != image.height ||
            image.pixels.size() % image.width) return false;
        image_ = std::move(image);
        path_.clear();
        data_.clear();
        invalidate(rect());
        return true;
    }
    void set_fit(const FitMode mode) { fit_ = mode; invalidate(rect()); }
    void set_backend(const Backend backend) { backend_ = backend; invalidate(rect()); }
    [[nodiscard]] auto fit() const noexcept -> FitMode { return fit_; }
    [[nodiscard]] auto backend() const noexcept -> Backend { return backend_; }
    [[nodiscard]] auto path() const noexcept -> const std::string& { return path_; }

    /// Encode for the selected protocol; the caller owns capability detection,
    /// terminal output, positioning, and placement lifetime.
    [[nodiscard]] auto protocol_bytes() const -> std::string {
        if (backend_ == Backend::Iterm2 && !data_.empty()) return ttytk::image::ITerm2::encode_file(data_);
        if (image_.pixels.empty()) return {};
        switch (backend_) {
        case Backend::Kitty: return ttytk::image::Kitty::encode(image_.pixels, image_.width, image_.height);
        case Backend::Sixel: return ttytk::image::Sixel::encode(image_.pixels, image_.width, image_.height);
        case Backend::Iterm2: return ttytk::image::ITerm2::encode(image_.pixels, image_.width, image_.height);
        default: return {};
        }
    }

    void paint(ui::Canvas& canvas) override {
        if ((backend_ != Backend::Auto && backend_ != Backend::Cells) || image_.pixels.empty()) return;
        const auto area = rect();
        if (area.empty()) return;
        const double width = static_cast<double>(image_.width);
        const double height = static_cast<double>(image_.height);
        const double vertical = 2.0 * area.height;
        const auto rx = area.width / width;
        const auto ry = vertical / height;
        const auto scale = fit_ == FitMode::Contain ? std::min(rx, ry) :
                           fit_ == FitMode::Cover ? std::max(rx, ry) : 1.0;
        const auto fitted_width = std::max(1.0, std::round(width * scale));
        const auto fitted_height = std::max(1.0, std::round(height * scale));
        const auto left = std::floor((area.width - fitted_width) / 2.0);
        const auto top = std::floor((vertical - fitted_height) / 2.0);
        const auto sample = [&](const int x, const double y) -> std::optional<ttytk::color::Rgb> {
            const auto dx = x - left, dy = y - top;
            if (dx < 0 || dy < 0 || dx >= fitted_width || dy >= fitted_height) return {};
            const auto px = std::min(image_.width - 1, static_cast<std::size_t>(dx / fitted_width * width));
            const auto py = std::min(image_.height - 1, static_cast<std::size_t>(dy / fitted_height * height));
            return image_.pixels[py * image_.width + px];
        };
        const auto visible = area.intersect(canvas.clip());
        for (int row = 0; row < visible.height; ++row) {
            const auto y = visible.y + row - area.y;
            for (int column = 0; column < visible.width; ++column) {
                const auto x = visible.x + column - area.x;
                const auto upper = sample(x, 2.0 * y);
                const auto lower = sample(x, 2.0 * y + 1.0);
                if (!upper && !lower) continue;
                auto rendition = style();
                if (!canvas.unicode()) {
                    rendition.background = upper ? upper : lower;
                    canvas.text(area.x + x, area.y + y, " ", rendition);
                } else {
                    rendition.foreground = upper ? upper : lower;
                    if (upper && lower) rendition.background = lower;
                    canvas.text(area.x + x, area.y + y, upper ? "▀" : "▄", rendition);
                }
            }
        }
    }

private:
    std::string path_;
    std::string data_;
    ttytk::Image image_;
    FitMode fit_{FitMode::Contain};
    Backend backend_{Backend::Auto};
};

inline auto show(ImageView& view, const std::filesystem::path& path) -> bool {
    return view.load(path);
}

}  // namespace ttyutils::imgview
