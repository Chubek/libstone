"""Inspect resolved C11 declarations from the Cephyr executable."""

import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile


TARGETS = ("x86_64-linux-gnu", "aarch64-linux-gnu", "riscv64-linux-gnu")
ABIS = ("SysVAMD64", "AAPCS64", "LP64")
STAGE = "--emit-interface=sexp"
SOURCE = """\
#include "counts.h"
typedef unsigned long Word;
typedef int Callback(int);
struct Hidden;
struct Node {
    char tag;
    unsigned int flags : FLAG_WIDTH;
    unsigned int more : 5;
    struct Node *next;
    Word payload[COUNT];
    unsigned char tail[];
};
union Choice { char small; long large; };
struct UnnamedBits { char first; unsigned int : 3; char last; };
typedef struct { short count; union { int value; float ratio; }; } Envelope;
enum Mode { negative = -2, computed = (1 << 4) + 1, following };
extern struct Hidden *opaque;
extern enum Mode mode;
extern const volatile Word external;
extern int unknown[];
static int private_object;
extern int private_object;
_Alignas(16) static int aligned;
int tentative[];
extern int tentative[];
extern int completed[];
int completed[4];
int initialized[] = {1, 2, 3};
char plain_char;
long double extended;
int no_prototype();
int empty_prototype(void);
int apply(const int values[static 4], Callback cb, ...);
int matrix(int rows[2][3]);
int adjusted(int values[const restrict 4]);
static int defined(int value) { int body_local = value; return body_local; }
inline int inline_only(int value) { return value; }
"""


def parse(text):
    """Read the v1 S-expression without depending on pretty-printing."""
    text = text.decode("utf-8")
    token = re.compile(r'\(|\)|"(?:[^"\\]|\\.)*"|[^\s()"]+')
    root = []
    stack = [root]
    end = 0
    for match in token.finditer(text):
        assert not text[end:match.start()].strip(), text
        end = match.end()
        word = match.group()
        if word == "(":
            child = []
            stack[-1].append(child)
            stack.append(child)
        elif word == ")":
            assert len(stack) > 1, text
            stack.pop()
        else:
            stack[-1].append(
                json.loads(word) if word.startswith('"') else
                int(word) if re.fullmatch(r"-?[0-9]+", word) else word
            )
    assert not text[end:].strip() and stack == [root] and len(root) == 1, text
    assert isinstance(root[0], list), root
    return root[0]


def forms(expr, name):
    return [item for item in expr if isinstance(item, list) and item and item[0] == name]


def one(expr, name):
    found = forms(expr, name)
    assert len(found) == 1, (name, expr)
    return found[0]


def value(expr, name):
    _, result = one(expr, name)
    return result


def named(expr, kind, name, index=1):
    found = [item for item in forms(expr, kind) if item[index] == name]
    assert len(found) == 1, (kind, name, found)
    return found[0]


def size_align(expr):
    layout = one(expr, "layout")
    return value(layout, "size"), value(layout, "align")


def check_declarations(document, target):
    amd64 = target == TARGETS[0]
    aarch64 = target == TARGETS[1]

    word = value(named(document, "typedef", "Word"), "type")
    assert word[:2] == ["integer", "unsigned-long"], word
    assert value(word, "rank") == 4 and value(word, "bits") == 64, word
    assert value(word, "signed") == "false", word
    assert (value(word, "size"), value(word, "align")) == (8, 8), word
    char = value(named(document, "object", "plain_char"), "type")
    assert char[:2] == ["integer", "char"], char
    assert value(char, "signed") == ("true" if amd64 else "false"), char
    real = value(named(document, "object", "extended"), "type")
    assert real[:2] == ["real", "long-double"], real
    assert value(real, "format") == ("x87" if amd64 else "binary128"), real
    assert value(real, "bits") == (80 if amd64 else 128), real
    assert (value(real, "size"), value(real, "align")) == (16, 16), real

    def tag(name):
        return named(document, "tag", name, index=3)

    hidden = tag("Hidden")
    assert hidden[2] == "struct" and value(hidden, "complete") == "false", hidden
    assert not forms(hidden, "layout"), hidden
    opaque = value(named(document, "object", "opaque"), "type")
    assert opaque[0] == "pointer" and value(opaque, "bits") == 64, opaque
    assert value(opaque, "pointee") == ["tag-ref", hidden[1]], opaque

    node = tag("Node")
    assert value(node, "complete") == "true" and size_align(node) == (40, 8), node
    fields = forms(node, "field")
    assert [(field[1], value(field, "offset")) for field in fields] == [
        ("tag", 0), ("flags", 1), ("more", 1), ("next", 8), ("payload", 16), ("tail", 40)
    ], fields
    for name, width, shift in (("flags", 3, 0), ("more", 5, 3)):
        bits = one(named(node, "field", name), "bit-field")
        assert (value(bits, "width"), value(bits, "shift"), value(bits, "container-offset")) == (
            width, shift, 0
        ), bits
    next_type = value(named(node, "field", "next"), "type")
    assert value(next_type, "pointee") == ["tag-ref", node[1]], next_type
    payload = value(named(node, "field", "payload"), "type")
    assert payload[0] == "array" and value(payload, "count") == 3, payload
    assert value(payload, "element") == word, payload
    tail = named(node, "field", "tail")
    assert value(tail, "flexible-array") == "true", tail
    assert value(value(tail, "type"), "count") == "unknown", tail

    choice = tag("Choice")
    assert choice[2] == "union" and size_align(choice) == (8, 8), choice
    assert [value(field, "offset") for field in forms(choice, "field")] == [0, 0], choice
    unnamed_bits = tag("UnnamedBits")
    assert size_align(unnamed_bits) == ((4, 4) if aarch64 else (3, 1)), unnamed_bits
    assert value(named(unnamed_bits, "field", "none"), "offset") == 1, unnamed_bits

    envelope = value(named(document, "typedef", "Envelope"), "type")
    assert envelope[0] == "tag-ref", envelope
    anonymous_struct = named(document, "tag", envelope[1])
    assert anonymous_struct[2:4] == ["struct", "none"], anonymous_struct
    anonymous_field = named(anonymous_struct, "field", "none")
    assert value(anonymous_field, "offset") == 4, anonymous_field
    anonymous_union = named(document, "tag", value(anonymous_field, "type")[1])
    assert anonymous_union[2:4] == ["union", "none"], anonymous_union
    assert [field[1] for field in forms(anonymous_union, "field")] == ["value", "ratio"]

    mode = tag("Mode")
    assert mode[2] == "enum" and value(mode, "complete") == "true", mode
    assert [item[1:] for item in forms(mode, "enumerator")] == [
        ["negative", -2], ["computed", 17], ["following", 18]
    ], mode
    assert value(named(document, "object", "mode"), "type") == ["tag-ref", mode[1]]

    for name, linkage, storage, definition, layout in (
        ("external", "external", ["extern"], "declaration", (8, 8)),
        ("unknown", "external", ["extern"], "declaration", ("unknown", 4)),
        ("private_object", "internal", ["static", "extern"], "tentative", (4, 4)),
        ("aligned", "internal", ["static"], "tentative", (4, 16)),
        ("tentative", "external", ["none", "extern"], "tentative", (4, 4)),
        ("completed", "external", ["extern", "none"], "tentative", (16, 4)),
        ("initialized", "external", ["none"], "defined", (12, 4)),
    ):
        obj = named(document, "object", name)
        assert value(obj, "linkage") == linkage and one(obj, "storage")[1:] == storage, obj
        assert value(obj, "definition") == definition and size_align(obj) == layout, obj
    qualified = value(named(document, "object", "external"), "type")
    assert qualified[0] == "qualified", qualified
    assert set(one(qualified, "qualifiers")[1:]) == {"const", "volatile"}, qualified
    assert one(qualified, "integer") == word, qualified
    for name, bound in (("unknown", "unknown"), ("tentative", 1), ("completed", 4), ("initialized", 3)):
        array = value(named(document, "object", name), "type")
        assert array[0] == "array" and value(array, "count") == bound, array

    def signature(name):
        return value(named(document, "function", name), "type")

    for name, prototype in (("no_prototype", "false"), ("empty_prototype", "true")):
        function = signature(name)
        assert value(function, "prototype") == prototype, function
        assert one(function, "parameters") == ["parameters"], function

    apply = signature("apply")
    assert value(apply, "prototype") == "true" and value(apply, "variadic") == "true", apply
    assert value(apply, "result")[:2] == ["integer", "int"], apply
    _, values, callback = one(apply, "parameters")
    assert values[0] == "pointer" and callback[0] == "pointer", apply
    pointee = value(values, "pointee")
    assert pointee[0] == "qualified" and value(pointee, "qualifiers") == "const", pointee
    assert one(pointee, "integer")[:2] == ["integer", "int"], pointee
    assert value(callback, "pointee") == value(named(document, "typedef", "Callback"), "type")
    function = named(document, "function", "apply")
    assert value(function, "calling-convention") == (
        "sysv" if amd64 else "aapcs64" if aarch64 else "lp64"
    ), function
    assert one(function, "parameter-names") == ["parameter-names", "values", "cb"], function

    _, matrix = one(signature("matrix"), "parameters")
    assert matrix[0] == "pointer", matrix
    row = value(matrix, "pointee")
    assert row[0] == "array" and value(row, "count") == 3, row
    _, adjusted = one(signature("adjusted"), "parameters")
    assert adjusted[0] == "pointer" and value(adjusted, "pointee")[:2] == ["integer", "int"], adjusted

    defined = named(document, "function", "defined")
    assert value(defined, "linkage") == "internal" and value(defined, "definition") == "defined"
    inline = named(document, "function", "inline_only")
    assert value(inline, "linkage") == "external" and value(inline, "definition") == "inline-only"
    assert "inline" in one(inline, "specifiers")[1:], inline


def main():
    cephyr = Path(sys.argv[1]).resolve()
    env = os.environ.copy()
    env.pop("CEPHYR_CONFIG", None)
    env["LC_ALL"] = "C"

    with tempfile.TemporaryDirectory(prefix="cephyr-interface-") as directory:
        work = Path(directory)
        (work / "counts.h").write_text("#define COUNT 3\n#define FLAG_WIDTH 3\n",
                                     encoding="utf-8")
        (work / "input.c").write_text(SOURCE, encoding="utf-8")

        def run(*args, data=None, error=None):
            proc = subprocess.run([cephyr, *args], input=data, capture_output=True,
                                  cwd=work, env=env)
            if error is None:
                assert proc.returncode == 0, (args, proc.stderr)
            else:
                assert proc.returncode == 1 and error in proc.stderr, (
                    args, proc.returncode, proc.stderr)
                assert not proc.stdout, proc.stdout
            return proc.stdout

        for target, abi in zip(TARGETS, ABIS):
            document = parse(run(f"--target={target}", STAGE, "input.c"))
            assert document[:2] == ["interface", 1], document
            assert value(document, "target") == target and value(document, "abi") == abi, document
            assert value(document, "data-model") == "LP64", document
            check_declarations(document, target)

        target = f"--target={TARGETS[0]}"
        direct = parse(run(target, STAGE, "input.c"))
        assert parse(run(target, STAGE, "input.c")) == direct
        assert run(target, STAGE, "input.c", "-o", "result.interface") == b""
        assert parse((work / "result.interface").read_bytes()) == direct
        ast = run(target, "--emit-ast=json", "input.c")
        assert parse(run(target, STAGE, "-x", "c11-ast-json", "-", data=ast)) == direct
        run(target, STAGE, "-x", "c11-ast-json", "-", data=ast + ast, error=b"cephyr.ast")
        preprocessed = run(target, "-E", "input.c")
        assert parse(run(target, STAGE, "-x", "cpp-output", "-", data=preprocessed)) == direct

        # Same-spelled tags introduced independently in prototypes are
        # distinct from each other and from the completed file-scope tag.
        scoped_tags = b"""\
void first(struct Tag *value);
void second(struct Tag *value);
struct Tag;
void file_scope(struct Tag *value);
struct Tag { int member; };
"""
        tags = parse(run(target, STAGE, "-", data=scoped_tags))
        ids = []
        for name in ("first", "second", "file_scope"):
            signature = value(named(tags, "function", name), "type")
            _, parameter = one(signature, "parameters")
            ids.append(value(parameter, "pointee")[1])
        assert len(set(ids)) == 3, tags
        assert [value(named(tags, "tag", tag), "complete") for tag in ids] == [
            "false", "false", "true",
        ], tags
        names = parse(run(target, STAGE, "-", data=b"""\
int named(int first, int);
int named(int other, int first);
int named(int x, int y) { return x + y; }
"""))
        assert one(named(names, "function", "named"), "parameter-names") == [
            "parameter-names", "x", "y",
        ], names

        # Function bodies are parsed, but not lowered to native MachineIR.
        vla = b"int body(int n) { int body_only[n]; return sizeof body_only; }\n"
        interface = run(target, STAGE, "-", data=vla)
        assert b'"body"' in interface and b"body_only" not in interface, interface
        run(target, "-fsyntax-only", "-", data=vla, error=b"variable-length arrays")

        # A declaration can be valid even if codegen's static image limit
        # prevents emission. The projection should not allocate that image.
        oversized = b"char large[5 * 1024 * 1024] = {1};\n"
        large = named(parse(run(target, STAGE, "-", data=oversized)), "object", "large")
        assert value(large, "definition") == "defined" and size_align(large) == (5 * 1024 * 1024, 1)
        run(target, "-S", "-", "-o", "-", data=oversized, error=b"cephyr.global.initializer-limit")

        run(target, STAGE, "-", data=b"#error projection preprocess failure\n",
            error=b"projection preprocess failure")
        run(target, STAGE, "-", data=b"int bad(;\n", error=b"cephyr.parse")
        run(target, STAGE, "-", data=b"int incompatible; long incompatible;\n",
            error=b"conflicting types")
        run(target, STAGE, "-", data=b"int observed; int not_constant = observed;\n",
            error=b"arithmetic constant expression")
        bad = b'#line 42 "assertion.h"\n_Static_assert(0, "projection validation");\n'
        run(target, STAGE, "-", data=bad, error=b"assertion.h:42:")
        output = work / "result.interface"
        output.write_bytes(b"previous output\n")
        run(target, STAGE, "-", "-o", output, data=bad, error=b"projection validation")
        assert output.read_bytes() == b"previous output\n"
        run(STAGE, "input.c", "-", data=b"int another;\n", error=b"exactly one")
        run(STAGE, error=b"exactly one")
        run(STAGE, "-x", "assembler", "-", data=b".text\n", error=b"cephyr.option")
        run("--emit-interface=json", "-", data=b"int another;\n", error=b"cephyr.option")
        for stage in ("-E", "-fsyntax-only", "--emit-ast=sexp", "--emit-mir", "-S", "-c"):
            run(STAGE, stage, "-", data=b"int another;\n", error=b"conflicting compilation stages")


if __name__ == "__main__":
    main()
