"""Driver integration and negative tests for the lcephyr sandbox."""

import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

CEPHYR = str(Path(sys.argv.pop(1)).resolve())


class LuaExtensions(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name)

    def script(self, data, name="extension.lua"):
        path = self.root / name
        path.write_bytes(data.encode() if isinstance(data, str) else data)
        return path

    def run_cephyr(self, *args, source="int answer = 42;\n"):
        return subprocess.run(
            [CEPHYR, *args, "-x", "c", "-"],
            cwd=self.root, input=source, text=True, capture_output=True, timeout=8,
            env=dict(os.environ, CEPHYR_LUA_PRIVATE_TEST="not granted"),
        )

    def test_sandbox_and_explicit_environment(self):
        script = self.script("""
            local sdk = require('lcephyr')
            assert(type(sdk) == 'table')
            assert(sdk.env.get('BUILD_MODE') == 'checked')
            assert(sdk.env.get('EMPTY') == '')
            assert(sdk.env.get('CEPHYR_LUA_PRIVATE_TEST') == nil)
            local names = sdk.env.list()
            assert(#names == 2 and names[1] == 'BUILD_MODE' and names[2] == 'EMPTY')
            assert(os == nil and io == nil and package == nil and debug == nil)
            assert(load == nil and loadfile == nil and dofile == nil)
            assert(coroutine == nil and pcall == nil and xpcall == nil)
            assert(print == nil and string.dump == nil)
            assert(sdk.set_text == nil)
        """)
        res = self.run_cephyr("-E", "-P", "--lua-extension", script,
                              "--lua-env=BUILD_MODE=checked", "--lua-env", "EMPTY=")
        self.assertEqual(res.returncode, 0, res.stderr)
        self.assertIn("int answer = 42;", res.stdout)

    def test_directive_emits_a_checked_declaration(self):
        script = self.script("""
            local sdk = require('lcephyr')
            sdk.directive('config_value', function(ctx)
                assert(ctx.text == 'CONFIG_VALUE')
                assert(ctx.directive_name == 'config_value' and ctx.line == 1)
                assert(ctx.target_triple == 'x86_64-linux-gnu')
                sdk.define_integer(ctx.text, tonumber(sdk.env.get('BUILD_VALUE')))
            end)
        """)
        for pragma in ("#pragma cephyr config_value CONFIG_VALUE",
                       "#pragma GCC config_value CONFIG_VALUE",
                       '_Pragma("GCC config_value CONFIG_VALUE")'):
            with self.subTest(pragma=pragma):
                source = pragma + '\n_Static_assert(CONFIG_VALUE == 73, "extension did not run");\n'
                res = self.run_cephyr("-fsyntax-only", "--lua-extension", script,
                                      "--lua-env", "BUILD_VALUE=73", source=source)
                self.assertEqual(res.returncode, 0, res.stderr)
        res = self.run_cephyr("-fsyntax-only", "--lua-extension", script,
                              "--lua-env", "BUILD_VALUE=-1",
                              source="#pragma GCC config_value CONFIG_VALUE\n")
        self.assertNotEqual(res.returncode, 0)
        self.assertIn("cephyr.lua", res.stderr)

    def test_script_errors_and_sandbox_limits(self):
        for program, diagnostic in (
            (b"\x1bLua", "binary"),
            ("require('os')", "lcephyr"),
            ("require('lcephyr').env.get = function() return 'override' end", "read-only"),
            ("require('lcephyr').on('assembly', 'backend', function() end)", "rewrite"),
            ("while true do end", "instruction"),
            ("local s = string.rep('x', 128 * 1024 * 1024)", "memory"),
            ("this is not Lua", "cephyr.lua"),
        ):
            with self.subTest(program=program):
                res = self.run_cephyr("-E", "--lua-extension", self.script(program))
                self.assertNotEqual(res.returncode, 0)
                self.assertIn(diagnostic, res.stderr)
                self.assertEqual(res.stdout, "")
        res = self.run_cephyr("-E", "--lua-extension", self.script(" " * (1024 * 1024 + 1)))
        self.assertNotEqual(res.returncode, 0)
        self.assertIn("source size limit", res.stderr)

    def test_hook_callback_budget_and_isolation(self):
        observer = self.script("""
            local sdk = require('lcephyr')
            private_to_this_script = true
            sdk.on('analyze', 'see_mir', function(ctx)
                assert(ctx.text:sub(1, 13) == 'archon-mir 1\\n')
                assert(ctx.mir_schema_version == 1)
                sdk.report('warning', 'lua.test', 'observed MIR')
            end)
        """)
        isolated = self.script("assert(private_to_this_script == nil)", "isolated.lua")
        res = self.run_cephyr("-fsyntax-only", "--lua-extension", observer,
                              "--lua-extension", isolated)
        self.assertEqual(res.returncode, 0, res.stderr)
        self.assertIn("observed MIR", res.stderr)
        self.assertIn("lua.test", res.stderr)
        res = self.run_cephyr("-Werror", "-fsyntax-only", "--lua-extension", observer)
        self.assertNotEqual(res.returncode, 0)
        self.assertIn("lua.test", res.stderr)

        script = self.script("""
            require('lcephyr').on('desugar', 'run_forever', function() while true do end end)
        """)
        res = self.run_cephyr("-E", "--lua-extension", script)
        self.assertNotEqual(res.returncode, 0)
        self.assertIn("instruction", res.stderr)

    def test_checked_files_and_options(self):
        fifo = self.root / "fifo.lua"
        os.mkfifo(fifo)
        res = self.run_cephyr("-E", "--lua-extension", fifo)
        self.assertNotEqual(res.returncode, 0)
        self.assertIn("regular file", res.stderr)
        res = self.run_cephyr("-E", "--lua-env", "BUILD_MODE=checked")
        self.assertNotEqual(res.returncode, 0)
        self.assertIn("requires --lua-extension", res.stderr)
        res = self.run_cephyr("-E", "--lua-extension", self.script(""), "--lua-env", "BAD=1",
                              "--lua-env", "BAD=2")
        self.assertNotEqual(res.returncode, 0)
        self.assertIn("duplicate Lua environment entry", res.stderr)


if __name__ == "__main__":
    unittest.main()
