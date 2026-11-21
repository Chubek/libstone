"""Execution DSL regressions; only the Python standard library is required."""
import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import unittest


DRIVER = Path(__file__).resolve().parents[2] / "scripts" / "llsynth.pl"


class ExecutionTests(unittest.TestCase):
    def setUp(self):
        temp_root = "/tmp/opencode" if Path("/tmp/opencode").is_dir() else None
        directory = tempfile.TemporaryDirectory(prefix="llsynth-execution-", dir=temp_root)
        self.addCleanup(directory.cleanup)
        self.work = Path(directory.name)
        self.counter = 0

    def spec(self, action, declarations=""):
        self.counter += 1
        path = self.work / f"case{self.counter}.llsynth"
        path.write_text(
            '.language "Execution"\n.token N: Int = /[0-9]+/\n'
            + declarations
            + "\n.rule Start\n'syntax: n:N EOF\n'semantics:\n"
            + action + "\n.end\n", encoding="utf-8")
        return path

    def generate(self, action, declarations="", *, error=None, target="python3", check=False):
        path = self.spec(action, declarations)
        output = path.with_suffix(".py")
        cmd = [str(DRIVER), "--mode=interpret", "-T", target]
        cmd += ["--check"] if check else ["-o", str(output)]
        result = subprocess.run(cmd + [str(path)], text=True, capture_output=True, timeout=20)
        if error:
            self.assertNotEqual(result.returncode, 0, result.stdout)
            self.assertIn(error, result.stderr)
            self.assertFalse(output.exists())
            return
        self.assertEqual(result.returncode, 0, result.stderr)
        if check: return result
        name = f"execution_case_{self.counter}"
        loader = importlib.util.spec_from_file_location(name, output)
        module = importlib.util.module_from_spec(loader)
        sys.modules[name] = module
        self.addCleanup(sys.modules.pop, name, None)
        loader.loader.exec_module(module)
        return module

    def test_escaping_mutable_cells_and_independent_factories(self):
        module = self.generate("""
let a = counter($n);
let b = counter(100);
return a() + a() + b();
""", """.function counter(start: Int) -> Fn()->Int
let x = start;
return fn() -> Int { x = x + 1; return x; };
.end
""")
        self.assertEqual(module.Context().interpret("10"), 124)

    def test_shared_capture_assignment_and_parameter_shadowing(self):
        module = self.generate("""
let x = $n;
let read = fn() -> Int { return x; };
x = 30;
let shadow = fn(x: Int) -> Int { return x + read(); };
return shadow(2);
""")
        self.assertEqual(module.Context().interpret("10"), 32)

    def test_sibling_closures_share_an_escaping_binding(self):
        module = self.generate("""
let pair = factory($n);
discard pair[0]();
return pair[1]();
""", """.function factory(value: Int) -> Vec<Fn()->Int>
let increment = fn() -> Int { value = value + 1; return value; };
let read = fn() -> Int { return value; };
return [increment, read];
.end
""")
        self.assertEqual(module.Context().interpret("8"), 9)

    def test_closure_local_shadow_and_nested_higher_order_calls(self):
        module = self.generate("""
let x = 10;
let factory = fn(x: Int) -> Fn(Int)->Int {
    return fn(y: Int) -> Int { let x = 20; return x + y; };
};
let op = factory(5);
return apply(op, x);
""", """.function apply(op: Fn(Int)->Int, x: Int) -> Int
return op(x);
.end
""")
        self.assertEqual(module.Context().interpret("1"), 30)

    def test_closure_in_vector_and_capture_of_grammar_value(self):
        module = self.generate("""
let ops = [fn(x: Int) -> Int { return x + $n; }];
return ops[0](2);
""")
        self.assertEqual(module.Context().interpret("40"), 42)

    def test_async_returns_await_and_async_closure(self):
        module = self.generate("""
let twice = async fn(x: Int) -> Int { return await plus(x) + 1; };
let work: Task<Int> = twice($n);
return join(spawn(work));
""", """.async function plus(x: Int) -> Int
return x + 1;
.end
""")
        self.assertEqual(module.Context().interpret("40"), 42)

    def test_lazy_task_snapshot_and_state_tables_arguments_isolation(self):
        module = self.generate("""
let values = [5];
#names[1] = 7;
let task = change(values);
@value = 100;
values[0] = 50;
#names[1] = 70;
let result = await task;
return result + @value + values[0] + #names[1];
""", """.state @value: Int = 3
.symtbl #names: Int -> Int
.async function change(values: Vec<Int>) -> Int
values[0] = values[0] + @value + #names[1];
@value = 9;
#names[1] = 10;
return values[0];
.end
""")
        self.assertEqual(module.Context().interpret("1"), 235)

    def test_async_capture_snapshot_and_result_closure(self):
        module = self.generate("""
let x = 10;
let factory = async fn() -> Fn()->Int {
    x = x + 1;
    return fn() -> Int { return x; };
};
let task = factory();
x = 50;
let result = await task;
return result() + x;
""")
        self.assertEqual(module.Context().interpret("1"), 61)

    def test_only_free_bindings_are_captured_into_tasks(self):
        module = self.generate("""
let x = work();
let runner = async fn(x: Int) -> Int {
    let inner = fn() -> Int { return x; };
    return inner();
};
let local = async fn() -> Int { let x = 20; return x; };
return await runner(10) + await local();
""", """.async function work() -> Int
return 1;
.end
""")
        self.assertEqual(module.Context().interpret("1"), 30)

    def test_function_and_task_aliases(self):
        module = self.generate("""
let add: %Operation = fn(x: %Number) -> %Number { return x + $n; };
let t: %Work = later(add);
return await t;
""", """.type %Number = Int
.type %Operation = Fn(%Number)->%Number
.type %Work = Task<%Number>
.async function later(operation: %Operation) -> %Number
return operation(2);
.end
""")
        self.assertEqual(module.Context().interpret("40"), 42)

    def test_new_builtin_names_can_still_be_user_functions(self):
        module = self.generate("return sleep($n, 1);", """.function sleep(value: Int, increment: Int) -> Int
return value + increment;
.end
""")
        self.assertEqual(module.Context().interpret("40"), 41)
        self.generate("return sleep($n, 1);", """.function sleep(value: Int, increment: Int) -> Int
return value + increment;
.end
""", target="c11", check=True)

    def test_explicit_named_function_reference_survives_local_shadow(self):
        module = self.generate("""
let operation = fn() -> Int { return 10; };
let global = &operation;
return global(2) + operation();
""", """.function operation(x: Int) -> Int
return x + 1;
.end
""")
        self.assertEqual(module.Context().interpret("1"), 13)

    def test_awaited_exception_and_unobserved_started_exception(self):
        declarations = """.exception !bad(value: Int) "bad {value}"
.async function fail(x: Int) -> Int
Throw(!bad, x);
.end
"""
        for action in ("return await fail($n);", "discard spawn(fail($n)); return 0;"):
            module = self.generate(action, declarations)
            with self.assertRaisesRegex(module.LanguageError, r"!bad: bad 7"):
                module.Context().interpret("7")

    def test_real_concurrency_with_barrier_not_timing_only(self):
        module = self.generate("""
let first = spawn(wait($n));
let second = spawn(wait($n + 1));
return await first + join(second);
""", """.async function wait(x: Int) -> Int
discard sleep(0.01);
return x;
.end
""")
        # Sequential fake spawn would time out. Both task threads must reach this
        # real barrier before either can complete.
        barrier = threading.Barrier(2, timeout=3)
        tids = set()
        class Clock:
            @staticmethod
            def sleep(seconds):
                tids.add(threading.get_ident())
                barrier.wait()
        module.time = Clock
        self.assertEqual(module.Context().interpret("20"), 41)
        self.assertEqual(len(tids), 2)

    def test_contexts_and_executions_are_isolated(self):
        module = self.generate("return fn() -> Int { return $n; };")
        first, second = module.Context(), module.Context()
        closure = first.interpret("8")
        with self.assertRaisesRegex(module.LanguageError, "another context or execution"):
            second.invoke(closure, [])
        self.assertEqual(first.invoke(closure, []), 8)
        first.interpret("9")
        with self.assertRaisesRegex(module.LanguageError, "another context or execution"):
            first.invoke(closure, [])

    def test_task_handles_do_not_cross_task_boundaries(self):
        module = self.generate("""
let t = work();
let call = async fn() -> Int { return await t; };
return await call();
""", """.async function work() -> Int
return 1;
.end
""")
        with self.assertRaisesRegex(module.LanguageError, "task handles cannot cross"):
            module.Context().interpret("1")

    def test_structured_join_and_repeated_join_result_copies(self):
        module = self.generate("""
let t = work();
let a = join(t);
a[0] = 99;
let b = join(t);
discard spawn(work());
return b[0];
""", """.async function work() -> Vec<Int>
discard sleep(0.02);
return [5];
.end
""")
        ctx = module.Context()
        self.assertEqual(ctx.interpret("1"), 5)
        self.assertTrue(all(t.thread is None or not t.thread.is_alive() for t in ctx.budget.tasks))

    def test_validation_errors(self):
        cases = [
            ("let f = fn(x: Int) -> Int { return x; }; return f();", "expects 1 arguments"),
            ("return await 1;", "await requires a Task"),
            ("return spawn(1);", "spawn requires a Task"),
            ("return fn(x: Int, x: Int) -> Int { return x; };", "duplicate closure parameter"),
            ("return fn() -> Int { return missing; };", "undeclared variable"),
            ("return fn() -> Int { return true; };", "type mismatch in return"),
            ("return fn() -> Int { discard 1; };", "may finish without returning"),
        ]
        for action, error in cases:
            with self.subTest(action=action):
                self.generate(action, error=error, check=True)

    def test_runtime_arity_and_sleep_errors(self):
        module = self.generate("return fn(x: Int) -> Int { return x; };")
        ctx = module.Context()
        closure = ctx.interpret("1")
        with self.assertRaisesRegex(module.LanguageError, "expects 1 arguments"):
            ctx.invoke(closure, [])
        module = self.generate("discard sleep(-1); return 0;")
        with self.assertRaisesRegex(module.LanguageError, "sleep requires finite"):
            module.Context().interpret("1")

    def test_task_budget_and_shared_instruction_budget(self):
        module = self.generate("""
let i = 0;
while i < 65 { discard work(); i = i + 1; }
return 0;
""", """.async function work() -> Int
return 1;
.end
""")
        with self.assertRaisesRegex(module.LanguageError, "task creation limit"):
            module.Context().interpret("1")
        module = self.generate("return await work();", """.async function work() -> Int
while true { discard 1; }
return 0;
.end
""")
        with self.assertRaisesRegex(module.LanguageError, "instruction budget exhausted"):
            module.Context().interpret("1")

    def test_native_capability_rejection_check_and_generate(self):
        features = [
            ("return fn() -> Int { return 1; };", ""),
            ("return 1;", ".async function f() -> Int\nreturn 1;\n.end\n"),
            ("return await f();", ".async function f() -> Int\nreturn 1;\n.end\n"),
        ]
        for target in ("c99", "c11", "cpp17", "cpp20"):
            for check in (False, True):
                for action, declarations in features:
                    self.generate(action, declarations, target=target, check=check,
                                  error="require the python3 tool target")


if __name__ == "__main__":
    unittest.main()
