#pragma once

#include "backend.hpp"
#include "config.hpp"
#include "control_plane.hpp"
#include "presentation_plane.hpp"

namespace domios {

struct SplashFrame {
    Geometry geometry{};
    DitFormat format{DitFormat::unknown};
    std::vector<std::uint32_t> pixels;
};

class Bootstrap {
public:
    bool emit_dit_template(const std::filesystem::path& output, std::string& error) const;
    bool render_splash(SplashFrame& frame, std::string& error) const;
    bool run(std::string& error);
};

} // namespace domios
