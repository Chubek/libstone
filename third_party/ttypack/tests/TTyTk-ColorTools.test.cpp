#include <TTyTk/TTyTk-ColorTools.hpp>

#include <cassert>
#include <cstdint>

int main() {
    const ttytk::color::Rgb orange{0xFF, 0x87, 0x00};
    assert(ttytk::color::parse("#ff8700") == orange);
    assert((ttytk::color::parse("CYAN") == ttytk::color::Rgb{0, 255, 255}));
    assert((ttytk::color::parse(" Light Sky Blue ") == ttytk::color::Rgb{135, 206, 250}));
    assert((ttytk::color::parse("grey50") == ttytk::color::Rgb{127, 127, 127}));
    assert((ttytk::color::parse("green") == ttytk::color::Rgb{0, 255, 0}));
    assert((ttytk::color::parse("#abc") == ttytk::color::Rgb{160, 176, 192}));
    assert((ttytk::color::parse("rgb:a/bb/cccc") == ttytk::color::Rgb{170, 187, 204}));
    assert(!ttytk::color::parse("rgb:f/0/0/0"));
    assert(!ttytk::color::parse("#ffff"));
    assert(!ttytk::color::parse("not-a-colour"));
    assert(ttytk::color::to_256({0, 0, 0}) == 0);
    assert(ttytk::color::to_16({255, 255, 255}) == 15);
    assert(ttytk::color::to_256(orange) == 208);
    assert(ttytk::color::from_256(208) == orange);
    assert((ttytk::color::from_256(232) == ttytk::color::Rgb{8, 8, 8}));
    assert(ttytk::color::from_16(0x1f) == ttytk::color::from_256(15));
    assert(ttytk::color::distance(orange, orange) == 0.0);
    assert(ttytk::color::luminance({255, 255, 255}) > 0.99);
    assert(ttytk::color::luminance({128, 128, 128}) < 0.22);
}
