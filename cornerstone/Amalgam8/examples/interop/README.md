# Native interoperability example

## Design

Amalgam8 recognizes audited native component names separately from legacy
family-relative paths. A named dependency identifies a public header and its
build requirements; it never guesses a host ABI, automatically enables a
CMake target, or loads a shared object. A recognized but unavailable named
integration produces a source-located diagnostic. An explicit legacy path
remains an include, with the historical C++ guard for C++ header suffixes.

[`NativePipeline.am8`](NativePipeline.am8) checks the real XBind target
inventory, passes the input to Moonstone's bounded S-expression parser, then
uses the list arity in a Firestone `BigInt` calculation above the 64-bit
unsigned range. The structured functions call explicit typed `extern`
declarations. The XBind adapter accepts C11 or C++20 consumers and requires
`XBind::xbind`. The Moonstone adapter uses a C-compatible signature but its
inline implementation requires a C++20 consumer. The full pipeline is C++20,
not a C linkable artifact. Neither adapter loads a shared library or executes
an arbitrary foreign call.

The parser adapter accepts a NUL-terminated string. It returns a list's arity
on success, `-1` for null, invalid or non-list input, `-2` for byte/node/depth
limits, and `-3` for other exceptions. No C++ exception crosses the adapter
boundary. This is syntactic parsing, not a safety check. The CLI returns `1`
for an unknown XBind target, `2` for parsing failure, `3` unless the result has
three elements, and `64` for incorrect argument count.

From the Libstone checkout root, make a working copy of the example:

```sh
cp cornerstone/Amalgam8/examples/interop/NativePipeline.am8 ./NativePipeline.am8
amalgam8 --cli --deps ./NativePipeline.am8
cc -std=c11 -c tourchstone/xbind/xbind.c -o xbind.o
c++ -std=c++20 -I . NativePipeline.cpp xbind.o -o native-pipeline
./native-pipeline lua '(one two three)'
```

The XBind source object in this command can instead be supplied by linking the
`XBind::xbind` CMake target. The emitted native includes use the checkout-relative
spelling (as do legacy `(use family path)` declarations), so pass the checkout
root as an include directory.

[`OptionalTarget.am8`](OptionalTarget.am8) intentionally fails to resolve the
named `tourchstone:dyninvoke` integration. The error points to the declaration
and identifies the `Tourchstone::dyninvoke` target, its enabling option and the
active qBFD dependency. This is not a probe of the host build; callers
managing their own native build can still use the explicit source header path.

From the checkout root, run the generator and native regression tests with
`python3 cornerstone/Amalgam8/tests/W7.py` (optional `--cc` and `--cxx`).
