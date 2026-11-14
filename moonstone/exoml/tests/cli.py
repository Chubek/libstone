import json
import subprocess
import sys

cli, fixture = sys.argv[1:]


def run(*args, text=None):
    return subprocess.run([cli, *args], input=text, text=True, capture_output=True, timeout=10)


result = run("--emit", "Manifest", fixture)
assert result.returncode == 0, result.stderr
assert json.loads(result.stdout)["name"] == "Script"
assert run("--help").returncode == 0
assert run("--check", fixture).stdout == ""
assert run("--check", "-", text="X ~get! Unknown();").returncode == 0
assert run("-", text="X ~get! Unknown();").returncode != 0
assert run("--emit", "Missing", fixture).returncode != 0
assert run("--unknown").returncode != 0
assert run("--emit").returncode != 0
assert run("--check", "-", text="X ~get! Types:Name(1,);").returncode != 0
assert run("--check", "-", text=" " * (1024 * 1024 + 1)).returncode != 0
result = run("--emit", "X", "-", text='X ~get! Types:Join(["a", "b"], ":");')
assert result.returncode == 0 and result.stdout == "a:b", result.stderr
result = run("--repl", text='X ~get! Types:Name(1);\n:symbols\n:quit\n')
assert result.returncode == 0 and "X = integer" in result.stdout, result.stderr
