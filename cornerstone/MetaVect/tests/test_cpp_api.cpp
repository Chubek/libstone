#undef NDEBUG
#include <cassert>
#include <string>
#include <string_view>

#include "MetaVect.hpp"

int main() {
    assert(std::string_view(metavect::version()) == "0.1.0");
    metavect::check(MV_STATUS_OK);
    try {
        metavect::check(MV_STATUS_NOT_IMPLEMENTED);
        assert(false);
    } catch (const metavect::error& e) {
        assert(std::string_view(e.what()) == "not implemented");
    }

    std::string svg;
    {
        std::string source =
            R"(start canvas(100,80) { let pos = 1; rect(pos,2,10,20,"#ff0000"); })"
            " trailing bytes";
        constexpr auto prefix_size = sizeof "start " - 1;
        const auto drawing = std::string_view(source).substr(prefix_size,
            source.find('}') + 1 - prefix_size);
        svg = metavect::compile(drawing);
        const auto eps = metavect::compile(drawing, MV_BACKEND_EPS);
        assert(eps.compare(0, sizeof "%!PS-Adobe-" - 1, "%!PS-Adobe-") == 0);
    }
    assert(svg.find("<svg") != std::string::npos);
    assert(svg.find("<rect") != std::string::npos);
    assert(svg.find('\0') == std::string::npos);

    try {
        (void)metavect::compile("canvas(100,80) {\n  @\n}");
        assert(false);
    } catch (const metavect::compile_error& e) {
        assert(e.status() == MV_STATUS_SYNTAX_ERROR);
        assert(e.diagnostic().offset == sizeof "canvas(100,80) {\n  " - 1);
        assert(e.diagnostic().line == 2 && e.diagnostic().column == 3);
        assert(e.diagnostic().message != nullptr);
        assert(std::string_view(e.what()) == e.diagnostic().message);
    }

    try {
        (void)metavect::compile(std::string_view{});
        assert(false);
    } catch (const metavect::compile_error& e) {
        assert(e.status() == MV_STATUS_SYNTAX_ERROR);
    }
}
