# Aurocks native matcher helper

`aurocks-liblr` is an optional, build-time helper for the scannerless C99
parser generator in [`scripts/aurocks.pl`](scripts/aurocks.pl). It reads
one byte-oriented NFA and uses Firestone liblr to determinize and minimize it.

From the repository root, with CMake 3.20 or later and a C++17 compiler:

```sh
cmake -S cornerstone/Aurocks -B build/aurocks
cmake --build build/aurocks
ctest --test-dir build/aurocks --output-on-failure
```

If Perl 5.10 or later and a C compiler are present, the tests also generate,
compile, and run C99 parsers with and without the helper. Configure with
`-DBUILD_TESTING=OFF` to build only the helper.

Opt in at parser generation time:

```sh
perl cornerstone/Aurocks/scripts/aurocks.pl --liblr-helper "$PWD/build/aurocks/aurocks-liblr" \
    --no-strpool grammar.g > parser.c
cc -std=c99 -c parser.c
```

Generated parsers do not need the helper or Firestone at runtime. The parser
remains scannerless, and matchers use the longest prefix at the current input
position. The helper is used for regex terminals and `%skip` expressions that
don't use context assertions. Expressions containing `^`, `$`, `\b`, or `\B`
assertions still use the original engine. Without `--liblr-helper`, the
generator uses the original engine for every matcher.

## Helper protocol

Invoke `aurocks-liblr INPUT_PATH OUTPUT_PATH` with exactly two path arguments.
The input is ASCII whitespace-separated text, with unsigned decimal integers and
zero-based state IDs:

```text
N START
ACCEPT EDGE_COUNT [TARGET LABEL]...
...
```

There are exactly `N` state records in ID order. `ACCEPT` is `0` or `1`. Each
record has `EDGE_COUNT` pairs whose target must be in `[0, N)`. A label is `-`
for an epsilon transition, or exactly 64 lowercase hexadecimal digits for a
byte set. Hex digit `i` describes bytes `4*i` through `4*i+3`, from low to high
bit. For example, a label beginning with `1` followed by 63 zeros matches just
byte `0`, and one ending with `8` after 63 zeros matches just byte `255`. An
all-zero label matches no bytes.

The output starts with `N START` for the resulting DFA. Each of its `N` records
has the `ACCEPT` flag followed by exactly 256 integer targets, ordered by byte
value. A target of `-1` denotes an absent transition. An accepting start state
is valid in this protocol; the generated parser decides whether zero bytes
count as a match in a particular context.

The helper accepts at most 16 MiB of input, 16,384 NFA states, and 131,072 NFA
edges. It emits at most 16,384 minimized states. Subset construction can grow
exponentially even on small NFAs; where supported, the helper limits its CPU
time to 10 seconds and address space to 512 MiB (or the existing limit, if
lower). Errors produce a nonzero exit status; do not use the output after a
failed invocation.
