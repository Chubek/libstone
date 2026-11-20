#include "MetaVect.h"

#include <stdio.h>
#include <string.h>

#define CHECK(expr) do {                                               \
    if (!(expr)) {                                                     \
        fprintf(stderr, "line %d: check failed: %s\n", __LINE__, #expr); \
        return 1;                                                     \
    }                                                                 \
} while (0)

static int test_output(mv_backend backend, const char *expected)
{
    char source[] =
        "canvas(10.5, 6.25) {\n"
        "  let ink = \"#aB3cD4\";\n"
        "  rect(9, 9, 0, 1, \"#FFFFFF\");\n"
        "  rect(-1, 0.5, 3, 2, ink);\n"
        "  circle(9, 9, -0, \"#FFFFFF\");\n"
        "  circle(2, 1.5, 1, \"#010203\");\n"
        "  rect(9, 9, 2, -0, \"#FFFFFF\");\n"
        "  line(-0, 1, -0, 1, \"#Ff0080\", 0.5);\n"
        "}\n";
    mv_output first, second;
    mv_diagnostic diag = {.message = "old message"};

    CHECK(mv_compile(source, sizeof source - 1, backend, &first, &diag) == MV_STATUS_OK);
    CHECK(diag.message == NULL && diag.offset == 0 && diag.line == 0 && diag.column == 0);
    CHECK(mv_compile(source, sizeof source - 1, backend, &second, NULL) == MV_STATUS_OK);
    CHECK(first.data != NULL && second.data != NULL && first.data != second.data);
    CHECK(first.size == strlen(expected) && second.size == first.size);

    /* Neither result is a source slice, and destroying one leaves the other intact. */
    memset(source, ' ', sizeof source);
    CHECK(memcmp(first.data, expected, first.size + 1) == 0);
    mv_output_destroy(&first);
    CHECK(first.data == NULL && first.size == 0);
    mv_output_destroy(&first);
    CHECK(memcmp(second.data, expected, second.size + 1) == 0);
    mv_output_destroy(&second);
    return 0;
}

static int test_eps_numbers(void)
{
    static const char valid[] =
        "canvas(0.000030517578125, 2147483647) {\n"
        "  rect(-1e20, 1e-10, 1, 2, \"#ABCDEF\");\n"
        "}\n";
    static const char too_large[] =
        "canvas(8, 8) {\n"
        "  rect(0, 0, 1, 2, \"#FFFFFF\");\n"
        "  line(0, 0, 1e100, 1, \"#010203\", 1);\n"
        "}\n";
    static const char too_small[] = "canvas(1e-100, 1) {}";
    static const char empty[] =
        "canvas(8, 8) { rect(1e100, 0, 0, 2, \"#FFFFFF\"); }\n";
    mv_output output;
    mv_diagnostic diag;

    CHECK(mv_compile(valid, sizeof valid - 1, MV_BACKEND_EPS, &output, &diag) == MV_STATUS_OK);
    CHECK(strstr(output.data, "%%BoundingBox: 0 0 1 2147483647\n") != NULL);
    CHECK(strstr(output.data, "%%HiResBoundingBox: 0 0 0.000030517578125 2147483647\n") != NULL);
    CHECK(strstr(output.data, "-1E+20 ") != NULL);
    CHECK(strstr(output.data, " 1 2 rectfill\n") != NULL);
    mv_output_destroy(&output);

    CHECK(mv_compile(too_large, sizeof too_large - 1, MV_BACKEND_EPS, &output, &diag) ==
          MV_STATUS_BACKEND_ERROR);
    CHECK(output.data == NULL && output.size == 0);
    CHECK(diag.line == 3 && diag.column == 3 && diag.message != NULL);
    CHECK(strstr(diag.message, "EPS") != NULL);
    CHECK(mv_compile(too_small, sizeof too_small - 1, MV_BACKEND_EPS, &output, &diag) ==
          MV_STATUS_BACKEND_ERROR);
    CHECK(output.data == NULL && output.size == 0 && diag.line == 1);
    CHECK(mv_compile(too_large, sizeof too_large - 1, MV_BACKEND_SVG, &output, &diag) ==
          MV_STATUS_OK);
    CHECK(strstr(output.data, "stroke=\"#010203\"") != NULL);
    mv_output_destroy(&output);

    /* Only surviving geometry is subject to a backend's numeric limits. */
    CHECK(mv_compile(empty, sizeof empty - 1, MV_BACKEND_EPS, &output, &diag) == MV_STATUS_OK);
    CHECK(strstr(output.data, "rectfill") == NULL);
    mv_output_destroy(&output);
    return 0;
}

int main(void)
{
    static const char svg[] =
        "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"10.5\" height=\"6.25\""
        " viewBox=\"0 0 10.5 6.25\">\n"
        "  <defs>\n"
        "    <clipPath id=\"mv-viewport\">\n"
        "      <rect width=\"10.5\" height=\"6.25\"/>\n"
        "    </clipPath>\n"
        "  </defs>\n"
        "  <g clip-path=\"url(#mv-viewport)\">\n"
        "    <rect x=\"-1\" y=\"0.5\" width=\"3\" height=\"2\" fill=\"#ab3cd4\"/>\n"
        "    <circle cx=\"2\" cy=\"1.5\" r=\"1\" fill=\"#010203\"/>\n"
        "    <line x1=\"-0\" y1=\"1\" x2=\"-0\" y2=\"1\""
        " stroke-width=\"0.5\" stroke-linecap=\"butt\" stroke=\"#ff0080\"/>\n"
        "  </g>\n"
        "</svg>\n";
    static const char eps[] =
        "%!PS-Adobe-3.0 EPSF-3.0\n"
        "%%BoundingBox: 0 0 11 7\n"
        "%%HiResBoundingBox: 0 0 10.5 6.25\n"
        "%%LanguageLevel: 2\n"
        "%%EndComments\n"
        "gsave\n"
        "0 6.25 translate\n"
        "1 -1 scale\n"
        "0 0 10.5 6.25 rectclip\n"
        "0 setlinecap\n"
        "[] 0 setdash\n"
        "171 255 div 60 255 div 212 255 div setrgbcolor\n"
        "-1 0.5 3 2 rectfill\n"
        "1 255 div 2 255 div 3 255 div setrgbcolor\n"
        "newpath 2 1.5 1 0 360 arc closepath fill\n"
        "255 255 div 0 255 div 128 255 div setrgbcolor\n"
        "newpath -0 1 moveto -0 1 lineto 0.5 setlinewidth stroke\n"
        "grestore\n"
        "%%EOF\n";

    if (test_output(MV_BACKEND_SVG, svg) || test_output(MV_BACKEND_EPS, eps))
        return 1;
    return test_eps_numbers();
}
