#pragma once

#include "backend.hpp"
#include "control_plane.hpp"

namespace domios {

class PresentationPlane {
public:
    explicit PresentationPlane(GraphicsBackend& backend);
    bool render(const ControlPlane& control, EventQueue& events, std::string& error);
    GraphicsBackend& backend() noexcept { return backend_; }

private:
    GraphicsBackend& backend_;
};

} // namespace domios

