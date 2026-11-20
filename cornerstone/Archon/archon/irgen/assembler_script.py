"""Generate an argv-safe assembler launcher for the selected pipeline target."""

from collections.abc import Mapping
from importlib.resources import files
import json
import re
import shlex

from ..diagnostics import ArchonError
from ..runtime import Target


_FORMATS = {
    "gas": ("obj",),
    "nasm": ("elf64", "bin", "win64", "macho64", "elf32", "win32", "macho32"),
    "fasm": ("source",),
    "llvm": ("obj",),
    "sandstone": ("bin", "obj"),
    "archon": ("bin",),
}
_PROVIDER_OPTIONS = {
    "llvm": {"llvm_triple", "llvm_mattr"},
    "sandstone": {"sandstone_bank", "sandstone_archml", "sandstone_syntax"},
}
_TRIPLE = re.compile(r"[A-Za-z0-9_]+(?:-[A-Za-z0-9_.]+){0,4}")
_MATTR = re.compile(r"[+-][A-Za-z0-9][A-Za-z0-9_.-]*(?:,[+-][A-Za-z0-9][A-Za-z0-9_.-]*)*")
_SYNTAX = re.compile(r"[A-Za-z_][A-Za-z0-9_.-]*")

# Mappings for shipped architectures with reviewed native assembly providers.
# The tuple includes width and endianness so a contrary ArchDB cannot silently
# select (for example) a little-endian target just by reusing the same name.
_TARGETS = {
    ("AMD64", 64, "little"): ("x86_64", "x86_64-unknown-linux-gnu", "as", ("--64",)),
    ("AArch64", 64, "little"): (
        "aarch64", "aarch64-unknown-linux-gnu", "aarch64-linux-gnu-as", ("-EL",),
    ),
    ("MIPS32", 32, "little"): (
        "mipsel", "mipsel-unknown-linux-gnu", "mipsel-linux-gnu-as",
        ("-EL", "-march=mips32", "-mabi=32"),
    ),
    ("RISCV32", 32, "little"): (
        "riscv32", "riscv32-unknown-elf", "riscv64-linux-gnu-as", ("-march=rv32i", "-mabi=ilp32"),
    ),
    ("RISCV64", 64, "little"): (
        "riscv64", "riscv64-unknown-elf", "riscv64-linux-gnu-as", ("-march=rv64i", "-mabi=lp64"),
    ),
    ("RISCV64IM", 64, "little"): (
        "riscv64", "riscv64-unknown-elf", "riscv64-linux-gnu-as", ("-march=rv64im", "-mabi=lp64"),
    ),
    ("Wasm32", 32, "little"): ("wasm32", "wasm32-unknown-unknown", None, ()),
}
_LLVM_ARCH_ALIASES = {"x86_64": ("amd64",), "aarch64": ("arm64",)}


def _option(options, name, default=None, *, pattern=None):
    if name not in options:
        return default
    value = options[name]
    if (
        not isinstance(value, str)
        or not value.strip()
        or any(ord(character) < 32 or ord(character) == 127 for character in value)
    ):
        raise ArchonError(f"assembler option {name!r} must be a nonempty string without control characters")
    if pattern is not None and not pattern.fullmatch(value):
        raise ArchonError(f"invalid assembler option {name!r}: {value!r}")
    return value


def _selected_database(context, options):
    db = context.get("database")
    if db is not None:
        return db
    # The normal orchestrator supplies database. Also accept the documented
    # context shape when this stage is used directly.
    architectures = context.get("architectures", ())
    name = options.get("target")
    if name is not None:
        db = next((row for row in architectures if row["architecture"] == name), None)
        if db is None:
            raise ArchonError(f"assembler target {name!r} has no selected ArchDB")
        return db
    return min(architectures, key=lambda row: row["architecture"], default=None)


def _literal_path(path):
    # Some providers, including GAS and LLVM, expand @response files even
    # before their option parser sees '--'. Preserve these as literal paths.
    return "./" + path if path.startswith(("-", "@")) else path


def _archon_runtime(db):
    if db is None:
        raise ArchonError("the Archon assembler requires a selected ArchDB")
    Target(db)
    # Ship the existing fixed-width parser/encoder instead of duplicating it
    # or requiring an installed Archon at runtime. Only that codec's data is
    # embedded; no ABI facts, scheduling models, or ArchSem are consumed.
    selected = {
        key: db[key]
        for key in ("version", "address_width", "endian", "registers")
        if key in db
    }
    selected["instructions"] = [
        {key: row[key] for key in ("name", "width", "mask", "value", "operands", "fields", "assembly")}
        for row in db["instructions"]
    ]
    encoded = json.dumps(selected, sort_keys=True, separators=(",", ":"), ensure_ascii=True)
    runtime = files(__package__.rsplit(".", 1)[0])
    result = {
        "ArchonAssembler.py": _ARCHON_ASSEMBLER.replace("@DATABASE@", repr(encoded)),
        "_archon_asm_runtime/__init__.py": _RUNTIME_LICENSE,
    }
    for module in ("database.py", "diagnostics.py", "runtime.py", "semantics.py"):
        result[f"_archon_asm_runtime/{module}"] = runtime.joinpath(module).read_text(encoding="utf-8")
    return result


def emit(context) -> dict[str, str]:
    options = context.get("options", {})
    if not isinstance(options, Mapping):
        raise ArchonError("assembler options must be an object")
    provider = _option(options, "assembler", "gas")
    if provider not in _FORMATS:
        raise ArchonError(f"unknown assembler {provider!r}; choose from {', '.join(_FORMATS)}")
    for owner, names in _PROVIDER_OPTIONS.items():
        incompatible = sorted(names & options.keys())
        if owner != provider and incompatible:
            raise ArchonError(f"assembler option {incompatible[0]!r} requires assembler={owner}")

    program = _option(options, "assembler_program")
    if program is not None and program.startswith("-"):
        raise ArchonError("assembler option 'assembler_program' must be an executable, not an option")

    db = _selected_database(context, options)
    name = None if db is None else db["architecture"]
    target = None if db is None else _TARGETS.get((name, db.get("address_width"), db.get("endian")))
    arch, triple, gas, gas_flags = target or (None, None, None, ())
    if provider in ("nasm", "fasm") and db is not None and arch != "x86_64":
        raise ArchonError(f"{provider} requires an x86 source/target; no mapping for ArchDB {name!r}")
    default_format = "bin" if provider == "nasm" and db is None else _FORMATS[provider][0]
    output_format = _option(options, "assembler_format", default_format)
    if output_format not in _FORMATS[provider]:
        raise ArchonError(f"{provider} assembler_format must be one of: {', '.join(_FORMATS[provider])}")
    if provider == "nasm" and arch == "x86_64" and output_format.endswith("32"):
        raise ArchonError("cannot use a 32-bit NASM object format for AMD64")

    script = (
        f"#!/bin/sh\n# Generated by Archon ({provider} assembler).\n"
        "# Use trusted tools and assembly inputs. Source syntax remains provider-native.\n"
        + _ARGUMENTS
    )
    if provider in ("llvm", "nasm", "fasm"):
        script += f'[ "$#" -eq 1 ] || fail "{provider} accepts exactly one assembly input"\n'
    outputs = {}
    args = []
    if provider == "gas":
        if db is not None and program is None and gas is None:
            message = f"no GNU assembler mapping for ArchDB {name!r}; configure a target assembler_program"
            return {"Assembler.sh": script + f"fail {shlex.quote(message)}\n"}
        program = program or gas or "as"
        args.extend(gas_flags)
        # GAS treats '--' as a stdin operand in some versions. All literal
        # option-like paths were already prefixed with './' by the wrapper.
        invocation = '-o "$staged" "$@"'
    elif provider == "llvm":
        program = program or "llvm-mc"
        triple = _option(options, "llvm_triple", triple, pattern=_TRIPLE)
        if triple is None:
            raise ArchonError("llvm assembler requires 'llvm_triple' for this target")
        if arch and triple.split("-", 1)[0] not in (arch, *_LLVM_ARCH_ALIASES.get(arch, ())):
            raise ArchonError(f"llvm_triple {triple!r} does not match ArchDB {name!r}")
        args.extend((f"-triple={triple}", "-filetype=obj"))
        attributes = _option(
            options, "llvm_mattr", "+m" if name == "RISCV64IM" and arch == "riscv64" else None,
            pattern=_MATTR,
        )
        if attributes is not None:
            features = attributes.split(",")
            if len({feature[1:] for feature in features}) != len(features):
                raise ArchonError("llvm_mattr must not repeat a target attribute")
            args.append(f"-mattr={attributes}")
        invocation = '-o "$staged" -- "$@"'
    elif provider == "nasm":
        program = program or "nasm"
        args.extend(("-f", output_format))
        if arch == "x86_64" and output_format == "bin":
            # NASM otherwise defaults to 16-bit mode for flat binary output.
            args.extend(("--before", "BITS 64"))
        # Do not rely on NASM releases interpreting '--' consistently.
        invocation = '-o "$staged" "$1"'
    elif provider == "fasm":
        program = program or "fasm"
        script += "# FASM output format and instruction mode are set in the assembly source.\n"
        invocation = '"$1" "$staged"'
    elif provider == "sandstone":
        program = program or "sandstone-as"
        bank = _option(options, "sandstone_bank")
        archml = _option(options, "sandstone_archml")
        syntax = _option(options, "sandstone_syntax", pattern=_SYNTAX)
        if bank is None and archml is None:
            raise ArchonError("sandstone assembler requires 'sandstone_bank' or 'sandstone_archml'")
        if syntax is not None and bank is None:
            raise ArchonError("sandstone_syntax requires a sandstone_bank")
        for flag, path in (("--bank", bank), ("--archml", archml)):
            if path is not None:
                path = _literal_path(path)
                args.extend((flag, path))
                script += f"protect_output {shlex.quote(path)} {shlex.quote(flag[2:])}\n"
        if syntax is not None:
            args.extend(("--syntax", syntax))
        args.extend(("-f", output_format))
        script += _ADJACENT
        script += '[ -f "$script_dir/AsmOPT.lua" ] || fail "missing adjacent AsmOPT.lua"\n'
        script += 'protect_output "$script_dir/AsmOPT.lua" "optimizer hook"\n'
        outputs["AsmOPT.lua"] = (
            _RISCV64_ASM_OPT if arch == "riscv64" and bank and syntax == "default" else _IDENTITY_ASM_OPT
        )
        invocation = '--asm-opt "$script_dir/AsmOPT.lua" -o "$staged" -- "$@"'
    else:  # archon
        program = program or "python3"
        outputs.update(_archon_runtime(db))
        script += _ADJACENT
        script += _PROTECT_RUNTIME
        invocation = '-I -B "$script_dir/ArchonAssembler.py" "$staged" "$@"'

    script += f"assembler={shlex.quote(program)}\n" + _FIND_EXECUTABLE + _STAGE_OUTPUT
    fixed = shlex.join(args) + " " if args else ""
    outputs["Assembler.sh"] = script + f'if "$tool" {fixed}{invocation}; then\n' + _PUBLISH
    return outputs


_ARGUMENTS = r'''
set -eu

fail() {
    printf '%s\n' "Assembler.sh: $*" >&2
    exit 2
}

protect_output() {
    if [ "$1" = "$output" ] || [ "$1" -ef "$output" ]; then
        fail "output would overwrite $2: $1"
    fi
}

[ "$#" -ge 3 ] && [ "$1" = -o ] && [ -n "$2" ] ||
    fail "usage: sh Assembler.sh -o OUTPUT [--] INPUT..."
output=$2
shift 2
if [ "$1" = -- ]; then
    shift
fi
[ "$#" -gt 0 ] || fail "at least one assembly input is required"
case $output in
    -*|@*) output=./$output ;;
esac
protect_output "$0" "wrapper"

# Rotate the original arguments, keeping each literal path in its own argv
# slot and using the caller's working directory for all paths it supplied.
for input do
    [ -n "$input" ] || fail "empty assembly input path"
    case $input in
        -*|@*) input=./$input ;;
    esac
    protect_output "$input" "an assembly input"
    shift
    set -- "$@" "$input"
done

'''

_ADJACENT = r'''case $0 in
    /*) script_path=$0 ;;
    *) script_path=./$0 ;;
esac
script_dir=${script_path%/*}
'''

_PROTECT_RUNTIME = r'''[ -f "$script_dir/ArchonAssembler.py" ] || fail "missing adjacent ArchonAssembler.py"
protect_output "$script_dir/ArchonAssembler.py" "generated Archon assembler"
for runtime in "$script_dir"/_archon_asm_runtime/*.py; do
    protect_output "$runtime" "bundled Archon runtime"
done
'''

_FIND_EXECUTABLE = r'''tool=$(command -v "$assembler") || tool=
case $tool in
    */*) ;;
    ?*) tool=./$tool ;;
esac
if [ ! -f "$tool" ] || [ ! -x "$tool" ]; then
    printf '%s\n' "Assembler.sh: executable not found: $assembler (configure assembler_program)" >&2
    exit 127
fi
protect_output "$tool" "selected assembler"
'''

_STAGE_OUTPUT = r'''
# Keep the originally requested basename: providers may inspect its suffix.
# Stage on the destination filesystem so publication is a single rename.
case $output in
    */) fail "output path must name a file" ;;
    */*) outdir=${output%/*}; outdir=${outdir:-/} ;;
    *) outdir=. ;;
esac
check_destination() {
    if [ -d "$output" ] || { [ -e "$output" ] && [ ! -f "$output" ]; }; then
        fail "output path is not a regular file: $output"
    fi
}
check_destination
for utility in mktemp mv rm; do
    command -v "$utility" >/dev/null 2>&1 || fail "required utility not found: $utility"
done
tempdir=$(mktemp -d "${outdir%/}/.archon-as.XXXXXXXX") || fail "cannot stage output: $output"
staged=$tempdir/${output##*/}
trap 'rm -rf "$tempdir"' 0
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

'''

_PUBLISH = r'''    [ -f "$staged" ] && [ ! -L "$staged" ] ||
        fail "$assembler reported success without producing a regular output: $output"
    check_destination
    mv -f "$staged" "$output" || fail "cannot publish output: $output"
else
    status=$?
    printf '%s\n' "Assembler.sh: $assembler failed (exit $status)" >&2
    exit "$status"
fi
'''

_IDENTITY_ASM_OPT = '''-- Generated by Archon. No proven post-expansion rewrite for this target/syntax.
function asm_optimize(source, name)
    return source
end
'''

_RISCV64_ASM_OPT = r'''-- Generated by Archon for RV64 and the explicitly selected "default" syntax.
-- The only rewrite is the exact RV64I NOP alias. Require evidence from the
-- *loaded* bank: both forms must emit the same NOP bytes without relocations.
-- Match only the two exact lines tested by the bank probe. In a custom
-- syntax, even extra whitespace can alter comment and separator parsing.
-- Same bytes imply identical layout and machine state.
function asm_optimize(source, name)
    if type(asm_context) ~= "table" or asm_context.arch ~= "riscv64"
        or asm_context.syntax ~= "default" or asm_context.riscv64_nop_alias ~= true then
        return source
    end
    local rewritten = source:gsub("[^\n]+", function(line)
        if line == "addi x0, x0, 0" or line == "addi x0,x0,0" then
            return "nop"
        end
        return line
    end)
    return rewritten
end
'''

_RUNTIME_LICENSE = '''"""Archon runtime bundled for Assembler.sh."""

# MIT License
#
# Copyright (c) 2026 Chubak Bidpaa <chubakbidpaa@riseup.net>
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in all
# copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.
'''

_ARCHON_ASSEMBLER = '''#!/usr/bin/env python3
"""Generated ArchDB v1 codec: canonical syntax in, flat binary out.

Each UTF-8 input has its own local labels and is placed after the preceding
input, starting at byte address zero. No sections, directives, macros, external
symbols, relocation records, or mixed-width encodings.
"""

import json
from pathlib import Path
import sys

# Assembler.sh uses Python's isolated mode; import only the adjacent runtime.
sys.path.insert(0, str(Path(__file__).resolve().parent))
from _archon_asm_runtime.diagnostics import ArchonError
from _archon_asm_runtime.runtime import Target

_DB = @DATABASE@


def main(args):
    if len(args) < 2:
        print("usage: sh Assembler.sh -o OUTPUT [--] INPUT...", file=sys.stderr)
        return 2
    output, *inputs = args
    try:
        target = Target(json.loads(_DB))
        image = bytearray()
        for source in inputs:
            text = Path(source).read_text(encoding="utf-8")
            image.extend(target.assemble_program(text, base_address=len(image), filename=source))
        Path(output).write_bytes(image)
    except (ArchonError, OSError, UnicodeError) as error:
        print(f"Archon assembler: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
'''
