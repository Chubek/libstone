#include "Native.hpp"

#include <cassert>
#include <utility>

namespace {

template <class Fn>
void rejected(Fn fn, std::string_view diagnostic) {
    try {
        fn();
    } catch (const std::invalid_argument& error) {
        assert(std::string_view(error.what()).find(diagnostic) != std::string_view::npos);
        return;
    }
    assert(false && "invalid registration was accepted");
}

am8::NativeForm form(std::string name) {
    return {
        .name = std::move(name), .min_arity = 1, .max_arity = std::nullopt,
        .validate = {},
        .lower = [](const am8::Syntax& source) { return std::vector{source}; },
        .capabilities = {"example"},
    };
}

} // namespace

int main() {
    am8::NativeRegistry registry;
    assert(registry.find("demo-form") == nullptr);
    assert(&registry.add(form("demo-form")) == &registry);
    const char prefixed[] = "prefix:demo-form:suffix";
    const auto* entry = registry.find(std::string_view(prefixed + 7, 9));
    assert(entry && !entry->max_arity && !entry->validate);
    assert(entry->capabilities == std::vector<std::string>{"example"});
    registry.add(form("another_form"));
    assert(registry.find("demo-form") == entry);
    assert(registry.find("demo-forms") == nullptr);
    assert(registry.find("another_form")->lower(am8::Syntax{}).size() == 1);

    rejected([&] { registry.add(form("demo-form")); }, "duplicate native form");
    rejected([&] { registry.add(form("")); }, "invalid native form name");
    rejected([&] { registry.add(form("with space")); }, "invalid native form name");
    rejected([&] { registry.add(form("qualified.form")); }, "invalid native form name");
    rejected([&] {
        auto bad = form("bad");
        bad.max_arity = 0;
        registry.add(std::move(bad));
    }, "invalid arity range");
    rejected([&] {
        auto bad = form("bad");
        bad.lower = {};
        registry.add(std::move(bad));
    }, "requires a lowering callback");
    assert(registry.find("demo-form") == entry && registry.find("bad") == nullptr);
}
