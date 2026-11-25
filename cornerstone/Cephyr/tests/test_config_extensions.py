"""Cephyr.toml discovery, extension ordering, validation and permission tests."""

import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


CEPHYR = Path(sys.argv.pop(1)).resolve()
CC = sys.argv.pop(1)
SOURCE_DIR = Path(__file__).resolve().parents[1]


class ConfigExtensions(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(tmp.cleanup)
        cls.plugins = Path(tmp.name)
        for name, defines in (
            ("z-first.so", ["FIRST_PLUGIN"]),
            ("a-second.so", []),
            ("no-entry.so", ["NO_ENTRY"]),
            ("wrong-abi.so", ["PLUGIN_ABI=0"]),
            ("too-many-hooks.so", ["HOOK_COUNT=65"]),
        ):
            subprocess.run(
                [
                    CC, "-std=gnu11", "-shared", "-fPIC", "-I", SOURCE_DIR / "include",
                    *("-D" + define for define in defines), "-o", cls.plugins / name,
                    SOURCE_DIR / "tests/fixtures/sdk/config_plugin.c", "-ldl",
                ],
                check=True, timeout=15,
            )

    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name)
        self.project = self.root / "project"
        self.project.mkdir()
        self.marker = self.root / "loaded"
        self.env = dict(os.environ, CEPHYR_CONFIG_TEST_MARKER=str(self.marker),
                        PRIVATE_TEST_ENV="not granted")
        self.env.pop("CEPHYR_CONFIG", None)

    def write_config(self, settings):
        path = self.project / "Cephyr.toml"
        path.write_text("[extensions]\n" + settings, encoding="utf-8")
        return path

    def run_cephyr(self, *args, cwd=None):
        return subprocess.run(
            [CEPHYR, *args, "-E", "-P", "-x", "c", "-"],
            cwd=cwd or self.root, env=self.env, input="int original;\n",
            capture_output=True, text=True, timeout=8,
        )

    def use_plugin(self, name):
        shutil.copyfile(self.plugins / name, self.project / name)

    def test_config_discovery_and_precedence(self):
        unconfigured = self.root / "unconfigured"
        unconfigured.mkdir()
        cli_config = self.root / "explicit.toml"
        for path, sentinel in (
            (self.project / "Cephyr.toml", "from_cwd"),
            (self.root / "Cephyr.toml", "from_pwd"),
            (self.root / "environment.toml", "from_env"),
            (cli_config, "from_cli"),
        ):
            path.write_text(
                f'[preprocess]\ndefines = ["original={sentinel}"]\n',
                encoding="utf-8",
            )
        # PWD disagrees with the child's working directory and contains its own
        # Cephyr.toml. Neither PWD nor an ancestor is a discovery location.
        self.env["PWD"] = str(self.root)

        def check(expected, *args, cwd=self.project):
            result = self.run_cephyr(*args, cwd=cwd)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertRegex(result.stdout, rf"^\s*int\s+{expected}\s*;\s*$")

        check("from_cwd")
        self.env["CEPHYR_CONFIG"] = "../environment.toml"
        check("from_env")
        check("from_cli", "--config", cli_config)
        self.env.pop("CEPHYR_CONFIG")
        check("from_cli", f"--config={cli_config}", cwd=unconfigured)
        check("original", cwd=unconfigured)

    def test_config_relative_lua_order_and_separate_env(self):
        cfg = self.write_config(
            'lua = ["z-first.lua", "a-second.lua"]\n'
            'lua_env = { BOTH = "configured", EMPTY = "" }\n'
        )
        (self.project / "z-first.lua").write_text("""
            local sdk = require('lcephyr')
            assert(sdk.env.get('BOTH') == 'configured' and sdk.env.get('EMPTY') == '')
            assert(sdk.env.get('CLI_ONLY') == nil and sdk.env.get('PRIVATE_TEST_ENV') == nil)
            assert(os == nil and io == nil and package == nil)
            sdk.on('desugar', 'first', function(ctx)
                assert(ctx.text == 'int original;\\n')
                sdk.report('note', 'test.order', 'configured first')
            end)
        """, encoding="utf-8")
        (self.project / "a-second.lua").write_text("""
            local sdk = require('lcephyr')
            sdk.on('desugar', 'second', function(ctx)
                assert(ctx.text == 'int original;\\n')
                sdk.report('note', 'test.order', 'configured second')
            end)
        """, encoding="utf-8")
        (self.root / "cli.lua").write_text("""
            local sdk = require('lcephyr')
            assert(sdk.env.get('BOTH') == 'cli' and sdk.env.get('EMPTY') == nil)
            assert(sdk.env.get('CLI_ONLY') == 'granted')
            sdk.on('desugar', 'cli', function(ctx)
                assert(ctx.text == 'int original;\\n')
                sdk.report('note', 'test.order', 'command line third')
            end)
        """, encoding="utf-8")
        result = self.run_cephyr(
            "--config", cfg, "--lua-extension", "cli.lua",
            "--lua-env=BOTH=cli", "--lua-env=CLI_ONLY=granted",
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertLess(result.stderr.index("configured first"), result.stderr.index("configured second"))
        self.assertLess(result.stderr.index("configured second"), result.stderr.index("command line third"))
        self.env["CEPHYR_CONFIG"] = str(cfg.relative_to(self.root))
        result = self.run_cephyr()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("configured second", result.stderr)
        self.assertNotIn("command line third", result.stderr)

    def test_bad_config(self):
        for settings, message in (
            ('lua = "script.lua"', "array of strings"),
            ("native = [12]", "only strings"),
            ('lua = [""]', "must not be empty"),
            ('lua = ["-"]', "not standard input"),
            ('lua = ["script.lua"]\nlua_env = ["A=B"]', "table of strings"),
            ('lua = ["script.lua"]\nlua_env = { A = 1 }', "must be a string"),
            ('lua = ["script.lua"]\nlua_env = { "BAD-NAME" = "x" }', "entry name"),
            ('lua = ["script.lua"]\nlua_env = { VALUE = "\\u0000" }', "NUL"),
            ('lua_env = { VALUE = "x" }', "requires at least one path"),
            ("allow_native_plugins = true", "unknown configuration key"),
            ("native = " + json.dumps(["plugin.so"] * 9), "too many paths"),
            ("lua = " + json.dumps(["script.lua"] * 9), "too many paths"),
            ('lua = ["script.lua"]\nlua_env = { A = "' + "x" * 4097 + '" }', "size limit"),
        ):
            with self.subTest(message=message):
                result = self.run_cephyr("--config", self.write_config(settings + "\n"))
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(message, result.stderr)
                self.assertEqual(result.stdout, "")

    def test_combined_lua_limit(self):
        cfg = self.write_config("lua = " + json.dumps(["script.lua"] * 8) + "\n")
        result = self.run_cephyr("--config", cfg, "--lua-extension", "cli.lua")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("too many Lua extensions across configuration and command line", result.stderr)

    def test_native_permission_checks_before_dlopen(self):
        self.use_plugin("z-first.so")
        cfg = self.write_config('native = ["z-first.so"]\n')
        result = self.run_cephyr(cwd=self.project)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("--allow-native-plugins", result.stderr)
        self.assertFalse(self.marker.exists(), "plugin constructor ran without permission")
        result = self.run_cephyr("--config", cfg, "--allow-native-plugins")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("int first_plugin;", result.stdout)
        self.assertTrue(self.marker.is_file())

    def test_native_registration_order_and_local_visibility(self):
        self.use_plugin("z-first.so")
        self.use_plugin("a-second.so")
        cfg = self.write_config('native = ["z-first.so", "a-second.so"]\nlua = ["check.lua"]\n')
        (self.project / "check.lua").write_text("""
            local sdk = require('lcephyr')
            sdk.on('desugar', 'lua', function(ctx)
                assert(ctx.text == 'int second_plugin;\\n#pragma GCC config_native_value\\n')
                sdk.report('note', 'test.order', 'native then Lua')
            end)
        """, encoding="utf-8")
        result = self.run_cephyr("--config", cfg, "--allow-native-plugins")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("int second_plugin;", result.stdout)
        self.assertIn("enum { native_value = 7 };", result.stdout)
        self.assertIn("native then Lua", result.stderr)

    def test_native_loader_failures_and_conflicts(self):
        for name, message in (
            ("no-entry.so", "must export cephyr_plugin_v1"),
            ("wrong-abi.so", "incompatible SDK descriptor"),
            ("too-many-hooks.so", "registration count"),
        ):
            with self.subTest(name=name):
                self.use_plugin(name)
                cfg = self.write_config(f'native = ["{name}"]\n')
                result = self.run_cephyr("--config", cfg, "--allow-native-plugins")
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(message, result.stderr)
                self.assertIn(name, result.stderr)
        self.use_plugin("z-first.so")
        cfg = self.write_config('native = ["z-first.so", "z-first.so"]\n')
        result = self.run_cephyr("--config", cfg, "--allow-native-plugins")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("hook name must be unique", result.stderr)

    def test_native_does_not_search_library_paths(self):
        cfg = self.write_config('native = ["z-first.so"]\n')
        self.env["LD_LIBRARY_PATH"] = str(self.plugins)
        result = self.run_cephyr("--config", cfg, "--allow-native-plugins")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("cannot read native plugin", result.stderr)
        self.assertFalse(self.marker.exists(), "plugin was found on LD_LIBRARY_PATH")


if __name__ == "__main__":
    unittest.main()
