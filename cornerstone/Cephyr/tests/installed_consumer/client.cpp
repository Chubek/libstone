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
    const auto mir = cephyr::emit_mir({"-"}, "int value(void) { return 42; }\n");
    if (!mir || mir.output().find("archon-mir 1\n") != 0 ||
        mir.output().find("\nfunction value ") == std::string_view::npos) {
        std::cerr << mir.diagnostics();
        return 1;
    }
    return output.find("int value") != std::string_view::npos &&
           output.find("42") != std::string_view::npos &&
           output.find("VALUE") == std::string_view::npos ? 0 : 1;
}
