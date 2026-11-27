#include <TTyUtils/TTyUtils-HexView.hpp>
#include <array>
#include <cassert>
#include <limits>

int main() {
    using namespace ttyutils;
    hexview::HexView view;
    std::vector<std::uint8_t> bytes(32, 0);
    bytes[0] = 1; bytes[1] = 2; bytes[2] = 3; bytes[3] = 0xfa; bytes[17] = 'a'; bytes[18] = 'b';
    view.set_data(std::move(bytes));
    assert(hexview::Inspector::u16(view.data(), 0) == 513);
    assert(hexview::Inspector::u32(view.data(), 0) == 0xfa030201);
    assert(hexview::Inspector::u32(view.data(), 0, std::endian::big) == 0x010203fa);
    assert(!hexview::Inspector::read(view.data(), std::numeric_limits<std::size_t>::max(), 2));

    view.set_rect({0, 0, 100, 1});
    const auto match = view.find(std::array<std::uint8_t, 2>{'a', 'b'});
    assert(match == 17 && view.cursor_offset() == 17 && view.offset() == 1);
    const theme::Style annotated{.background = ttytk::color::Rgb{10, 20, 30}};
    view.annotate({17, 19}, "bytes", annotated);
    ui::Canvas canvas{{0, 0, 100, 1}};
    view.render(canvas);
    assert(canvas.texts()[3].value == "61" && canvas.texts()[4].value == "a");
    assert(canvas.texts()[3].style == canvas.texts()[4].style);
    assert(canvas.texts()[3].style.background == annotated.background);
    assert(canvas.texts()[3].style.attributes != annotated.attributes);
}
