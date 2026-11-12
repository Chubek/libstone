"""Generate cached profiles, emit Lua IR through the CLI, execute a user translator."""
from pathlib import Path
import subprocess
import sys
import tempfile

generator, cli, lua, modules, test, root = sys.argv[1:]

def run(*args):
    result = subprocess.run(args, capture_output=True, text=True, timeout=20)
    assert result.returncode == 0, (args, result.stdout, result.stderr)
    return result.stdout

with tempfile.TemporaryDirectory(prefix="specir-", dir=root) as owned:
    directory = Path(owned)
    cache = directory / "cache"
    run(generator, str(cache))
    script = directory / "profile.spec.lua"
    text = run(cli, "speculate", str(cache), "profiles")
    assert text == run(cli, "speculate", str(cache), "profiles")
    script.write_text(text, encoding="utf-8")
    incomplete = directory / "incomplete.spec.lua"
    incomplete.write_text(run(cli, "speculate", str(cache), "incomplete"), encoding="utf-8")
    assert "SpecIR translation verified" in run(lua, test, modules, str(script), str(incomplete))
