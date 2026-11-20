"""Command-line interface to the Archon compiler and runtime."""

import argparse
import json
import os
from pathlib import Path
import re
import sys

from . import database
from .c99 import generate_codec
from .compiler import compile_file
from .diagnostics import ArchonError
from .runtime import Target


_IRGEN_ASSEMBLERS = ("gas", "nasm", "fasm", "llvm", "sandstone", "archon")
_IRGEN_LINKERS = ("gnu", "gold", "mold", "lld", "yld")
_IRGEN_TOOL_OPTIONS = (
    "assembler", "assembler_program", "assembler_format", "llvm_triple", "llvm_mattr",
    "sandstone_bank", "sandstone_archml", "sandstone_syntax",
    "linker", "linker_program", "linker_script", "lto", "lto_driver",
)
_IRGEN_TOOL_INPUTS = ("linker_script", "sandstone_bank", "sandstone_archml")
_IRGEN_TOOL_PROGRAMS = ("assembler_program", "linker_program", "lto_driver")


def _assignment(text):
    name, sep, value = text.partition("=")
    if not sep or not name or not value:
        raise argparse.ArgumentTypeError("expected NAME=INTEGER")
    return name, _integer(value)


def _integer(value):
    try:
        return int(value, 0)
    except ValueError:
        if re.fullmatch(r"[+-]?[0-9]+", value):
            try:
                return int(value, 10)
            except ValueError:
                pass
        raise argparse.ArgumentTypeError(f"invalid integer {value!r}") from None


def _program_base(value):
    address = _integer(value)
    if address < 0:
        raise argparse.ArgumentTypeError("base address must be nonnegative")
    return address


def _features(text):
    if not text.strip():
        return []
    names = [name.strip() for name in text.split(",")]
    if any(not name for name in names):
        raise argparse.ArgumentTypeError("expected a comma-separated list of feature names")
    return names


def _generation_option(text):
    name, sep, value = text.partition("=")
    if not sep or not re.fullmatch(r"[A-Za-z][A-Za-z0-9_.-]*", name):
        raise argparse.ArgumentTypeError("expected OPTION=JSON")
    try:
        parsed = json.loads(value)
    except json.JSONDecodeError as exc:
        raise argparse.ArgumentTypeError(
            f"invalid JSON value for generation option {name!r}: {exc.msg}"
        ) from None
    return name, parsed


def _compile(args):
    db = compile_file(args.source, parameters=dict(args.define), features=args.features)
    if args.command == "compile":
        if args.output == "-":
            sys.stdout.write(database.dumps(db))
        else:
            database.save(db, args.output)


def _inspect(args):
    db = database.load(args.database)
    if args.json:
        sys.stdout.write(database.dumps(db))
        return
    regs = db["registers"]
    print(f"{db['architecture']} ({db['address_width']}-bit, {db['endian']}-endian)")
    print(f"ArchDB schema version: {db['version']}")
    if regs is None:
        print("Registers: (none)")
    else:
        print(f"Registers: {regs['count']} × {regs['width']}-bit ({regs['name']})")
    state = db.get("state", {"scalars": [], "stacks": [], "memories": []})
    print(
        "Non-register state: "
        f"{len(state['scalars'])} scalar(s), {len(state['stacks'])} stack(s), "
        f"{len(state['memories'])} memory space(s)"
    )
    params = ", ".join(f"{name}={db['parameters'][name]}" for name in sorted(db["parameters"]))
    print(f"Parameters: {params or '(none)'}")
    print(f"Features: {', '.join(db['features']) or '(none)'}")
    abi = db.get("abi_facts_v2", db.get("abi_facts"))
    if abi is not None:
        print(f"Scalar ABI facts: {abi['name']} (schema {abi['schema_version']})")
    print(f"Instructions ({len(db['instructions'])}):")
    for inst in db["instructions"]:
        print(f"  {inst['name']} ({inst['width']} bits)")


def _generate(args):
    if args.header is not None and not args.emulator:
        raise ArchonError("--header requires --emulator")
    if args.output != "-" and _same_path(args.database, args.output):
        raise ArchonError("C99 output path must differ from the input database")
    if args.header is not None:
        if args.header != "-" and _same_path(args.database, args.header):
            raise ArchonError("C99 header path must differ from the input database")
        if (args.header == args.output
                or (args.header != "-" and args.output != "-" and _same_path(args.header, args.output))):
            raise ArchonError("C99 output and header must use different destinations")

    db = database.load(args.database)
    if args.emulator:
        from .c99_emulator import generate_emulator, generate_emulator_header

        code = generate_emulator(db, prefix=args.prefix)
        header = generate_emulator_header(db, prefix=args.prefix) if args.header is not None else None
    else:
        code = generate_codec(db, prefix=args.prefix)
        header = None

    _write_c99(args.output, code)
    if header is not None:
        _write_c99(args.header, header)


def _ir_generate(args):
    from .irgen import (
        generate_ir,
        standard_spec_entry_points,
        write_generated_directory,
    )

    paths = [*args.databases, *args.database]
    if paths:
        seen = set()
        for path in paths:
            try:
                canonical = Path(path).resolve(strict=False)
            except (OSError, RuntimeError) as exc:
                raise ArchonError(f"cannot resolve ArchDB path {path!r}: {exc}") from exc
            if canonical in seen:
                raise ArchonError(f"duplicate ArchDB input {path!r}")
            seen.add(canonical)
        architectures = [database.load(path) for path in paths]
        inputs = paths
    else:
        entries = standard_spec_entry_points()
        architectures = [compile_file(path) for path in entries]
        inputs = entries

    options = {}
    for name, value in args.option:
        if name in options:
            raise ArchonError(f"duplicate generation option {name!r}")
        options[name] = value
    if args.no_oracle:
        if "oracle" in options:
            raise ArchonError(
                "oracle was specified by both --no-oracle and --option"
            )
        options["oracle"] = False
    if args.target is not None:
        if "target" in options:
            raise ArchonError(
                "target was specified by both --target and --option"
            )
        options["target"] = args.target
    for name in ("profile", "abi", *_IRGEN_TOOL_OPTIONS):
        value = getattr(args, name)
        if value is not None:
            if name in options:
                flag = "no-lto" if name == "lto" and not value else name.replace("_", "-")
                raise ArchonError(f"{name} was specified by both --{flag} and --option")
            options[name] = value
    # --force must not replace a configured script, bank, or executable
    # supplied by path under the destination.
    inputs = [*inputs]
    for name in (*_IRGEN_TOOL_INPUTS, *_IRGEN_TOOL_PROGRAMS):
        value = options.get(name)
        if (isinstance(value, str) and value and "\0" not in value
                and (name in _IRGEN_TOOL_INPUTS or "/" in value or "\\" in value)):
            inputs.append(value)
    outputs = generate_ir(args.ir, architectures, options=options)
    write_generated_directory(
        outputs, args.output, input_paths=inputs, force=args.force,
    )


def _write_c99(path, text):
    if path == "-":
        sys.stdout.write(text)
    else:
        Path(path).write_text(text, encoding="utf-8", newline="\n")


def _native_object(args):
    from .irgen.objectwriter import assemble_object

    assemble_object(args.backend, args.assembler, args.assembler_flavor,
                    args.source, args.output)


def _native_link(args):
    from .irgen.objectwriter import link_executable

    link_executable(args.linker, args.objects, args.entry, args.output)


def _same_path(first, second):
    if os.path.realpath(first) == os.path.realpath(second):
        return True
    try:
        return os.path.samefile(first, second)
    except FileNotFoundError:
        return False


def _assemble(args):
    target = Target(database.load(args.database))
    name, operands = target.assemble(args.instruction)
    data = target.encode(name, operands)
    if args.output is None:
        print(data.hex())
    elif args.output == "-":
        sys.stdout.buffer.write(data)
    else:
        Path(args.output).write_bytes(data)


def _disassemble(args):
    target = Target(database.load(args.database))
    if args.file is not None:
        data = sys.stdin.buffer.read() if args.file == "-" else Path(args.file).read_bytes()
    else:
        try:
            data = bytes.fromhex(args.hex_bytes)
        except ValueError as exc:
            raise ArchonError("invalid hex byte string; pass bytes in target order, e.g. b3003100") from exc
    print(target.disassemble(data))


def _program_bytes(args, target):
    if args.source == "-":
        text, filename = sys.stdin.buffer.read().decode("utf-8"), "<stdin>"
    else:
        text, filename = Path(args.source).read_text(encoding="utf-8"), args.source
    return target.assemble_program(text, base_address=args.base, filename=filename)


def _assemble_program(args):
    target = Target(database.load(args.database))
    data = _program_bytes(args, target)
    if args.output == "-":
        sys.stdout.buffer.write(data)
    else:
        Path(args.output).write_bytes(data)


def _disassemble_program(args):
    target = Target(database.load(args.database))
    data = sys.stdin.buffer.read() if args.file == "-" else Path(args.file).read_bytes()
    text = target.disassemble_program(data, base_address=args.base)
    if args.output == "-":
        sys.stdout.write(text)
    else:
        Path(args.output).write_text(text, encoding="utf-8", newline="\n")


def _register_index(regs, name):
    if regs is None:
        raise ArchonError(f"target has no registers (cannot set {name!r})")
    if name in regs["aliases"]:
        return regs["aliases"][name]
    prefix = regs["prefix"]
    if name.startswith(prefix):
        suffix = name[len(prefix):]
        if re.fullmatch(r"[0-9]+", suffix):
            try:
                index = int(suffix)
            except ValueError:
                pass
            else:
                if index < regs["count"]:
                    return index
    raise ArchonError(f"unknown register {name!r} in --reg")


def _run(args):
    db = database.load(args.database)
    target = Target(db)
    name, operands = target.assemble(args.instruction)
    regs = db["registers"]
    result = target.execute(name, operands, _initial_registers(regs, args.reg))
    _print_registers(regs, result)


def _run_program(args):
    db = database.load(args.database)
    target = Target(db)
    data = _program_bytes(args, target)
    regs = db["registers"]
    result = target.execute_program(data, _initial_registers(regs, args.reg), base_address=args.base)
    _print_registers(regs, result)


def _initial_registers(regs, assignments):
    initial = [0] * (regs["count"] if regs is not None else 0)
    for register, value in assignments:
        initial[_register_index(regs, register)] = value
    return initial


def _print_registers(regs, result):
    prefix = regs["prefix"] if regs is not None else ""
    print(json.dumps({f"{prefix}{i}": value for i, value in enumerate(result)}))


def _register_option(command):
    command.add_argument(
        "--reg", metavar="NAME=VALUE", action="append", type=_assignment, default=[],
        help="set an initial register value (repeatable, default: zero)",
    )


def _program_input(commands, name, help_text, input_kind, handler):
    command = commands.add_parser(name, help=help_text)
    command.add_argument("database", help="compiled .archdb file")
    if input_kind == "source":
        command.add_argument("source", help="UTF-8 assembly file (or '-' for stdin)")
    else:
        command.add_argument("file", help="raw bytes in target byte order (or '-' for stdin)")
    command.add_argument("--base", metavar="ADDRESS", type=_program_base, default=0,
                         help="starting byte address (default: 0)")
    command.set_defaults(handler=handler)
    return command


def _parser():
    parser = argparse.ArgumentParser(
        prog="archon", description="Compile .arch descriptions and use compiled targets."
    )
    commands = parser.add_subparsers(dest="command", required=True)
    for name, help_text in (
        ("compile", "compile an architecture to an ArchDB JSON database"),
        ("check", "validate an architecture without writing a database"),
    ):
        command = commands.add_parser(name, help=help_text)
        command.add_argument("source", help=".arch entry point")
        command.add_argument(
            "-D", dest="define", metavar="NAME=VALUE", action="append", type=_assignment,
            default=[], help="override an integer architecture parameter (repeatable)",
        )
        command.add_argument(
            "--features", metavar="NAMES", type=_features,
            help="replace default features with comma-separated NAMES; use --features= for none",
        )
        if name == "compile":
            command.add_argument("-o", "--output", default="-", help="ArchDB output path (default: stdout)")
        command.set_defaults(handler=_compile)

    inspect = commands.add_parser("inspect", help="show a compiled architecture")
    inspect.add_argument("database", help="compiled .archdb file")
    inspect.add_argument("--json", action="store_true", help="print the full ASIR JSON")
    inspect.set_defaults(handler=_inspect)

    gen = commands.add_parser("gen", help="generate a standalone C99 codec or emulator")
    gen.add_argument("database", help="compiled .archdb file")
    gen.add_argument("-o", "--output", default="-", help="C99 output path (default: stdout)")
    gen.add_argument("--prefix", default="archon", help="prefix for C identifiers (default: archon)")
    gen.add_argument("--emulator", action="store_true",
                     help="generate a transactional architectural-state emulator with its codec")
    gen.add_argument("--header", metavar="PATH",
                     help="also write the emulator's public C99 header (or '-' for stdout)")
    gen.set_defaults(handler=_generate)

    irgen = commands.add_parser(
        "irgen",
        help="generate a standalone C99 IR implementation and pipeline support",
    )
    irgen.add_argument(
        "ir",
        help=(
            "IR representation: gated, memory-ssa, sea-of-nodes, anf, cps, "
            "typed-tac, untyped-tac, or supercombinators"
        ),
    )
    irgen.add_argument(
        "databases",
        nargs="*",
        metavar="ARCHDB",
        help="compiled ArchDB(s); omit to compile every bundled stdspec entry point",
    )
    irgen.add_argument(
        "-d",
        "--database",
        "--archdb",
        action="append",
        default=[],
        metavar="ARCHDB",
        help="add a compiled ArchDB (repeatable; may be combined with positional inputs)",
    )
    irgen.add_argument(
        "-o",
        "--output",
        required=True,
        metavar="DIRECTORY",
        help="atomic output directory",
    )
    irgen.add_argument(
        "--option",
        action="append",
        type=_generation_option,
        default=[],
        metavar="NAME=JSON",
        help="pass a JSON-compatible generation option (repeatable)",
    )
    irgen.add_argument(
        "--target",
        metavar="ARCHITECTURE",
        help=(
            "choose the final pipeline/emitter target when generating for "
            "multiple ArchDBs (all selected targets remain in shared facts)"
        ),
    )
    irgen.add_argument(
        "--no-oracle",
        action="store_true",
        help="omit the generated Oracle IPC and annotation support",
    )
    irgen.add_argument(
        "--profile", choices=("legacy", "native"),
        help="include the native runtime backend for a supported compiled target",
    )
    irgen.add_argument(
        "--abi", metavar="ABI",
        help="native backend ABI name (LP64, SysVAMD64, or AAPCS64, depending on target)",
    )
    irgen.add_argument(
        "--assembler", choices=_IRGEN_ASSEMBLERS,
        help="provider for generated Assembler.sh (default: gas; llvm uses llvm-mc)",
    )
    irgen.add_argument(
        "--assembler-program", metavar="PROGRAM",
        help="override the selected external assembler executable",
    )
    irgen.add_argument(
        "--assembler-format", metavar="FORMAT",
        help="select a provider-specific output format",
    )
    irgen.add_argument(
        "--llvm-triple", metavar="TRIPLE",
        help="select an explicit target triple for llvm-mc",
    )
    irgen.add_argument(
        "--llvm-mattr", metavar="FEATURES",
        help="set comma-separated llvm-mc attributes such as +m",
    )
    irgen.add_argument(
        "--sandstone-bank", metavar="PATH",
        help="SymAS bank for the Sandstone assembler",
    )
    irgen.add_argument(
        "--sandstone-archml", metavar="PATH",
        help="optional Sandstone ArchML profile (may replace a SymAS bank)",
    )
    irgen.add_argument(
        "--sandstone-syntax", metavar="NAME",
        help="syntax from the selected SymAS bank",
    )
    irgen.add_argument(
        "--linker", choices=_IRGEN_LINKERS,
        help="provider for generated Linker.sh (default: gnu; yld uses qobjld)",
    )
    irgen.add_argument(
        "--linker-program", metavar="PROGRAM",
        help="override the selected linker executable",
    )
    irgen.add_argument(
        "--linker-script", metavar="PATH",
        help="layout script for Linker.sh (.lua is specific to Yellowstone)",
    )
    irgen.add_argument(
        "--lto-driver", metavar="PROGRAM",
        help="GCC driver for GNU ld, Gold or Mold LTO",
    )
    lto = irgen.add_mutually_exclusive_group()
    lto.add_argument(
        "--lto", dest="lto", action="store_true", default=None,
        help="enable link-time optimization in Linker.sh",
    )
    lto.add_argument(
        "--no-lto", dest="lto", action="store_false",
        help="disable link-time optimization (the default)",
    )
    irgen.add_argument(
        "--force",
        action="store_true",
        help="replace a nonempty output directory (input paths remain protected)",
    )
    irgen.set_defaults(handler=_ir_generate)

    native_obj = commands.add_parser(
        "native-object", help="compile runtime MachineIR to an RV64IM ELF object using an external assembler",
    )
    native_obj.add_argument("source", help="runtime MachineIR text path (or '-' for stdin)")
    native_obj.add_argument("--backend", required=True, help="generated archon-backend executable")
    native_obj.add_argument("--assembler", required=True, help="configured Clang or RISC-V GNU assembler")
    native_obj.add_argument("--assembler-flavor", choices=("clang", "gnu"), default="clang")
    native_obj.add_argument("-o", "--output", required=True, help="destination ELF object file")
    native_obj.set_defaults(handler=_native_object)

    native_link = commands.add_parser(
        "native-link", help="link RV64IM ELF objects into a static executable using a configured linker",
    )
    native_link.add_argument("objects", nargs="+", help="RV64IM LP64 relocatable ELF objects")
    native_link.add_argument("--linker", required=True, help="configured ld.lld executable")
    native_link.add_argument("--entry", default="_start", help="entry symbol (default: _start)")
    native_link.add_argument("-o", "--output", required=True, help="destination ELF executable")
    native_link.set_defaults(handler=_native_link)

    asm = commands.add_parser("asm", help="assemble one instruction")
    asm.add_argument("database", help="compiled .archdb file")
    asm.add_argument("instruction", help="quoted assembly instruction")
    asm.add_argument("-o", "--output", help="write raw bytes to this path (or '-' for stdout); default: hex")
    asm.set_defaults(handler=_assemble)

    dis = commands.add_parser("dis", help="disassemble one instruction")
    dis.add_argument("database", help="compiled .archdb file")
    dis_input = dis.add_mutually_exclusive_group(required=True)
    dis_input.add_argument("hex_bytes", nargs="?", help="hexadecimal bytes in target byte order")
    dis_input.add_argument("--file", help="read raw bytes from this path (or '-' for stdin)")
    dis.set_defaults(handler=_disassemble)

    run = commands.add_parser("run", help="execute one instruction against a register file")
    run.add_argument("database", help="compiled .archdb file")
    run.add_argument("instruction", help="quoted assembly instruction")
    _register_option(run)
    run.set_defaults(handler=_run)

    asm_program = _program_input(commands, "asm-program", "assemble a file with local labels",
                                 "source", _assemble_program)
    asm_program.add_argument("-o", "--output", default="-", help="raw bytes output path (default: stdout)")

    dis_program = _program_input(commands, "dis-program", "disassemble a raw instruction stream",
                                 "file", _disassemble_program)
    dis_program.add_argument("-o", "--output", default="-", help="assembly output path (default: stdout)")

    run_program = _program_input(commands, "run-program", "execute an assembly file linearly (registers only)",
                                 "source", _run_program)
    _register_option(run_program)
    return parser


def main(argv: list[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    try:
        args.handler(args)
    except (ArchonError, OSError, UnicodeError) as exc:
        print(f"archon: error: {exc}", file=sys.stderr)
        return 1
    return 0
