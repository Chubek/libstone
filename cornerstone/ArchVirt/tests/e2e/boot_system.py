"""Boot the actual bundled kernel; verify commands against guest RAM/files."""
import os
import pathlib
import subprocess
import sys
import tempfile

binary, machine, backend = sys.argv[1:]
if backend == "kvm" and not os.access("/dev/kvm", os.R_OK | os.W_OK):
    sys.exit(77)
commands = (
    "uname\ncmdline\ncat /etc/hostname\npwd\nmkdir /work\ncd /work\n"
    "echo 'guest data' > note\ncat note\ncp note copy\nmv copy moved\n"
    "cat moved\nwc moved\nls\nrm note\ncat note\ncd ..\npwd\n"
    "\x12echo console-two\n\x11pwd\nexit\n"
)
args = [binary, "--machine", machine, "--backend", backend, "--headless", "--cmdline", "boot-test=1"]
if backend != "kvm":
    args += ["--max-instructions", "5000000"]
env = os.environ.copy()
for name in list(env):
    if name.startswith(("DOMVIRT_", "ARCHVIRT_")):
        del env[name]
with tempfile.TemporaryDirectory() as directory:
    result = subprocess.run(args, input=commands, capture_output=True, text=True,
                            cwd=directory, env=env, timeout=30)
assert result.returncode == 0, result.stdout + result.stderr
for expected in (f"Domix {machine}\n", "Root filesystem mounted", "domvirt-domix\n", "boot-test=1\n",
                 "guest data\n", "11\n", "moved\n", "file not found\n",
                 "console-two\n", "Domix shutdown\n"):
    assert expected in result.stdout, (expected, result.stdout)
assert result.stdout.count("guest data\n") == 2, result.stdout
assert "domix# pwd\n/\n" in result.stdout, result.stdout

# Exercise normal termination of a VM waiting for console input.
process = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                           stderr=subprocess.PIPE, text=True, env=env)
try:
    while True:
        line = process.stdout.readline()
        if "resident shell ready" in line:
            break
        assert line, process.stderr.read()
    process.terminate()
    out, error = process.communicate(timeout=5)
    assert process.returncode == 0, out + error
finally:
    if process.poll() is None:
        process.kill()
        process.wait()
print(f"{machine}/{backend}: boot, filesystem, consoles, shutdown and host stop passed")
