#include <TTyUtils/TTyUtils-ImageView.hpp>
#include <cassert>

int main() {
    using namespace ttyutils;
    imgview::ImageView view;
    const ttytk::color::Rgb red{255, 0, 0};
    const ttytk::color::Rgb blue{0, 0, 255};
    assert(view.set_image({1, 2, {red, blue}}));
    assert(!view.set_image({1, 2, {red}}));
    view.set_rect({0, 0, 1, 1});
    ui::Canvas canvas{{0, 0, 1, 1}};
    view.render(canvas);
    assert(canvas.texts().size() == 1);
    assert(canvas.texts().front().value == "▀");
    assert(canvas.texts().front().style.foreground == red);
    assert(canvas.texts().front().style.background == blue);
    view.set_fit(imgview::FitMode::Cover);
    assert(view.fit() == imgview::FitMode::Cover);
    view.set_backend(imgview::Backend::Kitty);
    assert(view.protocol_bytes().starts_with("\x1b_G"));
    ui::Canvas native{{0, 0, 1, 1}};
    view.render(native);
    assert(native.texts().empty());
    view.set_backend(imgview::Backend::Iterm2);
    assert(view.protocol_bytes().starts_with("\x1b]1337;File="));
}
