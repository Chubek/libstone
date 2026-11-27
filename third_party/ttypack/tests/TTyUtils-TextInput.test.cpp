#include <TTyUtils/TTyUtils-TextInput.hpp>
#include <cassert>
int main() {
    ttyutils::ui::TextInput input;
    input.set_mask({"99/99"});
    input.set_validator(ttyutils::ui::Validator::regex("12/[0-9][0-9]"));
    std::string submitted;
    input.on_submit([&](auto value) { submitted = std::move(value); });
    assert(input.handle(ttytk::input::Paste{"12/3"}));
    assert(!input.valid());
    assert(input.handle(ttytk::input::Key{U'x'}));
    assert(input.text() == "12/3");
    assert(input.handle(ttytk::input::Key{U'4'}));
    assert(input.handle(ttytk::input::Key{U'\r'}));
    assert(submitted == "12/34");
    input.set_rect({0, 0, 3, 1});
    ttyutils::ui::Canvas canvas{{0, 0, 3, 1}};
    input.render(canvas);
    assert(canvas.texts().size() == 1 && canvas.texts().front().value == "/34");

    ttyutils::ui::PasswordInput password;
    password.set_text("A\u0301猫");
    assert(password.display_text() == "**");
    password.set_reveal_last(true);
    assert(password.display_text() == "*猫");
}
