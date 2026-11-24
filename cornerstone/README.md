# Cornerstone

Cornerstone contains standalone language and toolchain generators.

| Project | Entry point | Purpose |
| --- | --- | --- |
| [Aurocks](Aurocks/) | `cmake -S cornerstone/Aurocks -B build-aurocks` | Native generator for scannerless C99 parsers, with Firestone LibLR analysis, a linked LibGLR backend, and sandboxed Lua extensions. |
| [ASDL](ASDL/) | [`ASDL/scripts/asdl.pl`](ASDL/scripts/asdl.pl) | Generate C11 ASTs, visitors and runtimes from ASDL schemas. |
| [LLSynth](LLSynth/) | [`LLSynth/scripts/llsynth.pl`](LLSynth/scripts/llsynth.pl) | Generate language tools in Python, C or C++ from LLSynth specifications. |
| [Archon](Archon/) | `cd cornerstone/Archon && python3 -m archon --help` | Compile architecture descriptions for assembly, disassembly, interpretation, and standalone C99 emulation. |
| [Cephyr](Cephyr/) | `cmake -S cornerstone/Cephyr -B build-cephyr` | Unix-style C compiler using Archon IRGen and a bundled RV64IM Linux C runtime. |

The older Aurocks Perl implementation can still be found at
[`yellowstone/scripts/aurocks.pl`](../yellowstone/scripts/aurocks.pl).
