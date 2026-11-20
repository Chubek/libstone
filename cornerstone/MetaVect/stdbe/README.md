# SVG and EPS backends

Both backends draw in source order on a transparent, top-left-origin canvas
with increasing Y downward. They clip all marks to the canvas. Rectangles and
circles are filled without a stroke; lines use a solid stroke with butt caps.
The verified MVIR pass removes zero-area fills before either backend sees
them, but does not remove lines with coincident endpoints.

SVG uses a canvas-sized view box and clip path, and encodes RGB colors as
lowercase `#rrggbb`. EPS is Level 2 with viewport bounds in both
`%%BoundingBox` (rounded outward) and `%%HiResBoundingBox` (decimal). It flips
the Y axis inside a saved graphics state, expresses RGB channels as fractions
of 255, and does not call `showpage`.

Numbers are formatted independently of the process locale. SVG emits the
finite MVIR values as round-trip decimals; a viewer may use less precision.
EPS uses PostScript numbers (uppercase `E` in exponents), with less range and
precision than MVIR doubles. The EPS backend accepts zero and magnitudes from
`1.1754943508222875e-38` through `1e38` and reports a backend error for other
operands instead of emitting overflow or silently underflowing to zero.
Renderers and output devices can impose further geometry or resolution limits.
