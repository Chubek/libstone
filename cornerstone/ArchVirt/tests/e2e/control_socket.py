import json
import pathlib
import socket
import subprocess
import sys
import tempfile

with tempfile.TemporaryDirectory() as directory:
    path = str(pathlib.Path(directory) / "control.sock")
    process = subprocess.Popen([sys.argv[1], "--machine", "aarch64", "--headless", "--control", path],
                               stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    try:
        while True:
            line = process.stdout.readline()
            if "resident shell ready" in line:
                break
            assert line, process.stderr.read()
        def request(command):
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
                client.settimeout(5)
                client.connect(path)
                client.sendall(json.dumps({"execute": command}).encode())
                return json.loads(client.recv(4096))
        status = request("query-status")
        assert status["return"]["machine"] == "aarch64" and status["return"]["running"], status
        assert "error" in request("not-stop"), "Commands must match exactly"
        assert "return" in request("stop")
        out, error = process.communicate(timeout=5)
        assert process.returncode == 0, out + error
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()
print("Control socket reports machine state and stops the runtime")
