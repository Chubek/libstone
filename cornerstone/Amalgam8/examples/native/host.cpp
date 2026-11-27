#include "Amalgam8Base.hpp"

#include <charconv>
#include <iostream>

namespace {

int small_integer(const am8::Syntax& source) {
    const auto text = am8::atom(source);
    int value{};
    const auto end = text.data() + text.size();
    const auto result = std::from_chars(text.data(), end, value);
    if (source.quoted || result.ec != std::errc{} || result.ptr != end ||
        value < -1000 || value > 1000)
        am8::fail(source, "expected an integer between -1000 and 1000");
    return value;
}

am8::NativeForm sum_form() {
    return {
        .name = "host-sum",
        .min_arity = 2,
        .max_arity = 2,
        .validate = [](const am8::Syntax& form) {
            for (std::size_t i = 1; i < form.items.size(); ++i)
                small_integer(form.items[i]);
        },
        .lower = [](const am8::Syntax& form) {
            const auto sum = small_integer(form.items[1]) + small_integer(form.items[2]);
            const auto token = [&](std::string text) {
                return am8::Syntax{true, std::move(text), {}, false, form.span, {}, {}};
            };
            const auto list = [&](std::vector<am8::Syntax> items) {
                return am8::Syntax{false, {}, std::move(items), false, form.span, {}, {}};
            };
            return std::vector{
                list({token("fn"), token("int"), token("native_answer"), list({}),
                      list({token("block"), list({token("return"), token(std::to_string(sum))})})})
            };
        },
        .capabilities = {"integer-sum"},
    };
}

} // namespace

int main(int argc, char **argv) {
    if (argc != 2) {
        std::cerr << "usage: native-host INPUT.am8\n";
        return 2;
    }
    try {
        am8::Options options;
        options.native.add(sum_form());
        std::cout << am8::compile_file(argv[1], options).source;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
