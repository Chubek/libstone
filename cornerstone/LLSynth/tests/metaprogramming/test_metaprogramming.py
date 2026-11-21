#!/usr/bin/env python3
"""Compile-time AST expansion, diagnostics, and Python/native parity."""
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest


HERE = Path(__file__).resolve().parent
DRIVER = HERE.parents[1] / "scripts" / "llsynth.pl"
CC = shlex.split(os.environ.get("CC", "cc"))


def run(*args, cwd, data=None):
    result = subprocess.run(
        args, cwd=cwd, input=data, capture_output=True, timeout=60
    )
    if result.returncode:
        raise AssertionError(
            f"{shlex.join(map(str, args))} exited {result.returncode}\n"
            f"{result.stdout.decode(errors='replace')}"
            f"{result.stderr.decode(errors='replace')}"
        )
    return result.stdout


def spec(definitions="", action="return 0;", extra="", lower=None):
    if lower is not None:
        action += "\n'lower: " + lower
    return f""".language "Metaprogramming"
.skip /[ \\t\\n]+/
.token NUMBER: Int = /[0-9]+/
{definitions}
.rule Start
'syntax: value:NUMBER EOF
'semantics: {action}
.end
{extra}
"""


class MetaprogrammingTests(unittest.TestCase):
    def generate(self, root, source, target="python3", mode="interpret"):
        source_path = root / "main.llsynth"
        source_path.write_text(source)
        output = root / ("tool.py" if target == "python3" else "tool.c")
        run("perl", DRIVER, f"--mode={mode}", "--program-target=python3",
            "-T", target, "-o", output,
            source_path, cwd=root)
        if target == "python3":
            return (sys.executable, output)
        run(*CC, "-std=c11", "-pedantic-errors", output, "-lm", "-o",
            root / "tool", cwd=root)
        return (root / "tool",)

    def parity(self, source, expected, data=b"7\n", mode="interpret"):
        targets = ["python3"]
        if shutil.which(CC[0]):
            targets.append("c11")
        for target in targets:
            with self.subTest(target=target), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                tool = self.generate(root, source, target, mode)
                self.assertEqual(run(*tool, cwd=root, data=data), expected)

    def test_hygiene_nested_calls_and_once_only_arguments(self):
        self.parity(spec(
            """
.template Twice(x) = x + x
.template Offset(x, amount) = Twice(x) + amount
.state @sum: Int = Twice(1)
.state @count: Int = 0
.function bump() -> Int
@count = @count + 1;
return @count;
.end
.function identity(x: Int) -> Int
return x;
.end
.macro Accumulate(value)
let tmp = value;
for i in [1, 2] {
    let tmp = i;
    @sum = @sum + tmp;
}
@sum = @sum + tmp + tmp;
.end
.macro Nested(value)
let tmp = value;
Accumulate(tmp);
.end
""",
            """
let tmp = $value;
let i = 100;
let value = 200;
let identity = 999;
Nested(bump());
Accumulate(tmp);
print(tmp);
print(i);
print(value);
print(@count);
print(@sum);
return Offset(&identity(2), 1);
"""), b"7\n100\n200\n1\n24\n5\n")

    def test_definition_bindings_and_caller_argument_bindings(self):
        self.parity(spec(
            """
.template Size(x) = len(x)
.template Helper(x) = helper(x)
.function helper(x: Int) -> Int
return x + 1;
.end
.function test(len: Int) -> Int
return Size([len, len]) + Helper(len);
.end
.macro PrintSize(x)
let len = 300;
print(Size(x));
.end
""",
            """
let len = 7;
let helper = 10;
PrintSize([len, helper]);
return test(4);
"""), b"2\n7\n")

    def test_macro_returns_and_parameter_assignment_are_local(self):
        self.parity(spec(
            """
.macro Finish(x)
x = x + 1;
return x;
.end
.macro Ignore(x)
let y = 0;
.end
.state @count: Int = 0
.function bump() -> Int
@count = @count + 1;
return @count;
.end
""", "let x = 8; Ignore(bump()); print(@count); Finish(x);"),
            b"1\n9\n")

    def test_grab_init_and_caller_on_branch(self):
        self.parity(spec(
            """
.symtbl #items: Int -> Int
.macro Store(key, value)
#items[key] = value;
.end
.macro Fetch(key)
grab key from #items into Int(tmp);
let t'tmp = tmp;
let __lls_meta_1 = 2;
while __lls_meta_1 > 0 {
    __lls_meta_1 = __lls_meta_1 - 1;
}
if t'tmp > 0 {
    print(t'tmp);
} else {
    print(0);
}
.end
.init
Store(1, 9);
.end
""", """
let tmp = 7;
let t'tmp = 3;
let __lls_meta_1 = 5;
on value ->
Fetch(1);
-> end
return tmp + t'tmp + __lls_meta_1;
"""), b"9\n15\n")

    def test_all_action_facets_and_backend_templates(self):
        source = spec(
            """
.template Immediate(x) = x + 1
.macro Lower(x)
emit(@code, Stack.PUSH_F64(Immediate(x)));
.end
.state @code: Vec<Stack.Op> = []
.ir Stack
.op PUSH_F64(value: Float64): () -> (Float64)
.end
.backend python3 for Stack
.prologue "stack = []\\n"
.emit PUSH_F64(value) => "stack.append(${value})\\n"
.epilogue "print(stack[-1])\\n"
.end
""", "return Immediate($value);", lower="Lower($value); return @code;")
        for target in ("python3", "c11"):
            if target == "c11" and not shutil.which(CC[0]):
                continue
            with self.subTest(target=target), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                tool = self.generate(root, source, target, mode="both")
                self.assertEqual(run(*tool, cwd=root, data=b"7\n"), b"8\n")
                program = run(*tool, "--compile", cwd=root, data=b"7\n")
                self.assertIn(b"stack.append(8", program)
                (root / "program.py").write_bytes(program)
                self.assertEqual(run(sys.executable, root / "program.py",
                                     cwd=root), b"8\n")

    def test_deterministic_generation(self):
        source = spec(
            ".template Twice(x) = x + x\n.macro Save(x)\nlet temp = x;\nprint(temp);\n.end",
            "let temp = 9; Save(Twice($value)); return temp;")
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self.generate(root, source)
            first = (root / "tool.py").read_bytes()
            self.generate(root, source)
            self.assertEqual((root / "tool.py").read_bytes(), first)

    def test_expression_splicing_is_not_text_replacement(self):
        self.parity(spec(
            """
.template Twice(x) = x * x
.template Literal(x) = "x"
.template Unused(x) = 1
.state @count: Int = 0
.function bump() -> Int
@count = @count + 1;
return @count;
.end
""", """
print(Twice(2 + 3));
print(Literal(99));
print(Unused(bump()));
print(Twice(bump()));
return @count;
"""), b"25\nx\n1\n2\n2\n")

    def test_templates_in_rewrite_guards_and_replacements(self):
        source = spec(
            """
.template Fold(a, b) = a + b
.template Nonzero(a) = a != 0
.ir Stack
.op PUSH_F64(value: Float64): () -> (Float64)
.op ADD: (Float64, Float64) -> (Float64)
.end
.rewrite FoldPair: Stack
.match PUSH_F64(a), PUSH_F64(b), ADD
.when Nonzero(a)
.replace PUSH_F64(Fold(a, b))
.end
.backend python3 for Stack
.prologue "stack = []\\n"
.emit PUSH_F64(value) => "stack.append(${value})\\n"
.emit ADD => "stack.append(stack.pop() + stack.pop())\\n"
.epilogue "print(stack[-1])\\n"
.end
""", "return Fold($value, 1);",
            lower="return [Stack.PUSH_F64($value), Stack.PUSH_F64(1), Stack.ADD()];")
        for target in ("python3", "c11"):
            if target == "c11" and not shutil.which(CC[0]):
                continue
            with self.subTest(target=target), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                tool = self.generate(root, source, target, mode="both")
                program = run(*tool, "--compile", cwd=root, data=b"7\n")
                self.assertIn(b"stack.append(8", program)
                self.assertNotIn(b"stack.pop()", program)

    def test_bounded_expansion(self):
        cases = [
            ("depth",
             "\n".join(f".template T{i}(x) = T{i+1}(x)" for i in range(65))
             + "\n.template T65(x) = x", "return T0(1);",
             "expansion depth exceeds 64"),
            ("invocations", ".template T(x) = x",
             "T(1);" * 4097 + "return 0;",
             "expansion exceeds 4096 invocations"),
            ("copies", ".template T0(x) = x + x\n"
             + "\n".join(f".template T{i}(x) = T{i-1}(T{i-1}(x))"
                         for i in range(1, 12)),
             "return 0;", "expansion exceeds 100000 AST visits"),
        ]
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for name, definitions, action, message in cases:
                with self.subTest(case=name):
                    path = root / f"{name}.llsynth"
                    path.write_text(spec(definitions, action))
                    result = subprocess.run(
                        ["perl", DRIVER, "--check", "--mode=interpret", path],
                        cwd=root, capture_output=True, timeout=30)
                    self.assertEqual(result.returncode, 1)
                    self.assertIn(message, result.stderr.decode())

    def test_definition_and_call_diagnostics(self):
        cases = [
            ("duplicate", ".template T(x) = x\n.macro T(x)\nprint(x);\n.end",
             "return 0;", "duplicate compile-time declaration 'T'"),
            ("parameters", ".template T(x, x) = x", "return 0;",
             "duplicate parameter 'x'"),
            ("bad-parameter", ".template T(x,) = x", "return 0;",
             "invalid parameter ''"),
            ("arity", ".template T(x) = x", "return T();",
             "template 'T' expects 1 arguments, got 0"),
            ("macro-arity", ".macro M(x)\nprint(x);\n.end", "M(1, 2); return 0;",
             "macro 'M' expects 1 arguments, got 2"),
            ("direct-recursion", ".template T(x) = T(x)", "return 0;",
             "recursive expansion of 'T'"),
            ("indirect-recursion",
             ".macro A(x)\nB(x);\n.end\n.macro B(x)\nA(x);\n.end",
             "return 0;", "recursive expansion of 'A'"),
            ("free-local", ".macro M(x)\nprint(caller);\n.end",
             "let caller = 7; M(1); return 0;", "free name 'caller'"),
            ("free-capture", ".template T(x) = $value + x", "return 0;",
             "free capture '$value'"),
            ("duplicate-local", ".macro M(x)\nlet y = x;\nlet y = 2;\n.end",
             "return 0;", "duplicate local 'y'"),
            ("parameter-local", ".macro M(x)\nlet x = 2;\n.end",
             "return 0;", "duplicate local 'x'"),
            ("statement-only", ".macro M(x)\nprint(x);\n.end", "return M(1);",
             "requires a whole statement"),
            ("runtime-collision", ".template len(x) = x", "return 0;",
             "conflicts with runtime name"),
            ("unused-arity", ".template T(x) = x\n.template U(y) = T()",
             "return 0;", "expects 1 arguments, got 0"),
            ("parameter-collision", ".template T(T) = T", "return 0;",
             "parameter 'T' conflicts with compile-time declaration"),
            ("duplicate-caller-local", ".template T(x) = x",
             "let x = 1; let x = 2; return x;", "duplicate local 'x'"),
            ("invalid-table", ".macro M(x)\ngrab x from #bad into Int(y);\n.end",
             "return 0;", "unknown table '#bad' in definition"),
            ("invalid-exception", ".macro M(x)\nThrow(!bad, x);\n.end",
             "return 0;", "unknown exception '!bad' in definition"),
        ]
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for name, definitions, action, message in cases:
                with self.subTest(case=name):
                    path = root / f"{name}.llsynth"
                    path.write_text(spec(definitions, action))
                    result = subprocess.run(
                        ["perl", DRIVER, "--check", "--mode=interpret",
                         "--diagnostics=json", path],
                        cwd=root, capture_output=True, timeout=30)
                    self.assertEqual(result.returncode, 1)
                    diagnostic = json.loads(result.stderr)
                    self.assertIn(message, diagnostic["message"])
                    self.assertEqual(diagnostic["location"]["file"], str(path))
                    self.assertGreater(diagnostic["location"]["line"], 0)

    def test_include_definition_locations(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            included = root / "defs.llsynth"
            included.write_text(".template Bad(x) = missing + x\n")
            path = root / "main.llsynth"
            path.write_text(spec('.include "defs.llsynth"'))
            result = subprocess.run(
                ["perl", DRIVER, "--check", "--mode=interpret",
                 "--diagnostics=json", path],
                cwd=root, capture_output=True, timeout=30)
            self.assertEqual(result.returncode, 1)
            diagnostic = json.loads(result.stderr)
            self.assertEqual(diagnostic["location"]["file"], str(included))
            self.assertEqual(diagnostic["location"]["line"], 1)
            self.assertIn("free name 'missing'", diagnostic["message"])


if __name__ == "__main__":
    unittest.main()
