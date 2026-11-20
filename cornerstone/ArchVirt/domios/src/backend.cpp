#include "backend.hpp"

#include <algorithm>
#include <memory>

namespace domios {

BufferSurface::BufferSurface(Geometry geometry)
    : geometry_(geometry), pixels_(static_cast<std::size_t>(geometry.width) * geometry.height, 0) {}

void BufferSurface::clear(std::uint32_t color) {
    std::fill(pixels_.begin(), pixels_.end(), color);
}

void BufferSurface::fill_rect(std::uint32_t x, std::uint32_t y, std::uint32_t w, std::uint32_t h, std::uint32_t color) {
    const std::uint32_t x2 = std::min<std::uint32_t>(geometry_.width, x + w);
    const std::uint32_t y2 = std::min<std::uint32_t>(geometry_.height, y + h);
    for (std::uint32_t yy = y; yy < y2; ++yy) {
        for (std::uint32_t xx = x; xx < x2; ++xx) {
            pixels_[static_cast<std::size_t>(yy) * geometry_.width + xx] = color;
        }
    }
}

bool VgaBackend::initialize() {
    framebuffer_.assign(static_cast<std::size_t>(geometry_.width) * geometry_.height, 0);
    return true;
}

std::unique_ptr<DrawingSurface> VgaBackend::acquire_surface() {
    return std::make_unique<BufferSurface>(geometry_);
}

bool VgaBackend::present(const DrawingSurface& surface) {
    const auto& pixels = surface.pixels();
    framebuffer_.assign(pixels.begin(), pixels.end());
    return true;
}

std::vector<std::uint32_t> VgaBackend::snapshot() const {
    return framebuffer_;
}

void VgaBackend::shutdown() noexcept {
    framebuffer_.clear();
}

bool VirtioGpuBackend::initialize() {
    framebuffer_.assign(static_cast<std::size_t>(geometry_.width) * geometry_.height, 0);
    return true;
}

std::unique_ptr<DrawingSurface> VirtioGpuBackend::acquire_surface() {
    return std::make_unique<BufferSurface>(geometry_);
}

bool VirtioGpuBackend::present(const DrawingSurface& surface) {
    const auto& pixels = surface.pixels();
    framebuffer_.assign(pixels.begin(), pixels.end());
    return true;
}

std::vector<std::uint32_t> VirtioGpuBackend::snapshot() const {
    return framebuffer_;
}

void VirtioGpuBackend::shutdown() noexcept {
    framebuffer_.clear();
}

std::unique_ptr<GraphicsBackend> choose_backend(Config::GraphicsChoice choice, EventQueue& events, std::string& reason) {
    if (choice == Config::GraphicsChoice::vga) {
        auto backend = std::make_unique<VgaBackend>();
        if (!backend->initialize()) {
            reason = "VGA backend initialization failed";
            events.try_push({EventKind::diagnostic, 1, "graphics", reason});
            return {};
        }
        reason = "VGA backend selected by config";
        return backend;
    }
    if (choice == Config::GraphicsChoice::virtio_gpu) {
        auto backend = std::make_unique<VirtioGpuBackend>();
        if (!backend->initialize()) {
            reason = "VirtIO-GPU backend initialization failed";
            events.try_push({EventKind::diagnostic, 1, "graphics", reason});
            return {};
        }
        reason = "VirtIO-GPU backend selected by config";
        return backend;
    }

    auto virtio = std::make_unique<VirtioGpuBackend>();
    if (virtio->initialize()) {
        reason = "auto selected VirtIO-GPU";
        return virtio;
    }
    events.try_push({EventKind::diagnostic, 1, "graphics", "VirtIO-GPU probe failed; falling back to VGA"});
    auto vga = std::make_unique<VgaBackend>();
    if (!vga->initialize()) {
        reason = "VGA backend initialization failed after VirtIO-GPU probe failure";
        events.try_push({EventKind::diagnostic, 1, "graphics", reason});
        return {};
    }
    reason = "auto fell back to VGA";
    return vga;
}

} // namespace domios
