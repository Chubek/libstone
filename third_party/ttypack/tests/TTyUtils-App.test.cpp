#include <TTyUtils/TTyUtils-App.hpp>
#include <cassert>
#include <cstdlib>
#include <string>

struct InputWidget final : ttyutils::ui::Widget {
    void paint(ttyutils::ui::Canvas& canvas) override { canvas.text(rect().x, rect().y, label); }
    auto handle(const ttytk::input::Event& event) -> bool override {
        const auto* key = std::get_if<ttytk::input::Key>(&event);
        if (key && key->codepoint == U'x') {
            ++keys;
            return true;
        }
        return false;
    }
    std::string label{"initial"};
    int keys{};
};

int main() {
    using namespace std::chrono_literals;
    using namespace ttyutils;
    {
        app::App application({.input_fd = -1, .output_fd = -1});
        auto root = std::make_unique<InputWidget>();
        auto* target = root.get();
        application.set_root(std::move(root));
        loop::post(application.loop(), [&] {
            assert(application.screen().output() == "initial");
            assert(application.dispatch(ttytk::input::Key{U'\t'}));
            assert(target->focused());
            assert(application.dispatch(ttytk::input::Key{U'x'}));
            target->label = "changed";
            target->invalidate(target->rect());
            (void)application.loop().after(1ms, [&] {
                assert(application.screen().output() == "changed");
                application.quit(7);
            });
        });
        assert(application.run() == 7 && target->keys == 1);
    }

    const auto master = ::posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK);
    assert(master >= 0);
    const auto granted = ::grantpt(master);
    const auto unlocked = ::unlockpt(master);
    assert(granted == 0 && unlocked == 0);
    const auto slave = ::open(::ptsname(master), O_RDWR | O_NOCTTY);
    assert(slave >= 0);
    const auto env = ::setenv("TERM", "xterm", 1);
    assert(env == 0);
    struct termios original {};
    auto status = ::tcgetattr(slave, &original);
    assert(status == 0);
    app::App application({.mouse = true, .input_fd = slave, .output_fd = slave});
    {
        auto root = std::make_unique<InputWidget>();
        auto* target = root.get();
        application.set_root(std::move(root));
        loop::post(application.loop(), [&] {
            struct termios active {};
            auto result = ::tcgetattr(slave, &active);
            assert(result == 0 && !(active.c_lflag & ICANON));
            const auto written = ::write(master, "x", 1);
            assert(written == 1);
        });
        (void)application.loop().after(40ms, [&] {
            assert(target->keys == 1);
            application.quit();
        });
        const auto result = application.run();
        assert(result == 0);
    }
    struct termios restored {};
    status = ::tcgetattr(slave, &restored);
    assert(status == 0 && restored.c_lflag == original.c_lflag &&
           restored.c_iflag == original.c_iflag && restored.c_oflag == original.c_oflag);
    std::string capture;
    char bytes[256];
    for (ssize_t size; (size = ::read(master, bytes, sizeof(bytes))) > 0;)
        capture.append(bytes, static_cast<std::size_t>(size));
    const auto enter = capture.find("\x1b[?1049h");
    const auto leave = capture.find("\x1b[?1049l");
    assert(enter != std::string::npos && leave != std::string::npos && enter < leave);
    assert(capture.find("\x1b[?1006h") < capture.find("\x1b[?1006l"));
    ::close(slave);
    ::close(master);
    return 0;
}
