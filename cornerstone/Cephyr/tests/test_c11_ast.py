"""Versioned C11 ASTs round-trip through the CLI and normal compiler."""

import copy
import json
from pathlib import Path
import subprocess
import sys
import tempfile


cephyr = Path(sys.argv[1]).resolve()


def run(*args, data=None, error=None, cwd=None):
    proc = subprocess.run([cephyr, *args], input=data, capture_output=True, cwd=cwd)
    if error is None:
        assert proc.returncode == 0, proc.stderr.decode()
    else:
        assert proc.returncode == 1 and error in proc.stderr, (proc.returncode, proc.stderr)
        assert not proc.stdout, proc.stdout
    return proc.stdout


def nodes(value):
    if isinstance(value, dict):
        if "kind" in value:
            yield value
        for item in value.values():
            yield from nodes(item)
    if isinstance(value, list):
        for item in value:
            yield from nodes(item)


with tempfile.TemporaryDirectory(prefix="cephyr-ast-") as directory:
    path = Path(directory)
    (path / "values.h").write_text("#define VALUE 42\n", encoding="utf-8")
    (path / "input.c").write_text(
        '#include "values.h"\n'
        '#line 90 "presumed.c"\n'
        'typedef int T;\n'
        'enum E { answer = VALUE };\n'
        'struct S { struct S *next; T value; };\n'
        'const char bytes[] = "\\377\\0\\"\\\\é";\n'
        'unsigned long long big = 18446744073709551615ULL;\n'
        'int f(void) { struct S s = { .value = answer };\n'
        '  return _Generic(s.value, T: s.value, default: 0); }\n',
        encoding="utf-8",
    )
    original = run("-S", "input.c", "-o", "-", cwd=path)
    json_text = run("--emit-ast=json", "input.c", cwd=path)
    document = json.loads(json_text)
    assert next(iter(document)) == "schema_version" and document["schema_version"] == 1
    assert document["ast"]["kind"] == "list"
    tree = list(nodes(document["ast"]))
    assert {"function_decl", "generic", "designated_init", "struct_type"} <= {
        node["kind"] for node in tree
    }
    assert any(node["attributes"]["bytes"] == [255, 0, 34, 92, 195, 169]
               for node in tree if node["kind"] == "string")
    assert any(node["attributes"]["value"] == "18446744073709551615"
               for node in tree if node["kind"] == "integer")
    assert "VALUE" not in document["source"]["text"]
    assert any(run["file"] == "presumed.c" and run["line"] == 90
               for run in document["source"]["line_map"])

    for fmt, prefix in (("json", b'{\n  "schema_version": 1'),
                        ("yaml", b"schema_version: 1\n"),
                        ("sexp", b"(1 ")):
        filename = f"tree.{fmt}"
        run(f"--emit-ast={fmt}", "input.c", "-o", filename, cwd=path)
        payload = (path / filename).read_bytes()
        assert payload.startswith(prefix), payload[:100]
        assert run("-S", filename, "-o", "-", cwd=path) == original
        assert run("-S", "-x", f"c11-ast-{fmt}", "-", "-o", "-", data=payload) == original
        assert run("--emit-ast=json", filename, cwd=path) == json_text
        # Wrong versions and trailing payloads fail in all three decoders.
        wrong = payload.replace(prefix, prefix.replace(b"1", b"2"), 1)
        run("-fsyntax-only", "-x", f"c11-ast-{fmt}", "-", data=wrong,
            error=b"C11Syntax.schema.json")
        run("-fsyntax-only", "-x", f"c11-ast-{fmt}", "-", data=payload + payload,
            error=b"cephyr.ast")

    run("-c", "tree.sexp", "-o", "tree.o", cwd=path)
    assert (path / "tree.o").read_bytes().startswith(b"\x7fELF")

    def reject(payload, error):
        run("-fsyntax-only", "-x", "c11-ast-json", "-", data=payload, error=error)

    bad = copy.deepcopy(document)
    bad["ast"]["items"][0]["kind"] = "nonexistent"
    reject(json.dumps(bad).encode(), b"C11Syntax.schema.json")
    bad = copy.deepcopy(document)
    bad["source"]["text"] = bad["source"]["text"].replace("42", "43")
    reject(json.dumps(bad).encode(), b"does not match")
    bad = copy.deepcopy(document)
    bad["source"]["line_map"][0]["physical"] = 2
    reject(json.dumps(bad).encode(), b"line map")
    reject(json_text.replace(b'"schema_version": 1', b'"schema_version": 2, "schema_version": 1'),
           b"duplicate JSON member")
    reject(b"[" * 300 + b"]" * 300, b"nesting")
    run("-fsyntax-only", "-x", "c11-ast-yaml", "-", data=b"schema_version: &v 1\nast: *v\n",
        error=b"YAML anchors")
    run("-fsyntax-only", "-x", "c11-ast-yaml", "-", data=b"schema_version: 1\nschema_version: 1\n",
        error=b"duplicate YAML")
    run("--emit-ast=json", "-", data=b"int bad(;", error=b"cephyr.parse")
    run("--emit-ast=json", "input.c", "-", data=b"int other;", cwd=path, error=b"exactly one")
    run("--emit-ast=xml", "-", data=b"int other;", error=b"unsupported AST format")
    run("--emit-ast=json", "-S", "-", data=b"int other;", error=b"conflicting compilation stages")

    vla = b'#line 12 "vla.c"\nint f(int n) { int a[n]; return sizeof(a); }\n'
    parsed = run("--emit-ast=json", "-x", "cpp-output", "-", data=vla)
    run("-fsyntax-only", "-x", "c11-ast-json", "-", data=parsed,
        error=b"vla.c:12: error[cephyr.type]: variable-length arrays")
