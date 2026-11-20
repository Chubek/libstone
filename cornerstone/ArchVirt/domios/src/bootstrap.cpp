#include "bootstrap.hpp"

#include "dit.hpp"

#include <fstream>

namespace domios {

bool Bootstrap::emit_dit_template(const std::filesystem::path& output, std::string& error) const {
    std::ofstream out(output, std::ios::binary);
    if (!out) {
        error = "failed to open dit template output";
        return false;
    }
    const auto page = make_default_dit();
    const auto image = serialize_dit(page);
    out.write(reinterpret_cast<const char*>(image.bytes.data()), static_cast<std::streamsize>(image.bytes.size()));
    return static_cast<bool>(out);
}

bool Bootstrap::render_splash(SplashFrame& frame, std::string& error) const {
    std::string source;
    auto config = load_config(source, nullptr);
    EventQueue events(32);

    // The host frontend is a pixel display. Keep the firmware's configured
    // backend for normal operation, but use the pixel backend for the
    // bootstrap splash when a text-only VGA backend is selected.
    auto backend = choose_backend(config.graphics, events, error);
    if (!backend) {
        return false;
    }
    if (backend->format() == DitFormat::vga_text) {
        backend = choose_backend(Config::GraphicsChoice::virtio_gpu, events, error);
        if (!backend) {
            return false;
        }
    }

    ControlPlane control(std::move(config));
    if (!control.initialize(events, error)) {
        return false;
    }
    control.set_phase(Phase::splash);

    PresentationPlane presentation(*backend);
    if (!presentation.render(control, events, error)) {
        return false;
    }

    frame.geometry = backend->geometry();
    frame.format = backend->format();
    frame.pixels = backend->snapshot();
    if (frame.geometry.width == 0 || frame.geometry.height == 0 ||
        frame.geometry.pitch == 0 || frame.pixels.empty()) {
        error = "splash backend produced an empty frame";
        return false;
    }
    return true;
}

bool Bootstrap::run(std::string& error) {
    SplashFrame frame;
    return render_splash(frame, error);
}

} // namespace domios
