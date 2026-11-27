#include <TTyTk/TTyTk-ThemeTools.hpp>
#include <cassert>

int main() {
    namespace theme = ttytk::theme;
    const auto dark = theme::builtin("solarized-dark");
    const auto light = theme::load("solarized-light");
    assert(dark.name == "solarized-dark" && light && light->background != dark.background);
    assert(!theme::load("x") && theme::builtin("x").name == "default");
    const auto control = theme::apply(dark);
    assert(control.starts_with("\x1b]4;0;rgb:07/36/42;1;rgb:dc/32/2f;"));
    assert(control.ends_with("\x1b\\\x1b]10;rgb:83/94/96\x1b\\\x1b]11;rgb:00/2b/36\x1b\\"));

    const auto source = R"(scheme: "Example" # comment
author: Test
palette:
  base00: '010203'
  base01: 111111
  base02: 222222
  base03: 333333
  base04: 444444
  base05: "#aabbcc"
  base06: 666666
  base07: 777777
  base08: 888888
  base09: 999999
  base0A: aaaaaa
  base0B: bbbbbb
  base0C: cccccc
  base0D: dddddd
  base0E: eeeeee
  base0F: ffffff
)";
    const auto custom = theme::load(source);
    assert(custom && custom->name == "Example");
    assert((custom->background == ttytk::color::Rgb{1, 2, 3}));
    assert((custom->foreground == ttytk::color::Rgb{170, 187, 204}));
    assert((custom->palette[1] == ttytk::color::Rgb{136, 136, 136}));
    assert(!theme::load(std::string(source) + "\nbase0F: ffffff\n"));
    assert(!theme::load("name: missing colors\nbase00: 123456\n"));
}
