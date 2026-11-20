#include <cephyr/Cephyr.hpp>

#include <iostream>

int main()
{
    auto result = cephyr::compile({"-E", "-P", "-x", "c", "-", "-o", "-"},
                                  "#define VALUE 42\nint value = VALUE;\n");
    if (!result) {
        std::cerr << result.diagnostics();
        return 1;
    }
    const auto output = result.output();
    return output.find("int value") != std::string_view::npos &&
           output.find("42") != std::string_view::npos &&
           output.find("VALUE") == std::string_view::npos ? 0 : 1;
}
