"""Exercise compiler rejection and compiler-measured ABI data, not text matching."""
import json
import pathlib
import subprocess
import sys

compiler, compiler_id, generated, source, probe = sys.argv[1:]
generated = pathlib.Path(generated)
manifest = json.loads((generated / "manifest.json").read_text())
actual = json.loads(subprocess.check_output([probe], text=True))
assert actual["pointer_size"] > 0
assert len(manifest["layouts"]) == len(actual["records"])
for expected, measured in zip(manifest["layouts"], actual["records"]):
    assert expected["type"] == measured["type"]
    assert expected["size"] == measured["size"]
    assert expected["alignment"] == measured["alignment"]
    assert {field["name"]: field["offset"] for field in expected["fields"]} == measured["fields"]
for name in ("bad_signature", "bad_layout"):
    if compiler_id == "MSVC":
        command = [compiler, "/nologo", "/std:c++20", "/c", f"/I{source}", str(generated / f"{name}.cpp"), f"/Fo{generated / (name + '.obj')}"]
    else:
        command = [compiler, "-std=c++20", "-c", "-I", source, str(generated / f"{name}.cpp"), "-o", str(generated / f"{name}.o")]
    result = subprocess.run(command, capture_output=True, text=True, timeout=30)
    assert result.returncode != 0, f"compiler accepted {name}"
    assert "StaticFFI" in result.stdout + result.stderr, result.stdout + result.stderr
