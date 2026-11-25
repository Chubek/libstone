# Control-flow analysis with `std.flowgraph`

Import `std.flowgraph` to run Wetstone's forward and backward data-flow
worklists on a control-flow graph (CFG) supplied by your semantic or lowering
actions. No grammar tokens or global language state are added.

```text
.language "FlowgraphExample"
.import std.flowgraph as FG

.rule Start
'syntax: EOF
'semantics:
let blocks: Vec<%FG->Block> = [
    %FG->Block([1], [%FG->Step([], [7])]),
    %FG->Block([], [%FG->Step([7], [])])
];
let live = FG:liveness(blocks, 0, 100);
let defs = FG:reachingDefinitions(blocks, 0, 100);
if live[0].Out == [7] && defs[1].In == [%FG->Definition(0, 0, 7)] {
    return 42;
}
return 0;
.end
```

## Graphs and results

* `%FG->Block(successors, steps)` has `Successors: Vec<Int>` and
  `Steps: Vec<%FG->Step>`. Block IDs are zero-based vector indices. Each block
  has zero, one, or two distinct successors. A block with no successors is an
  exit; a block may have a self-edge.
* `%FG->Step(uses, defines)` has `Uses: Vec<Int>` and `Defines: Vec<Int>`.
  Steps execute in order within the block, with uses before definitions at
  each step. Variable IDs are in `0..4294967294`; repeated IDs within a list
  do not change the result. The same variable may be defined in multiple
  steps and blocks.
* `FG:liveness(blocks: Vec<%FG->Block>, entry: Int, limit: Int) -> Vec<%FG->Live>`
  is backward may-analysis: a variable is live if it can be used later
  before another definition along a path. Exit live-out is empty.
  The `In` and `Out` fields hold ascending `Vec<Int>` sets.
* `FG:reachingDefinitions(blocks: Vec<%FG->Block>, entry: Int, limit: Int) -> Vec<%FG->Reaching>`
  is forward may-analysis: definitions reach a point if they can arrive
  there without being overwritten. Each `%FG->Definition` has `Block`,
  `Step`, and `Value` integer fields, where `Step` is the zero-based index
  within `Block`. The `In` and `Out` sets are sorted by `(Block, Step, Value)`.
  Initial entry has no definition facts; add a preheader step defining any
  inputs that should have specific recorded definition sites.
* `In` and `Out` describe block boundaries. Each result vector has one record
  per input block, in the same order. `Reachable` is false and both sets are
  empty for unreachable blocks. An unreachable predecessor cannot contribute
  facts at a reachable join. A backedge can contribute facts at entry on
  later visits.

All supplied edges are considered possible; these analyses do not interpret
branch conditions, alias memory, or infer use/def information from an `.ir`
dialect. To represent uses that occur only along a particular edge (such as
incoming Phi operands), place those uses in a separate block on that edge
rather than treating them as uses at the join.

The graph must have 1–512 blocks. All edges, even in unreachable blocks,
are validated. The encoded request may contain up to 8192 integer words:
two header words, two count words per block, one per successor, and two
counts plus one per use or definition per step. Both analyses accept a
`limit` of `0..100000` Wetstone block-transfer evaluations. Invalid graph data
or an out-of-range limit raises `!FG->InvalidGraph`; a solver that uses up its
budget before converging raises `!FG->Incomplete`, never returning partial
facts. An encoded result exceeding 32768 integer words also raises
`!FG->Incomplete`: each result uses three header words per block
and one word per liveness fact or three per reaching definition. LLSynth's
ordinary action budget also applies to encoding and decoding.

## Build and run

The adapter uses Wetstone's C++20 headers and LLSynth's existing
[scalar C ABI](interop.md). Build the module-specific shared library on Linux:

```sh
cmake -S cornerstone/LLSynth/stdlib/flowgraph -B build/llsynth-flowgraph
cmake --build build/llsynth-flowgraph
```

If Wetstone is installed separately, configure with
`-DLLSYNTH_WETSTONE_DIR=/path/to/wetstone`.

Generating or checking a specification does not require the adapter to be
built. At runtime a Python tool looks up `libllsynth_flowgraph.so` on its
first analysis call:

```sh
perl cornerstone/LLSynth/scripts/llsynth.pl --mode=interpret -T python3 \
    -o example.py example.llsynth
LD_LIBRARY_PATH="$PWD/build/llsynth-flowgraph${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
    python3 example.py </dev/null
```

For a C11 or C99 tool, link against CMake target `llsynth_flowgraph` or link
the library explicitly. For example:

```sh
perl cornerstone/LLSynth/scripts/llsynth.pl --mode=interpret -T c11 \
    -o example.c example.llsynth
cc -std=c11 example.c -L build/llsynth-flowgraph -lllsynth_flowgraph -lm -o example
LD_LIBRARY_PATH="$PWD/build/llsynth-flowgraph${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
    ./example </dev/null
```

As for other native C imports, generated C++ tools are not supported.
There is no external process per analysis and no requirement to install Perl
or the original library specification with the generated tool.

Focused end-to-end tests build the real adapter and exercise generated tools:

```sh
python3 cornerstone/LLSynth/tests/stdlib/test_flowgraph.py
```
