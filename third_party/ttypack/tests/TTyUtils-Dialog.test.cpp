#include <TTyUtils/TTyUtils-Dialog.hpp>
#include <cassert>
using namespace ttyutils::ui;

struct Task {
    struct promise_type {
        auto get_return_object() -> Task { return Task{std::coroutine_handle<promise_type>::from_promise(*this)}; }
        auto initial_suspend() -> std::suspend_never { return {}; }
        auto final_suspend() noexcept -> std::suspend_always { return {}; }
        void return_void() {}
        void unhandled_exception() { std::terminate(); }
    };
    std::coroutine_handle<promise_type> coroutine;
    ~Task() { coroutine.destroy(); }
};

auto watch(Dialog& dialog, DialogResult& value) -> Task { value = co_await dialog; }

int main() {
    auto dialog = confirm("Title", "Question?");
    dialog->set_rect({0, 0, 14, 5});
    dialog->open();
    assert(FocusScope::active() == dialog.get());
    Canvas canvas(dialog->rect());
    dialog->render(canvas);
    assert(canvas.texts().back().value == "Question?");
    int finished = 0;
    dialog->on_result([&](DialogResult value) { assert(value == DialogResult::No); ++finished; });
    auto awaited = DialogResult::None;
    auto waiting = watch(*dialog, awaited);
    assert(awaited == DialogResult::None);
    assert(dialog->handle(ttytk::input::Key{U'\x1b'}));
    assert(awaited == DialogResult::No && dialog->result() == awaited && FocusScope::active() == nullptr);
    dialog->accept();
    assert(finished == 1);
    auto input = prompt("Title", "Name?", "initial");
    assert(input->input_text() == "initial");
}
