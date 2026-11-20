"""Cross-feature regressions for the generated action language."""

import importlib.util
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest


DRIVER = Path(__file__).resolve().parents[2] / "scripts" / "llsynth.pl"


class FeatureIntegrationTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(
            prefix="llsynth-integration-",
            dir="/tmp/opencode" if Path("/tmp/opencode").is_dir() else None,
        )
        self.addCleanup(temporary.cleanup)
        self.work = Path(temporary.name)

    def generate(self, declarations, action, target="python3", error=None):
        source = self.work / "features.llsynth"
        source.write_text(
            '.language "Features"\n.token NUMBER: Int = /[0-9]+/\n'
            + declarations
            + "\n.rule Start\n'syntax: value:NUMBER EOF\n'semantics:\n"
            + action + "\n..\n",
            encoding="utf-8",
        )
        output = self.work / ("features.py" if target == "python3" else "features.c")
        result = subprocess.run(
            [str(DRIVER), "--mode=interpret", "-T", target,
             "-o", str(output), str(source)],
            capture_output=True, text=True, timeout=30,
        )
        if error:
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(error, result.stderr)
            self.assertFalse(output.exists())
            return
        self.assertEqual(result.returncode, 0, result.stderr)
        if target != "python3":
            return output
        name = "llsynth_integration_features"
        spec = importlib.util.spec_from_file_location(name, output)
        module = importlib.util.module_from_spec(spec)
        sys.modules[name] = module
        self.addCleanup(sys.modules.pop, name, None)
        spec.loader.exec_module(module)
        return module

    def test_template_closure_preserves_caller_binding(self):
        module = self.generate(
            ".template Adder(base) = fn(value: Int) -> Int { return base + value; }\n",
            "let value = $value; let add = Adder(value); return add(2);",
        )
        self.assertEqual(module.interpret("40"), 42)

    def test_macro_closure_retains_mutable_cells(self):
        module = self.generate(
            """.state @total: Int = 0
.macro Accumulate(callback)
let value = callback();
@total = @total + value;
.end
.template Identity(value) = value
.function counter(value: Int) -> Fn()->Int
return fn() -> Int { value = value + 1; return Identity(value); };
.end
""",
            """let value = $value;
let next = counter(value);
Accumulate(next);
Accumulate(next);
return @total + value;""",
        )
        self.assertEqual(module.interpret("10"), 33)

    def test_async_function_parameters_and_macro_spawn(self):
        module = self.generate(
            """.template Ready(task) = await task
.macro Start(task)
discard spawn(task);
.end
.async function compute(value: Int) -> Int
let add = fn(value: Int) -> Int { return value + 2; };
return add(value);
.end
""",
            "let task = compute($value); Start(task); return Ready(task);",
        )
        self.assertEqual(module.interpret("40"), 42)

    def test_anonymous_async_template_and_hygiene(self):
        module = self.generate(
            ".template AsyncAdder(base) = async fn(value: Int) -> Int { return base + value; }\n",
            "let value = $value; let add = AsyncAdder(value); return await add(2);",
        )
        self.assertEqual(module.interpret("40"), 42)

    def test_native_rejects_closure_after_template_expansion(self):
        self.generate(
            ".template Adder(base) = fn(value: Int) -> Int { return base + value; }\n",
            "let add = Adder($value); return add(2);",
            target="c11",
            error="require the python3 tool target",
        )

    def native_library(self):
        compiler = shlex.split(os.environ.get("CC", "cc"))
        if not compiler or not shutil.which(compiler[0]):
            self.skipTest("C compiler unavailable")
        source = self.work / "native.c"
        source.write_text(
            "#include <stdint.h>\n"
            "int64_t feature_add(int64_t a, int64_t b) { return a + b; }\n",
            encoding="utf-8",
        )
        library = self.work / "libfeatures.so"
        result = subprocess.run(
            [*compiler, "-std=c11", "-fPIC", "-shared", str(source),
             "-o", str(library)],
            capture_output=True, text=True, timeout=30,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        return library

    def test_native_import_in_async_template(self):
        library = self.native_library()
        module = self.generate(
            f'.extern c native_add(a: Int, b: Int) -> Int from "{library}" symbol "feature_add"\n'
            + """.template AddTwo(value) = native_add(value, 2)
.async function compute(value: Int) -> Int
return AddTwo(value);
.end
""",
            "let task = compute($value); discard spawn(task); return await task;",
        )
        self.assertEqual(module.interpret("40"), 42)

    @unittest.skipUnless(os.environ.get("LLSYNTH_SANDSTONE_BUILD"),
                         "set LLSYNTH_SANDSTONE_BUILD for the all-features test")
    def test_all_features_with_real_sandstone(self):
        library = self.native_library()
        build = Path(os.environ["LLSYNTH_SANDSTONE_BUILD"]).resolve()
        bank = self.work / "instructions.symas"
        result = subprocess.run(
            [str(build / "mkbank"), str(bank)],
            capture_output=True, text=True, timeout=30,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        (self.work / "all.llsynth").write_text(
            '.module AllFeatures\n.export Compute\n'
            f'.extern c native_add(a: Int, b: Int) -> Int from "{library}" symbol "feature_add"\n'
            f'.sandstone assemble bank "{bank}" syntax "intel" cli "{build / "sandstone-as"}"\n'
            + """.state @total: Int = 0
.template Bump(value) = native_add(value, 1)
.macro Accumulate(value)
let temporary = Bump(value);
@total = @total + temporary;
.end
.async function Compute(value: Int) -> Int
let bytes = assemble("nop\\nret\\n");
let adder = fn(delta: Int) -> Int { return value + delta; };
Accumulate(adder(len(bytes)));
return @total;
.end
""",
            encoding="utf-8",
        )
        module = self.generate(
            """.import "all.llsynth" as A
.macro Start(task)
discard spawn(task);
.end
""",
            """let a = A__Compute($value);
let b = A__Compute($value + 1);
Start(a);
Start(b);
return (await a) + (await b);""",
        )
        self.assertEqual(module.interpret("38"), 83)

    def test_exported_macros_closures_and_async_helpers(self):
        (self.work / "helpers.llsynth").write_text(
            """.module Helpers
.export Adder, Accumulate, compute, @total
.state @total: Int = 0
.template AddTwo(value) = value + 2
.template Adder(base) = fn(delta: Int) -> Int { return base + delta; }
.macro Accumulate(value)
let temp = AddTwo(value);
@total = @total + temp;
.end
.async function compute(value: Int) -> Int
let add = Adder(value);
return add(2);
.end
""",
            encoding="utf-8",
        )
        module = self.generate(
            '.import "helpers.llsynth" as H\n.import "./helpers.llsynth" as H2\n',
            """H__Accumulate($value);
let add = H2__Adder(@H__total);
let task = H__compute(add(0));
discard spawn(task);
return await task;""",
        )
        self.assertEqual(module.interpret("38"), 42)

    def test_exported_native_import_with_macro_and_async(self):
        (self.work / "native.llsynth").write_text(
            """.module Native
.export unused, compute, Assemble
.extern c unused(value: Int) -> Int from "library-does-not-exist.so" symbol "native_unused"
.sandstone Assemble bank "bank-does-not-exist.symas"
.template AddTwo(value) = value + 2
.async function compute(value: Int) -> Int
return AddTwo(value);
.end
""",
            encoding="utf-8",
        )
        module = self.generate(
            '.import "native.llsynth" as N\n',
            "return await N__compute($value);",
        )
        self.assertEqual(module.interpret("40"), 42)
        foreign = {
            f["symbol"]: f for f in module.SPEC["functions"]
            if f.get("foreign") == "c"
        }
        self.assertEqual(foreign["native_unused"]["library"], "library-does-not-exist.so")
        self.assertTrue(foreign["native_unused"]["name"].startswith("LLSMod_"))
        assembler = next(f for f in module.SPEC["functions"]
                         if f.get("foreign") == "sandstone")
        self.assertEqual(assembler["bank"], "bank-does-not-exist.symas")
        self.assertTrue(assembler["name"].startswith("LLSMod_"))

    def test_imported_macro_native_c11(self):
        compiler = shlex.split(os.environ.get("CC", "cc"))
        if not compiler or not shutil.which(compiler[0]):
            self.skipTest("C compiler unavailable")
        (self.work / "math.llsynth").write_text(
            """.module Math
.export Add, @total
.state @total: Int = 0
.template AddTwo(value) = value + 2
.macro Add(value)
let temporary = AddTwo(value);
@total = @total + temporary;
.end
""",
            encoding="utf-8",
        )
        source = self.generate(
            '.import "math.llsynth" as M\n',
            "M__Add($value); return @M__total;",
            target="c11",
        )
        executable = self.work / "features"
        build = subprocess.run(
            [*compiler, "-std=c11", "-pedantic-errors", str(source),
             "-lm", "-o", str(executable)],
            capture_output=True, text=True, timeout=30,
        )
        self.assertEqual(build.returncode, 0, build.stderr)
        result = subprocess.run(
            [str(executable)], input="40", text=True,
            capture_output=True, timeout=30,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, "42\n")


if __name__ == "__main__":
    unittest.main()
