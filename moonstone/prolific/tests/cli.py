"""End-to-end LMDB, CLI and JSON regression, using only the Python standard library."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile

cli, host, directory = sys.argv[1:]


def run(*args, code=0):
    result = subprocess.run(args, capture_output=True, text=True, timeout=15)
    assert result.returncode == code, (args, result.returncode, result.stdout, result.stderr)
    return result.stdout


with tempfile.TemporaryDirectory(prefix="prolific-cli-", dir=directory) as owned:
    owned = Path(owned)
    cache = owned / "cache"
    original = json.loads(run(host, str(cache)))
    assert original["schema"] == "moonstone.prolific/1"
    assert original["complete"] and original["analysis"]["status"] == "completed"
    assert original["findings"][0]["evidence"] == "heuristic"
    assert all(t["exclusive_ns"] <= t["inclusive_ns"] for t in original["timings"])
    assert any(s["name"] == "folded.nodes" and s["sum"] == 8 for s in original["samples"])
    assert json.loads(run(cli, "report", str(cache), "example")) == original
    assert run(cli, "list", str(cache)).splitlines() == ["example"]
    script = owned / "profile.lua"
    script.write_text('''
        local vm = require('lprofvm')
        local count = 0
        vm.on('error', function(e)
            count = count + 1
            vm.finding('error', 'ESCAPE', 'a "quote"\\nline\\t\\001é: ' .. e.detail)
        end)
        vm.finish(function(s)
            assert(s.complete and count == 1 and tonumber(s.event_count) > 0)
            vm.finding('note', 'TOTAL', tostring(count))
        end)
    ''', encoding="utf-8")
    analyzed = json.loads(run(cli, "analyze", str(cache), "example", str(script), "specialized"))
    assert analyzed["events"] == original["events"]
    assert analyzed["findings"][0]["message"].startswith('a "quote"\nline\t\x01é: ')
    assert analyzed["findings"][1]["event_sequence"] == 0
    assert json.loads(run(cli, "report", str(cache), "specialized")) == analyzed
    run(cli, "analyze", str(cache), "example", str(script), "specialized", code=1)
    script.write_text("require('lprofvm').on('error', function() error('failure') end)")
    failed = json.loads(run(cli, "analyze", str(cache), "example", str(script), code=2))
    assert failed["analysis"]["status"] == "failed" and not failed["findings"]
    assert failed["events"] == original["events"]
    run(cli, "report", str(cache), "missing", code=1)
    run(cli, "list", str(owned / "missing"), code=1)
    assert not (owned / "missing").exists()
