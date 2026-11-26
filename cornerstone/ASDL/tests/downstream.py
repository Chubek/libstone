"""Run the ASDL generation steps used by existing Libstone consumers."""

import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--root", required=True, type=Path)
parser.add_argument("--perl", required=True)
parser.add_argument("--cc", required=True)
parser.add_argument("--work-dir", required=True, type=Path)
parser.add_argument("--native", type=Path)
options = parser.parse_args()
root = options.root.resolve()

# Paths and output names mirror the generation recipes. This deliberately
# doesn't build each consumer's unrelated parser-generator and runtime deps.
recipes = [
    ("cornerstone/Archon/examples/translate/Lang.asdl", None, None, None, False),
    ("limestone/traceml/TraceSource.asdl", "TraceSourceAST.h", "TraceSourceAST.c", None, False),
    ("limestone/vmweave/Abstract.asdl", "AbstractAST.h", "AbstractAST.c", None, False),
    ("moonstone/exoml/ExoML.asdl", "exoml_ast.h", None, "exoml_visitor.h", True),
    ("novostone/ActionSemantics/ActionSemantics.asdl", "ActionSemanticsAST.h",
     "ActionSemanticsAST.c", "ActionSemanticsVisitor.h", False),
    ("novostone/Datalog/Datalog.asdl", "DatalogAST.h", "DatalogAST.c", None, False),
    ("sandstone/archml/ArchML.asdl", "ArchMLAST.h", None, None, False),
    ("yellowstone/ld/ldscript.asdl", "ldscript_ast.h", None, None, True),
]


def run(cmd, *args, cwd):
    proc = subprocess.run([*cmd, *map(str, args)], cwd=cwd, capture_output=True, check=False)
    if proc.returncode:
        raise RuntimeError(f"{proc.args!r} failed ({proc.returncode}):\n"
                           + proc.stderr.decode(errors="replace"))
    return proc.stdout


def generate(cmd, path, header, source, visitor, no_builtins, directory):
    schema = json.loads(run(cmd, "--check", "--schema", "-", path, cwd=directory))
    if not header:
        return schema
    flags = ["--header", header]
    if source:
        flags += ["--source", source]
    if visitor:
        flags += ["--visitor", visitor]
    if no_builtins:
        flags += ["--no-builtins"]
    run(cmd, *flags, path, cwd=directory)
    outputs = [filename for filename in (header, source, visitor) if filename]
    for filename in outputs:
        if not (directory / filename).stat().st_size:
            raise RuntimeError(f"{filename} is empty")
    if source:
        run([options.cc], "-std=c11", "-c", source, "-o", "ast.o", cwd=directory)
    elif not no_builtins:
        (directory / "ast.c").write_text(f'#include "{header}"\n', encoding="ascii")
        run([options.cc], "-std=c11", "-c", "ast.c", "-o", "ast.o", cwd=directory)
    return schema


recipes = [(root / path, *args) for path, *args in recipes if (root / path).is_file()]
if not recipes:
    print("No downstream schemas in this checkout.")
    raise SystemExit(77)

legacy_script = root / "scripts/asdl.pl"
if not legacy_script.is_file():
    raise RuntimeError("missing repository compatibility entry point: scripts/asdl.pl")

with tempfile.TemporaryDirectory(prefix="asdl-downstream-", dir=options.work_dir) as scratch:
    for index, (path, *args) in enumerate(recipes):
        cwd = Path(scratch) / str(index)
        cwd.mkdir()
        reference = generate([options.perl, os.fspath(legacy_script)], path, *args, cwd)
        if options.native:
            cwd = cwd / "native"
            cwd.mkdir()
            actual = generate([os.fspath(options.native)], path, *args, cwd)
            if reference != actual:
                raise RuntimeError(f"native schema metadata differs from Perl for {path}")
        print(path.relative_to(root))
