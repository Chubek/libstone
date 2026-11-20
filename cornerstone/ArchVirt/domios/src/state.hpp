#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace domios {

enum class Phase : std::uint8_t {
    bootstrap = 0,
    config = 1,
    plugins = 2,
    lua = 3,
    splash = 4,
    shell = 5,
    browser = 6,
    shutdown = 7,
    fault = 255
};

enum class BackendKind : std::uint8_t {
    none = 0,
    vga = 1,
    virtio_gpu = 2
};

enum class PixelFormat : std::uint8_t {
    unknown = 0,
    xrgb8888 = 1,
    bgrx8888 = 2,
    rgb565 = 3,
    vga_text = 4
};

struct Geometry {
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t pitch{};
    PixelFormat format{PixelFormat::unknown};
};

struct Config {
    enum class GraphicsChoice {
        auto_backend,
        vga,
        virtio_gpu
    };

    GraphicsChoice graphics{GraphicsChoice::auto_backend};
    std::vector<std::string> plugin_ignore_list;
    std::vector<std::filesystem::path> extra_extension_dirs;
    std::vector<std::filesystem::path> extra_program_dirs;
    std::vector<std::filesystem::path> extra_plugin_dirs;
    std::string theme{"default"};
    std::string locale{"C"};
    std::optional<std::uint64_t> dit_base_override;
};

struct ImageManifestVersion {
    std::uint16_t major{};
    std::uint16_t minor{};
    std::uint16_t patch{};
};

struct ImageManifest {
    std::uint32_t image_format_version{};
    ImageManifestVersion plugin_abi{};
    std::uint64_t bootstrap_offset{};
    std::uint64_t dit_offset{};
    std::uint64_t logo_offset{};
    std::uint64_t background_offset{};
    std::uint64_t lua_offset{};
    std::uint64_t manifest_offset{};
};

struct Diagnostics {
    std::string message;
    std::string file;
};

} // namespace domios

