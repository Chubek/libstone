#pragma once

#include "dit.hpp"
#include "event_queue.hpp"

#include <cstdint>
#include <memory>
#include <vector>

namespace domios {

class DrawingSurface {
public:
    virtual ~DrawingSurface() = default;
    virtual void clear(std::uint32_t color) = 0;
    virtual void fill_rect(std::uint32_t x, std::uint32_t y, std::uint32_t w, std::uint32_t h, std::uint32_t color) = 0;
    virtual const std::vector<std::uint32_t>& pixels() const = 0;
};

class GraphicsBackend {
public:
    virtual ~GraphicsBackend() = default;
    virtual const char* name() const noexcept = 0;
    virtual bool initialize() = 0;
    virtual Geometry geometry() const noexcept = 0;
    virtual DitFormat format() const noexcept = 0;
    virtual std::unique_ptr<DrawingSurface> acquire_surface() = 0;
    virtual bool present(const DrawingSurface& surface) = 0;
    virtual std::vector<std::uint32_t> snapshot() const = 0;
    virtual void shutdown() noexcept = 0;
};

class VgaBackend final : public GraphicsBackend {
public:
    bool initialize() override;
    const char* name() const noexcept override { return "vga"; }
    Geometry geometry() const noexcept override { return geometry_; }
    DitFormat format() const noexcept override { return DitFormat::vga_text; }
    std::unique_ptr<DrawingSurface> acquire_surface() override;
    bool present(const DrawingSurface& surface) override;
    std::vector<std::uint32_t> snapshot() const override;
    void shutdown() noexcept override;
    void set_geometry(Geometry geometry) noexcept { geometry_ = geometry; }

private:
    Geometry geometry_{80u, 25u, 80u, PixelFormat::vga_text};
    std::vector<std::uint32_t> framebuffer_;
};

class VirtioGpuBackend final : public GraphicsBackend {
public:
    bool initialize() override;
    const char* name() const noexcept override { return "virtio-gpu"; }
    Geometry geometry() const noexcept override { return geometry_; }
    DitFormat format() const noexcept override { return DitFormat::xrgb8888; }
    std::unique_ptr<DrawingSurface> acquire_surface() override;
    bool present(const DrawingSurface& surface) override;
    std::vector<std::uint32_t> snapshot() const override;
    void shutdown() noexcept override;
    void set_geometry(Geometry geometry) noexcept { geometry_ = geometry; }

private:
    Geometry geometry_{800u, 600u, 800u * 4u, PixelFormat::xrgb8888};
    std::vector<std::uint32_t> framebuffer_;
};

class BufferSurface final : public DrawingSurface {
public:
    explicit BufferSurface(Geometry geometry);
    void clear(std::uint32_t color) override;
    void fill_rect(std::uint32_t x, std::uint32_t y, std::uint32_t w, std::uint32_t h, std::uint32_t color) override;
    const std::vector<std::uint32_t>& pixels() const override { return pixels_; }
    Geometry geometry() const noexcept { return geometry_; }

private:
    Geometry geometry_;
    std::vector<std::uint32_t> pixels_;
};

std::unique_ptr<GraphicsBackend> choose_backend(Config::GraphicsChoice choice, EventQueue& events, std::string& reason);

} // namespace domios
