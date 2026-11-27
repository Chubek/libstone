#include <TTyUtils/TTyUtils-Theme.hpp>
#include <cassert>
#include <fstream>
#include <unistd.h>

int main() {
    char filename[] = "/tmp/ttyutils-theme-test.XXXXXX";
    const auto fd = ::mkstemp(filename);
    assert(fd >= 0);
    ::close(fd);
    const std::filesystem::path path{filename};
    std::ofstream{path} << "dark = true\npalette.accent = \"#ff0000\"\n"
                          "menu.selected.fg = \"$accent\"\nmenu.bg = \"#333333\"\n"
                          "[light.menu.selected]\nfg = \"#0000ff\"\n";
    auto theme = ttyutils::theme::load(path);
    std::filesystem::remove(path);
    assert(theme.dark_mode());
    using ttytk::color::Rgb;
    assert((theme.role("menu.selected") == ttyutils::theme::Style{Rgb{255, 0, 0}, Rgb{51, 51, 51}, 0}));
    assert((theme.role("menu.selected", 256).foreground == Rgb{255, 0, 0}));
    assert(!theme.apply_background("10;rgb:ffff/ffff/ffff"));
    assert(!theme.apply_background("11;rgb:ffff/ffff/ffff/ffff"));
    assert(theme.apply_background("11;rgb:ffff/ffff/ffff"));
    assert(!theme.dark_mode());
    assert((theme.role("menu.selected").foreground == Rgb{0, 0, 255}));
    assert(theme.apply_background("11;rgb:0/0/0") && theme.dark_mode());
    return 0;
}
