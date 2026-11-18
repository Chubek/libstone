#!/usr/bin/env python3
"""Exercise PCC wire v1 over an actual pinned, forced-command SSH connection."""

import argparse
import json
import os
from pathlib import Path
import pwd
import shlex
import socket
import subprocess
import tempfile
import time


OK = b"PCC-VERIFY/1 OK\n"
REJECT = b"PCC-VERIFY/1 REJECT\n"


def run(args, data=None, *, check=True):
    return subprocess.run(args, input=data, capture_output=True, timeout=30, check=check)


def require(condition, message, result=None):
    if not condition:
        raise RuntimeError(f"{message}: {result!r}")


def generate_signer(openssl, directory, name):
    private = directory / f"{name}.pem"
    public = directory / f"{name}.pub.pem"
    run([openssl, "genpkey", "-algorithm", "ED25519", "-out", private])
    run([openssl, "pkey", "-in", private, "-pubout", "-out", public])
    return private, public


def ssh_key(keygen, directory, name):
    key = directory / name
    run([keygen, "-q", "-t", "ed25519", "-N", "", "-f", key])
    return key, key.with_suffix(".pub").read_text().strip()


def verify_over_ssh(args, directory, package, untrusted, trust_key):
    host_key, ssh_host_public = ssh_key(args.ssh_keygen, directory, "host")
    client_key, ssh_client_public = ssh_key(args.ssh_keygen, directory, "client")
    wrong_key, ssh_wrong_public = ssh_key(args.ssh_keygen, directory, "wrong")
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
        listener.bind(("127.0.0.1", 0))
        port = listener.getsockname()[1]

    # The file is independent of the network connection (not ssh-keyscan).
    hosts = directory / "known_hosts"
    wrong_hosts = directory / "known_hosts_wrong"
    hosts.write_text(f"[127.0.0.1]:{port} {ssh_host_public}\n")
    wrong_hosts.write_text(f"[127.0.0.1]:{port} {ssh_wrong_public}\n")
    authorized_keys = directory / "authorized_keys"
    authorized_keys.write_text(f"restrict {ssh_client_public}\n")
    config = directory / "sshd_config"
    config.write_text(
        f"ListenAddress 127.0.0.1\n"
        f"Port {port}\n"
        f"HostKey {json.dumps(str(host_key))}\n"
        f"PidFile {json.dumps(str(directory / 'sshd.pid'))}\n"
        f"AuthorizedKeysFile {json.dumps(str(authorized_keys))}\n"
        "UsePAM no\n"
        "PasswordAuthentication no\n"
        "KbdInteractiveAuthentication no\n"
        "AuthenticationMethods publickey\n"
        "PermitRootLogin prohibit-password\n"
        "PermitUserEnvironment no\n"
        "PermitUserRC no\n"
        "PermitTTY no\n"
        "DisableForwarding yes\n"
        f"ForceCommand {shlex.join([args.cli, 'pcc-serve', '--trust-key', str(trust_key)])}\n"
    )

    def request(data, *, identity=client_key, known=hosts):
        return run([
            args.ssh,
            "-F", "/dev/null",
            "-T", "-p", str(port),
            "-i", identity,
            "-o", "BatchMode=yes",
            "-o", "IdentitiesOnly=yes",
            "-o", "IdentityAgent=none",
            "-o", "StrictHostKeyChecking=yes",
            "-o", f'UserKnownHostsFile="{known}"',
            "-o", "GlobalKnownHostsFile=/dev/null",
            "-o", "UpdateHostKeys=no",
            "-o", "ConnectTimeout=5",
            f"{pwd.getpwuid(os.getuid()).pw_name}@127.0.0.1",
            # The server must run only the configured verifier.
            "printf NOT_THE_VERIFIER",
        ], data, check=False)

    log_path = directory / "sshd.log"
    with log_path.open("wb") as log:
        daemon = subprocess.Popen([args.sshd, "-D", "-e", "-f", config], stderr=log)
        try:
            deadline = time.monotonic() + 5
            while daemon.poll() is None:
                try:
                    with socket.create_connection(("127.0.0.1", port), timeout=1):
                        break
                except ConnectionRefusedError:
                    require(time.monotonic() < deadline, "sshd never started", log_path.read_text())
                    time.sleep(0.025)
            require(daemon.poll() is None, "could not start sshd", log_path.read_text())

            response = request(package)
            expected = (1, REJECT) if args.expect_no_solver else (0, OK)
            require((response.returncode, response.stdout) == expected,
                    "valid package over SSH", response)
            for name, bad in (
                ("tampered", package[:-1] + bytes([package[-1] ^ 1])),
                ("untrusted signer", untrusted),
                ("short", package[:-1]),
                ("oversized length", package[:16] + b"\xff" * 4),
                ("concatenated", package + package),
            ):
                response = request(bad)
                require((response.returncode, response.stdout) == (1, REJECT),
                        name + " package over SSH", response)
            for response in (request(package, identity=wrong_key), request(package, known=wrong_hosts)):
                require(response.returncode == 255 and not response.stdout,
                        "SSH must reject an untrusted transport peer", response)
        finally:
            if daemon.poll() is None:
                daemon.terminate()
                try:
                    daemon.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    daemon.kill()
                    daemon.wait()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("cli")
    parser.add_argument("source")
    parser.add_argument("--ssh", default="ssh")
    parser.add_argument("--sshd", default="/usr/sbin/sshd")
    parser.add_argument("--ssh-keygen", default="ssh-keygen")
    parser.add_argument("--openssl", default="openssl")
    parser.add_argument("--expect-no-solver", action="store_true")
    args = parser.parse_args()
    args.cli = str(Path(args.cli).resolve())
    args.sshd = str(Path(args.sshd).resolve())

    with tempfile.TemporaryDirectory(prefix="pcc-package-ssh-") as temp:
        directory = Path(temp).resolve()
        private, public = generate_signer(args.openssl, directory, "trusted")
        wrong, _ = generate_signer(args.openssl, directory, "untrusted")

        def pack(key):
            return run([args.cli, "pcc-pack", args.source, "--sign-key", key]).stdout

        package = pack(private)
        require(package.startswith(b"NSPCCPKG") and pack(private) == package,
                "nondeterministic or invalid wire package")
        # A second consumer of the signature encoding verifies the exact signed
        # bytes with the separately exported OpenSSL SPKI key.
        signed = directory / "message"
        seal = directory / "signature"
        signed.write_bytes(package[:-64])
        seal.write_bytes(package[-64:])
        run([args.openssl, "pkeyutl", "-verify", "-pubin", "-inkey", public,
             "-rawin", "-in", signed, "-sigfile", seal])

        path = directory / "signed.pcc"
        path.write_bytes(package)
        local = run([args.cli, "pcc-verify", path, "--trust-key", public], check=False)
        if args.expect_no_solver:
            require(local.returncode != 0 and local.stdout != OK,
                    "unexpected local success without solver", local)
        else:
            require((local.returncode, local.stdout) == (0, OK),
                    "local verification failed", local)
        verify_over_ssh(args, directory, package, pack(wrong), public)


if __name__ == "__main__":
    main()
