# Cornerstone

Cornerstone contains standalone language and toolchain generators.

| Project | Entry point | Purpose |
| --- | --- | --- |
| [Aurocks](Aurocks/) | [`Aurocks/scripts/aurocks.pl`](Aurocks/scripts/aurocks.pl) | Generate scannerless C99 parsers from Aurocks grammars; supports a Firestone `liblr` regex backend. |
| [ASDL](ASDL/) | [`ASDL/scripts/asdl.pl`](ASDL/scripts/asdl.pl) | Generate C11 ASTs, visitors and runtimes from ASDL schemas. |
| [LLSynth](LLSynth/) | [`LLSynth/scripts/llsynth.pl`](LLSynth/scripts/llsynth.pl) | Generate language tools in Python, C or C++ from LLSynth specifications. |
| [Archon](Archon/) | `cd cornerstone/Archon && python3 -m archon --help` | Compile architecture descriptions for assembly, disassembly, interpretation, and standalone C99 emulation. |
| [Cephyr](Cephyr/) | `cmake -S cornerstone/Cephyr -B build-cephyr` | Unix-style C compiler using Archon IRGen and a bundled RV64IM Linux C runtime. |

The existing Perl entry points in the repository-root `scripts/` directory
continue to work.
