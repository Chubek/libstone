#include <TTyTk/TTyTk-Core.hpp>

#include <cassert>

int main() {
    constexpr auto v = ttytk::version();
    static_assert(v.size() == 3);

    ttytk::Result<int> value{42};
    assert(value);
    assert(value.value() == 42);

    ttytk::Result<int> error{ttytk::Error{7, "failure"}};
    assert(!error);
    assert(error.error().code == 7);

    ttytk::Result<void> ok;
    ok.value();
    ttytk::Result<void> failure{ttytk::Error{1, "failure"}};
    assert(!failure);
    bool threw = false;
    try {
        failure.value();
    } catch (const std::bad_variant_access&) {
        threw = true;
    }
    assert(threw);
}
