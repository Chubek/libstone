"""Exercise frontend artifacts, original locations, and CLI failure contracts."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile

cli, novoparse, gatebindle, build = sys.argv[1:]

BUNDLES = {
    "C": 12,
    "C++": 28,
    "Java": 29,
    "Rust": 28,
    "Python": 31,
    "Ruby": 30,
    "Delphi": 29,
    "Typescript": 29,
    "Scheme": 10,
}


def run(*args, status=0):
    result = subprocess.run(args, text=True, capture_output=True)
    assert result.returncode == status, (args, result.returncode, result.stdout, result.stderr)
    return result


def contains(node, name):
    if node["name"] == name:
        return True
    return any(contains(child, name) for child in node["children"])


with tempfile.TemporaryDirectory(dir=build) as directory:
    temporary = Path(directory)
    bundle = str(Path(gatebindle) / "C")
    first = run(cli, "compile", bundle).stdout
    assert first == run(cli, "compile", bundle).stdout
    descriptor = json.loads(first)
    assert descriptor["format"] == "gateml.frontend" and descriptor["version"] == 1
    assert descriptor["lowering"]["Multiply"] == "mul"
    assert len(descriptor["productions"]) == 12
    codes = [rule["code"] for rule in descriptor["lexer"] if not rule["skip"]]
    assert len(codes) == len(set(codes))
    grammar = temporary / "C.ng"
    grammar.write_text(run(cli, "grammar", bundle).stdout)
    run(novoparse, "check", str(grammar))
    run(novoparse, "generate", str(grammar), "--profile", "C", "-o", str(temporary / "C.h"))
    assert "C_create" in (temporary / "C.h").read_text()
    expression = temporary / "expression.c"
    expression.write_text("1 + 2 * 3\n")
    tree = json.loads(run(cli, "parse", bundle, str(expression)).stdout)
    assert tree["name"] == "Add" and tree["start"] == 0 and tree["end"] == 9
    ir = run(cli, "lower", bundle, str(expression)).stdout
    assert "mul" in ir and "add" in ir and "ret" in ir
    expression.write_text("1 +\n@")
    error = run(cli, "parse", bundle, str(expression), status=1).stderr
    assert f"{expression}:2:1: error:" in error
    definition = temporary / "bad.gml"
    definition.write_text("frontend Bad;\nlexer { token X = \"a\" }\n")
    error = run(cli, "check", str(definition), status=1).stderr
    assert f"{definition}:2:" in error
    broken_bundle = temporary / "bundle"
    broken_bundle.mkdir()
    (broken_bundle / "MANIFEST").write_text("Profile.gml\nLexer.gml\n")
    (broken_bundle / "Profile.gml").write_text("frontend Bad;\n")
    (broken_bundle / "Lexer.gml").write_text("lexer { token X = \"a\" }\n")
    error = run(cli, "check", str(broken_bundle), status=1).stderr
    assert f"{broken_bundle / 'Lexer.gml'}:1:" in error
    (broken_bundle / "MANIFEST").write_text("../bad.gml\n")
    assert "local .gml filenames" in run(cli, "check", str(broken_bundle), status=1).stderr
    run(cli, "unknown", status=2)
    run(cli, "lower", bundle, status=2)
    for command in ("check", "compile", "grammar", "parse", "lower"):
        assert "Usage:" in run(cli, command, "--help").stdout

    for name, productions in BUNDLES.items():
        bundle = str(Path(gatebindle) / name)
        run(cli, "check", bundle)
        first = run(cli, "compile", bundle).stdout
        assert first == run(cli, "compile", bundle).stdout
        descriptor = json.loads(first)
        assert descriptor["format"] == "gateml.frontend" and descriptor["version"] == 1
        assert descriptor["value_type"] == "i64"
        assert len(descriptor["productions"]) == productions, name
        codes = [rule["code"] for rule in descriptor["lexer"] if not rule["skip"]]
        assert len(codes) == len(set(codes)), name
        grammar = temporary / f"{descriptor['name']}.ng"
        grammar.write_text(run(cli, "grammar", bundle).stdout)
        run(novoparse, "check", str(grammar))
        run(novoparse, "generate", str(grammar), "--profile", "C",
            "-o", str(temporary / f"{descriptor['name']}.h"))
        sample = temporary / f"sample.{name}"
        sample.write_text("(+ 1 (* 2 3))\n" if name == "Scheme" else "1 + 2 * 3\n")
        tree = json.loads(run(cli, "parse", bundle, str(sample)).stdout)
        assert contains(tree, "Add"), name
        ir = run(cli, "lower", bundle, str(sample)).stdout
        assert "mul" in ir and "add" in ir and "ret" in ir, name

        assert descriptor["options"]["visitor_order"] == "postorder"
        handlers = {key[6:]: value for key, value in descriptor["options"].items()
                    if key.startswith("visit_")}
        assert handlers == descriptor["lowering"], name

    for name, source, kind in (("Typescript", "$x = 8 >>> 1", "Assign"),
                               ("Scheme", "(f -12)", "NamedCall")):
        bundle = str(Path(gatebindle) / name)
        sample.write_text(source)
        assert contains(json.loads(run(cli, "parse", bundle, str(sample)).stdout), kind)
        assert "unresolved lowering facility" in run(cli, "lower", bundle, str(sample), status=1).stderr

    sample.write_text("(- -12) ; comment\n")
    assert "ret" in run(cli, "lower", str(Path(gatebindle) / "Scheme"), str(sample)).stdout
    sample.write_text("(+ 1 2 3)")
    run(cli, "parse", str(Path(gatebindle) / "Scheme"), str(sample), status=1)

print("GateML CLI/artifact tests passed")
