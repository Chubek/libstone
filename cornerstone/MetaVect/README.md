# MetaVect — a Multi-Output Graphics Language

MetaVect compiles a small declarative drawing language to SVG (Scalable Vector
Graphics) and EPS (Encapsulated PostScript). Both backends consume the same
verified intermediate representation, MVIR.

## Build and run

From `cornerstone/MetaVect/`, use CMake 3.20 or newer and C11/C++17 compilers
on a POSIX platform or Windows:

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
./build/metavectc examples/shapes.mvct -o shapes.svg
./build/metavectc --backend eps examples/shapes.mvct -o shapes.eps
```

SVG is the default. Pass `-` as the input path for stdin. Output goes to stdout
when `-o` is omitted or `-o -` is given. Compilation errors are written to stderr
and do not overwrite the specified output file. Tests also check the D binding
on native POSIX builds with LDC or DMD installed, and validate EPS with
Ghostscript when available.

## Language

```text
canvas(120, 80) {
    // Earlier bindings can be reused in later statements.
    let margin = 8;
    let ink = "#173B52";
    rect(margin, margin, 104, 64, "#EFF6F8");
    circle(34, 40, 20, "#DB5946");
    line(56, 40, sub(120, margin), 40, ink, 4);
}
```

- One `canvas(width, height)` declaration encloses all statements. Dimensions
  must be positive and no larger than 2,147,483,647.
- `let name = expression;` binds an immutable number or color, in source
  order. Duplicate definitions and references to names not yet defined are
  errors. Identifiers are ASCII letters, digits, and underscores, and cannot
  start with a digit. `canvas` and `let` are reserved.
- Use signed decimal literals (optionally with an `e` or `E` exponent), quoted
  `"#RRGGBB"` colors, names, and the numeric calls `add(a,b)`, `sub(a,b)`,
  `mul(a,b)`, and `div(a,b)`. Colors accept either case. Non-finite literals
  and results, literals that underflow to zero, and division by zero are
  rejected. Expressions may nest up to 128 levels.
- `rect(x,y,width,height,color);` and `circle(cx,cy,radius,color);` draw filled
  shapes. Dimensions and radii must be nonnegative. `line(x1,y1,x2,y2,color,width);`
  draws a stroke with positive width.
- Shapes paint in source order, with the origin at the upper left and y
  increasing downward. Both formats clip to the canvas; neither paints a
  background. `//` starts a line comment.

See [the example](examples/shapes.mvct) for a complete source file.

## Embedding

Link the `metavect` CMake target. The C API in `MetaVect.h` takes a bounded
source buffer and returns owned bytes with a trailing NUL for convenience. Use
`mv_output_destroy` to release them. Diagnostics, when requested, carry a
zero-based byte offset, one-based line and column, and a static message.
`MetaVect.hpp` and `MetaVect.di` expose C++ and D interfaces to the same library.

```c
#include "MetaVect.h"
#include <stdio.h>

int main(void)
{
    const char src[] = "canvas(10,10) { circle(5,5,3,\"#0088cc\"); }";
    mv_output out = {0};
    mv_diagnostic diag = {0};
    mv_status status = mv_compile(src, sizeof src - 1, MV_BACKEND_SVG, &out, &diag);
    if (status != MV_STATUS_OK) {
        fprintf(stderr, "%zu:%zu: %s\n", diag.line, diag.column, diag.message);
        return 1;
    }
    fwrite(out.data, 1, out.size, stdout);
    mv_output_destroy(&out);
    return 0;
}
```

## Implementation and scope

`frontend/` parses into an AST, then checks names and types and lowers to a
flat MVIR draw list. `mvir/` verifies the list before and after optimization.
The rewrite pass removes only filled rectangles with a zero dimension and
filled circles with a zero radius: these cover no area, so removal preserves
drawing order and cannot increase output size. See [the backend notes](stdbe/README.md)
for SVG and EPS output conventions and EPS numeric limits.

The current compiler implements only the language described above. General
functions, recursion, System-F polymorphism, user modules, user-defined
rewrites, native hooks, and pluggable renderers are not implemented. The
experimental D compiler sketches in `compiler/` are not part of the build.
