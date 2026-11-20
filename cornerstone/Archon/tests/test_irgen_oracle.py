"""Generated Oracle transport, annotation, and hardening contract tests.

The Oracle implementation is deliberately exercised as generated strict-C99
code.  Runtime tests use only local AF_UNIX sockets and POSIX shared memory
under /tmp/opencode; they do not import implementation-private Python helpers.
"""

from __future__ import annotations

import os
from pathlib import Path
import shutil
import socket
import stat
import struct
import subprocess
import tempfile
import threading
import time
import unittest

from archon.compiler import compile_file
from archon.irgen import generate_ir, pipeline_stages

try:
    from archon.irgen import oracle
except ImportError as error:  # Keep discovery useful while the peer stage lands.
    oracle = None
    _ORACLE_IMPORT_ERROR = error
else:
    _ORACLE_IMPORT_ERROR = None


ROOT = Path(__file__).resolve().parents[1]
RV32 = ROOT / "stdspec" / "riscv" / "rv32i.arch"
TMP_ROOT = Path("/tmp/opencode")


_CLIENT = r"""
#include "Oracle.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static ArchonOracleStage parse_stage(const char *name) {
    if (strcmp(name, "rewrite") == 0)
        return ARCHON_ORACLE_STAGE_REWRITE;
    if (strcmp(name, "schedule") == 0)
        return ARCHON_ORACLE_STAGE_SCHEDULE;
    if (strcmp(name, "isel") == 0)
        return ARCHON_ORACLE_STAGE_ISEL;
    if (strcmp(name, "isched") == 0)
        return ARCHON_ORACLE_STAGE_ISCHED;
    if (strcmp(name, "regalloc") == 0)
        return ARCHON_ORACLE_STAGE_REGALLOC;
    if (strcmp(name, "emit") == 0)
        return ARCHON_ORACLE_STAGE_EMIT;
    return ARCHON_ORACLE_STAGE_INVALID;
}

int main(int argc, char **argv) {
    ArchonOracleDiagnostic diagnostic;
    ArchonOracleStatus status;
    ArchonOracleStage stage;

    ArchonOracleOptions options;

    if (argc != 4 && argc != 5)
        return 2;
    stage = parse_stage(argv[2]);
    archon_oracle_options_init(&options);
    if (strcmp(argv[3], "-") != 0)
        options.socket_path = argv[3];
    if (argc == 5)
        options.max_artifact_bytes = (size_t)strtoull(argv[4], NULL, 10);
    archon_oracle_diagnostic_init(&diagnostic);
    status = archon_oracle_process_file(
        argv[1], stage, &options, &diagnostic);
    printf("%s\n", archon_oracle_status_string(status));
    if (diagnostic.message[0] != '\0')
        printf("%s\n", diagnostic.message);
    return status == ARCHON_ORACLE_STATUS_OK ||
        status == ARCHON_ORACLE_STATUS_INACTIVE ? 0 : 1;
}
"""


_WORKER = r"""
#include "Oracle.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int parse_stage(
        const char *name, ArchonOracleStage *stage) {
    if (strcmp(name, "rewrite") == 0)
        *stage = ARCHON_ORACLE_STAGE_REWRITE;
    else if (strcmp(name, "schedule") == 0)
        *stage = ARCHON_ORACLE_STAGE_SCHEDULE;
    else if (strcmp(name, "isel") == 0)
        *stage = ARCHON_ORACLE_STAGE_ISEL;
    else if (strcmp(name, "isched") == 0)
        *stage = ARCHON_ORACLE_STAGE_ISCHED;
    else if (strcmp(name, "regalloc") == 0)
        *stage = ARCHON_ORACLE_STAGE_REGALLOC;
    else if (strcmp(name, "emit") == 0)
        *stage = ARCHON_ORACLE_STAGE_EMIT;
    else
        return 0;
    return 1;
}

static ArchonOracleStatus transform(
        const ArchonOracleWorkerRequest *request,
        ArchonOracleWorkerResponse *response,
        void *context,
        ArchonOracleDiagnostic *diagnostic) {
    static const unsigned char prefix[] = "/* oracle:begin */\n";
    static const unsigned char suffix[] = "/* oracle:end */\n";
    size_t size;
    ArchonOracleStatus status;

    (void)diagnostic;
    if (request == NULL || response == NULL)
        return ARCHON_ORACLE_STATUS_INVALID_ARGUMENT;
    if (context == NULL ||
            request->stage != *(const ArchonOracleStage *)context)
        return ARCHON_ORACLE_STATUS_INVALID_ARGUMENT;
    if (request->artifact_path == NULL ||
            request->artifact_data == NULL ||
            request->artifact_size > (size_t)-1 - sizeof prefix + 1u ||
            request->artifact_size + sizeof prefix - 1u >
                (size_t)-1 - sizeof suffix + 1u)
        return ARCHON_ORACLE_STATUS_INVALID_ARGUMENT;
    size = sizeof prefix - 1u + request->artifact_size + sizeof suffix - 1u;
    if (size > request->artifact_capacity)
        return ARCHON_ORACLE_STATUS_LIMIT_EXCEEDED;
    memmove(
        request->artifact_data + sizeof prefix - 1u,
        request->artifact_data,
        request->artifact_size);
    memcpy(request->artifact_data, prefix, sizeof prefix - 1u);
    memcpy(
        request->artifact_data + sizeof prefix - 1u + request->artifact_size,
        suffix,
        sizeof suffix - 1u);
    response->artifact_size = size;
    response->artifact_changed = 1;
    status = archon_oracle_response_add_u64(
        response, "cost", UINT64_C(42));
    if (status != ARCHON_ORACLE_STATUS_OK)
        return status;
    status = archon_oracle_response_add_i64(
        response, "delta", -INT64_C(7));
    if (status != ARCHON_ORACLE_STATUS_OK)
        return status;
    status = archon_oracle_response_add_bool(
        response, "accepted", 1);
    if (status != ARCHON_ORACLE_STATUS_OK)
        return status;
    return archon_oracle_response_add_string(
        response, "phase", "peephole");
}

int main(int argc, char **argv) {
    ArchonOracleServer server;
    ArchonOracleDiagnostic diagnostic;
    ArchonOracleStatus status;
    ArchonOracleStage expected_stage;

    if (argc != 3)
        return 2;
    if (!parse_stage(argv[2], &expected_stage))
        return 2;
    archon_oracle_server_init(&server);
    archon_oracle_diagnostic_init(&diagnostic);
    status = archon_oracle_server_open(
        &server, argv[1], 1u, 0u, &diagnostic);
    if (status != ARCHON_ORACLE_STATUS_OK) {
        fprintf(
            stderr, "%s: %s\n",
            archon_oracle_status_string(status), diagnostic.message);
        archon_oracle_server_dispose(&server);
        return 1;
    }
    status = archon_oracle_server_serve_once(
        &server, transform, &expected_stage, &diagnostic);
    if (status != ARCHON_ORACLE_STATUS_OK) {
        fprintf(
            stderr, "%s: %s\n",
            archon_oracle_status_string(status), diagnostic.message);
        archon_oracle_server_dispose(&server);
        return 1;
    }
    archon_oracle_server_dispose(&server);
    return 0;
}
"""


_ANNOTATION_QUERY = r"""
#include "Oracle.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    ArchonOracleAnnotations annotations;
    ArchonOracleDiagnostic diagnostic;
    ArchonOracleStatus status;
    uint64_t u64 = 0u;
    int64_t i64 = 0;
    int boolean = 0;
    const char *string = NULL;
    ArchonOracleStage expected_stage;

    if (argc != 4)
        return 2;
    if (strcmp(argv[3], "rewrite") == 0)
        expected_stage = ARCHON_ORACLE_STAGE_REWRITE;
    else if (strcmp(argv[3], "schedule") == 0)
        expected_stage = ARCHON_ORACLE_STAGE_SCHEDULE;
    else if (strcmp(argv[3], "isel") == 0)
        expected_stage = ARCHON_ORACLE_STAGE_ISEL;
    else if (strcmp(argv[3], "isched") == 0)
        expected_stage = ARCHON_ORACLE_STAGE_ISCHED;
    else if (strcmp(argv[3], "regalloc") == 0)
        expected_stage = ARCHON_ORACLE_STAGE_REGALLOC;
    else if (strcmp(argv[3], "emit") == 0)
        expected_stage = ARCHON_ORACLE_STAGE_EMIT;
    else
        return 2;
    archon_oracle_annotations_init(&annotations);
    archon_oracle_diagnostic_init(&diagnostic);
    status = archon_oracle_annotations_load(
        argv[1], &annotations, &diagnostic);
    if (strcmp(argv[2], "reject") == 0) {
        archon_oracle_annotations_dispose(&annotations);
        return status == ARCHON_ORACLE_STATUS_OK ? 1 : 0;
    }
    if (status != ARCHON_ORACLE_STATUS_OK) {
        fprintf(
            stderr, "%s: %s\n",
            archon_oracle_status_string(status), diagnostic.message);
        archon_oracle_annotations_dispose(&annotations);
        return 1;
    }
    if (annotations.count != 4u ||
            annotations.stage != expected_stage ||
            archon_oracle_annotations_get_u64(
                &annotations, "cost", &u64) != ARCHON_ORACLE_STATUS_OK ||
            archon_oracle_annotations_get_i64(
                &annotations, "delta", &i64) != ARCHON_ORACLE_STATUS_OK ||
            archon_oracle_annotations_get_bool(
                &annotations, "accepted", &boolean) !=
                ARCHON_ORACLE_STATUS_OK ||
            archon_oracle_annotations_get_string(
                &annotations, "phase", &string) !=
                ARCHON_ORACLE_STATUS_OK ||
            u64 != UINT64_C(42) ||
            i64 != -INT64_C(7) ||
            boolean != 1 ||
            string == NULL ||
            strcmp(string, "peephole") != 0) {
        archon_oracle_annotations_dispose(&annotations);
        return 1;
    }
    /* Typed lookup must not coerce a value of another kind. */
    if (archon_oracle_annotations_get_i64(
            &annotations, "cost", &i64) !=
            ARCHON_ORACLE_STATUS_TYPE_MISMATCH) {
        archon_oracle_annotations_dispose(&annotations);
        return 1;
    }
    archon_oracle_annotations_dispose(&annotations);
    return 0;
}
"""


class OracleEmissionTests(unittest.TestCase):
    def test_expected_oracle_generator_is_available(self):
        self.assertIsNotNone(
            oracle,
            "expected archon.irgen.oracle: "
            f"{_ORACLE_IMPORT_ERROR!r}",
        )

    @unittest.skipIf(oracle is None, "Oracle generator is not present yet")
    def test_deterministic_emission_and_false_omission(self):
        default = oracle.emit(None)
        explicit = oracle.emit({
            "ir": "gated",
            "architecture_names": ["ignored"],
            "architectures": [{"architecture": "ignored"}],
            "options": {"oracle": True},
        })
        reordered = oracle.emit({
            "options": {"oracle": True},
            "architectures": [],
            "architecture_names": [],
            "ir": "anf",
        })
        self.assertEqual(set(default), {"Oracle.c", "Oracle.h"})
        self.assertEqual(default, explicit)
        self.assertEqual(default, reordered)
        self.assertEqual(oracle.emit({"options": {"oracle": False}}), {})
        self.assertTrue(all(text.isascii() for text in default.values()))
        self.assertNotIn("getenv(", default["Oracle.c"])
        self.assertNotIn("ARCHON_ORACLE_SOCKET", default["Oracle.c"])
        self.assertIn("archon_oracle_process_file", default["Oracle.h"])
        self.assertIn("ArchonOracleStage", default["Oracle.h"])
        self.assertIn("ARCHON_ORACLE_STAGE_INVALID", default["Oracle.h"])
        self.assertIn("ARCHON_ORACLE_STAGE_REWRITE", default["Oracle.h"])
        self.assertIn("ARCHON_ORACLE_STAGE_SCHEDULE", default["Oracle.h"])
        self.assertIn("ARCHON_ORACLE_STAGE_ISEL", default["Oracle.h"])
        self.assertIn("ARCHON_ORACLE_STAGE_ISCHED", default["Oracle.h"])
        self.assertIn("ARCHON_ORACLE_STAGE_REGALLOC", default["Oracle.h"])
        self.assertIn("ARCHON_ORACLE_STAGE_EMIT", default["Oracle.h"])
        self.assertIn("ArchonOracleWorkerRequest", default["Oracle.h"])
        self.assertIn("unsigned char *artifact_data", default["Oracle.h"])
        self.assertIn("archon_oracle_server_init", default["Oracle.h"])
        self.assertIn("archon_oracle_server_open", default["Oracle.h"])
        self.assertIn("archon_oracle_server_serve_once", default["Oracle.h"])
        self.assertIn("archon_oracle_server_dispose", default["Oracle.h"])
        self.assertIn("ARCHON_ORACLE_STATUS_INACTIVE", default["Oracle.h"])
        self.assertIn(
            '#define ARCHON_ORACLE_WIRE_MAGIC "ARCHORC1"',
            default["Oracle.h"],
        )
        self.assertIn(
            "#define ARCHON_ORACLE_REQUEST_HEADER_BYTES 72u",
            default["Oracle.h"],
        )
        self.assertIn(
            "#define ARCHON_ORACLE_RESPONSE_HEADER_BYTES 56u",
            default["Oracle.h"],
        )
        self.assertIn("archon_oracle_annotations_load", default["Oracle.h"])
        self.assertIn("ARCHON_ORACLE_BEGIN", default["Oracle.h"])

        stage_names = {
            stage.__name__.rsplit(".", 1)[-1]
            for stage in pipeline_stages()
        }
        self.assertIn("oracle", stage_names)

    @unittest.skipIf(oracle is None, "Oracle generator is not present yet")
    def test_generation_option_controls_pipeline_outputs(self):
        database = compile_file(RV32)
        disabled = generate_ir(
            "gated", [database], options={"oracle": False}
        )
        self.assertNotIn("Oracle.c", disabled)
        self.assertNotIn("Oracle.h", disabled)
        enabled = generate_ir(
            "gated", [database], options={"oracle": True}
        )
        self.assertTrue({"Oracle.c", "Oracle.h"} <= enabled.keys())


@unittest.skipIf(oracle is None, "Oracle generator is not present yet")
class OracleGeneratedRuntimeTests(unittest.TestCase):
    @staticmethod
    def compilers() -> list[str]:
        return [
            compiler
            for compiler in ("gcc", "clang")
            if shutil.which(compiler)
        ]

    def setUp(self):
        if os.name != "posix" or not hasattr(socket, "AF_UNIX"):
            self.skipTest("Oracle runtime checks require POSIX AF_UNIX")
        if not self.compilers():
            self.skipTest("GCC and Clang are unavailable")
        if not Path("/dev/shm").is_dir():
            self.skipTest("POSIX shared-memory filesystem /dev/shm is unavailable")
        TMP_ROOT.mkdir(parents=True, exist_ok=True)

    @staticmethod
    def sidecar(artifact: Path) -> Path:
        return artifact.with_name(artifact.name[:-2] + ".oracle.i")

    @staticmethod
    def clean_environment(**updates: str) -> dict[str, str]:
        environment = {
            key: value
            for key, value in os.environ.items()
            if not key.startswith("ARCHON_ORACLE_")
        }
        environment.update(updates)
        return environment

    @staticmethod
    def shm_snapshot() -> set[str]:
        try:
            return {
                path.name
                for path in Path("/dev/shm").iterdir()
                if "archon" in path.name.lower()
                and "oracle" in path.name.lower()
            }
        except OSError:
            return set()

    def write_generated(self, root: Path) -> None:
        for name, text in oracle.emit({"options": {"oracle": True}}).items():
            (root / name).write_text(text, encoding="ascii", newline="\n")

    def compile_program(
        self,
        root: Path,
        compiler: str,
        name: str,
        source: str,
    ) -> Path:
        source_path = root / f"{name}.c"
        executable = root / name
        source_path.write_text(source, encoding="ascii", newline="\n")
        command = [
            compiler,
            "-D_POSIX_C_SOURCE=200809L",
            "-std=c99",
            "-pedantic-errors",
            "-Wall",
            "-Wextra",
            "-Werror",
            "Oracle.c",
            source_path.name,
            "-o",
            str(executable),
        ]
        # shm_open needed -lrt on older glibc; it is harmless on current Linux.
        command.append("-lrt")
        subprocess.run(
            command,
            cwd=root,
            check=True,
            capture_output=True,
            text=True,
        )
        return executable

    def build_runtime(
        self, root: Path, compiler: str | None = None
    ) -> dict[str, Path]:
        compiler = compiler or self.compilers()[0]
        self.write_generated(root)
        return {
            "client": self.compile_program(root, compiler, "client", _CLIENT),
            "worker": self.compile_program(root, compiler, "worker", _WORKER),
            "query": self.compile_program(
                root, compiler, "query", _ANNOTATION_QUERY
            ),
        }

    def wait_for_socket(
        self, path: Path, process: subprocess.Popen[str]
    ) -> None:
        deadline = time.monotonic() + 5.0
        while time.monotonic() < deadline:
            if process.poll() is not None:
                stdout, stderr = process.communicate()
                self.fail(
                    "Oracle worker exited before listening: "
                    f"stdout={stdout!r}, stderr={stderr!r}"
                )
            try:
                if stat.S_ISSOCK(path.stat().st_mode):
                    return
            except FileNotFoundError:
                pass
            time.sleep(0.01)
        process.kill()
        stdout, stderr = process.communicate()
        self.fail(
            "timed out waiting for Oracle socket: "
            f"stdout={stdout!r}, stderr={stderr!r}"
        )

    def start_worker(
        self,
        worker: Path,
        socket_path: Path,
        *,
        stage: str = "rewrite",
    ) -> subprocess.Popen[str]:
        arguments = [str(worker), str(socket_path), stage]
        process = subprocess.Popen(
            arguments,
            cwd=worker.parent,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            env=self.clean_environment(),
        )
        self.wait_for_socket(socket_path, process)
        return process

    def run_client(
        self,
        client: Path,
        artifact: Path,
        socket_path: Path | None,
        *,
        check: bool,
        stage: str = "rewrite",
        max_artifact_bytes: int | None = None,
    ) -> subprocess.CompletedProcess[str]:
        arguments = [
            str(client),
            str(artifact),
            stage,
            str(socket_path) if socket_path is not None else "-",
        ]
        if max_artifact_bytes is not None:
            arguments.append(str(max_artifact_bytes))
        return subprocess.run(
            arguments,
            cwd=client.parent,
            env=self.clean_environment(),
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=check,
            timeout=10,
        )

    def assert_worker_success(self, process: subprocess.Popen[str]) -> None:
        stdout, stderr = process.communicate(timeout=10)
        self.assertEqual(
            process.returncode,
            0,
            f"worker stdout={stdout!r}, stderr={stderr!r}",
        )

    def test_oracle_c_is_strict_c99_with_gcc_and_clang(self):
        with tempfile.TemporaryDirectory(
            prefix="archon-oracle-c99-", dir=TMP_ROOT
        ) as directory:
            root = Path(directory)
            self.write_generated(root)
            for compiler in self.compilers():
                with self.subTest(compiler=compiler):
                    self.compile_program(
                        root, compiler, f"client-{compiler}", _CLIENT
                    )
                    self.compile_program(
                        root, compiler, f"worker-{compiler}", _WORKER
                    )
                    self.compile_program(
                        root,
                        compiler,
                        f"query-{compiler}",
                        _ANNOTATION_QUERY,
                    )

    def test_unconfigured_runtime_is_a_no_socket_noop(self):
        with tempfile.TemporaryDirectory(
            prefix="archon-oracle-noop-", dir=TMP_ROOT
        ) as directory:
            root = Path(directory)
            programs = self.build_runtime(root)
            artifact = root / "arbitrary.i"
            original = b"ARCHON_TEST_INPUT(1)\n"
            artifact.write_bytes(original)
            inode = artifact.stat().st_ino
            before_shm = self.shm_snapshot()

            completed = self.run_client(
                programs["client"], artifact, None, check=True
            )

            self.assertEqual(completed.stdout.splitlines()[0], "inactive")
            self.assertEqual(artifact.read_bytes(), original)
            self.assertEqual(artifact.stat().st_ino, inode)
            self.assertFalse(self.sidecar(artifact).exists())
            self.assertEqual(self.shm_snapshot(), before_shm)

    def test_all_stages_round_trip_and_bad_stage_is_rejected(self):
        stages = (
            "rewrite",
            "schedule",
            "isel",
            "isched",
            "regalloc",
            "emit",
        )
        with tempfile.TemporaryDirectory(
            prefix="archon-oracle-stages-", dir=TMP_ROOT
        ) as directory:
            root = Path(directory)
            programs = self.build_runtime(root)
            for index, stage in enumerate(stages):
                with self.subTest(stage=stage):
                    artifact = root / f"{stage}.i"
                    socket_path = root / f"stage-{index}.sock"
                    original = f"ARCHON_STAGE({stage})\n".encode("ascii")
                    artifact.write_bytes(original)
                    worker = self.start_worker(
                        programs["worker"], socket_path, stage=stage
                    )
                    self.run_client(
                        programs["client"],
                        artifact,
                        socket_path,
                        check=True,
                        stage=stage,
                    )
                    self.assert_worker_success(worker)
                    self.assertEqual(
                        artifact.read_bytes(),
                        b"/* oracle:begin */\n"
                        + original
                        + b"/* oracle:end */\n",
                    )
                    subprocess.run(
                        [
                            str(programs["query"]),
                            str(self.sidecar(artifact)),
                            "good",
                            stage,
                        ],
                        cwd=root,
                        check=True,
                        capture_output=True,
                        text=True,
                        timeout=10,
                    )
                    self.assertFalse(socket_path.exists())

            bad = root / "bad-stage.i"
            bad.write_bytes(b"unchanged\n")
            rejected = self.run_client(
                programs["client"],
                bad,
                root / "must-not-connect.sock",
                check=False,
                stage="bad",
            )
            self.assertNotEqual(rejected.returncode, 0)
            self.assertIn("stage", rejected.stdout.lower())
            self.assertEqual(bad.read_bytes(), b"unchanged\n")
            self.assertFalse(self.sidecar(bad).exists())

    def test_round_trip_atomic_writeback_annotations_and_cleanup(self):
        with tempfile.TemporaryDirectory(
            prefix="archon-oracle-roundtrip-", dir=TMP_ROOT
        ) as directory:
            root = Path(directory)
            programs = self.build_runtime(root)
            artifact = root / "custom artifact.i"
            socket_path = root / "oracle.sock"
            original = b"ARCHON_TEST_INPUT(7)\n"
            expected = (
                b"/* oracle:begin */\n"
                + original
                + b"/* oracle:end */\n"
            )
            artifact.write_bytes(original)
            old_inode = artifact.stat().st_ino
            before_shm = self.shm_snapshot()
            worker = self.start_worker(programs["worker"], socket_path)

            completed = self.run_client(
                programs["client"], artifact, socket_path, check=True
            )
            self.assertEqual(completed.stdout.splitlines()[0], "ok")
            self.assert_worker_success(worker)

            self.assertEqual(artifact.read_bytes(), expected)
            self.assertNotEqual(artifact.stat().st_ino, old_inode)
            sidecar = self.sidecar(artifact)
            annotation_text = sidecar.read_text(encoding="ascii")
            for macro in (
                "ARCHON_ORACLE_BEGIN",
                "ARCHON_ORACLE_U64",
                "ARCHON_ORACLE_I64",
                "ARCHON_ORACLE_BOOL",
                "ARCHON_ORACLE_STRING",
                "ARCHON_ORACLE_END",
            ):
                self.assertIn(macro, annotation_text)
            subprocess.run(
                [str(programs["query"]), str(sidecar), "good", "rewrite"],
                cwd=root,
                check=True,
                capture_output=True,
                text=True,
                timeout=10,
            )
            self.assertFalse(socket_path.exists())
            self.assertEqual(self.shm_snapshot(), before_shm)
            leftovers = [
                path.name
                for path in root.iterdir()
                if ".tmp" in path.name or "oracle-shm" in path.name
            ]
            self.assertEqual(leftovers, [])

    def test_every_dot_i_name_is_allowed_but_other_suffixes_are_rejected(self):
        with tempfile.TemporaryDirectory(
            prefix="archon-oracle-names-", dir=TMP_ROOT
        ) as directory:
            root = Path(directory)
            programs = self.build_runtime(root)
            nested = root / "nested"
            nested.mkdir()
            accepted = (
                root / "Program.i",
                root / "with spaces.i",
                root / ".i",
                nested / "not-a-fixed-artifact-name.i",
                root / "already.oracle.i",
            )
            for index, artifact in enumerate(accepted):
                with self.subTest(artifact=artifact.name):
                    socket_path = root / f"name-{index}.sock"
                    artifact.write_text(
                        f"ARCHON_NAME_TEST({index})\n", encoding="ascii"
                    )
                    worker = self.start_worker(
                        programs["worker"], socket_path
                    )
                    self.run_client(
                        programs["client"],
                        artifact,
                        socket_path,
                        check=True,
                    )
                    self.assert_worker_success(worker)
                    self.assertTrue(
                        artifact.read_text(encoding="ascii").startswith(
                            "/* oracle:begin */"
                        )
                    )
                    self.assertTrue(self.sidecar(artifact).is_file())
                    self.assertFalse(socket_path.exists())

            for index, name in enumerate(
                ("Program", "Program.c", "Program.I", "Program.i.txt")
            ):
                with self.subTest(rejected=name):
                    artifact = root / name
                    artifact.write_text("unchanged\n", encoding="ascii")
                    completed = self.run_client(
                        programs["client"],
                        artifact,
                        root / f"does-not-exist-{index}.sock",
                        check=False,
                    )
                    self.assertNotEqual(completed.returncode, 0)
                    self.assertIn(".i suffix", completed.stdout.lower())
                    self.assertEqual(
                        artifact.read_text(encoding="ascii"), "unchanged\n"
                    )

    def test_malformed_truncated_and_oversized_annotations_are_rejected(self):
        with tempfile.TemporaryDirectory(
            prefix="archon-oracle-annotations-", dir=TMP_ROOT
        ) as directory:
            root = Path(directory)
            programs = self.build_runtime(root)
            artifact = root / "annotations.i"
            socket_path = root / "annotations.sock"
            artifact.write_text("ARCHON_ANNOTATION_TEST()\n", encoding="ascii")
            worker = self.start_worker(programs["worker"], socket_path)
            self.run_client(
                programs["client"], artifact, socket_path, check=True
            )
            self.assert_worker_success(worker)
            good = self.sidecar(artifact)
            text = good.read_text(encoding="ascii")

            cases = {
                "malformed": text + "not_a_macro(1)\n",
                "truncated": (
                    "\n".join(text.rstrip().splitlines()[:-1]) + "\n"
                ),
            }
            for name, contents in cases.items():
                with self.subTest(case=name):
                    path = root / f"{name}.oracle.i"
                    path.write_text(contents, encoding="ascii")
                    subprocess.run(
                        [
                            str(programs["query"]),
                            str(path),
                            "reject",
                            "rewrite",
                        ],
                        cwd=root,
                        check=True,
                        capture_output=True,
                        text=True,
                        timeout=10,
                    )
            oversized = root / "oversized.oracle.i"
            oversized.write_bytes(b"x" * (8 * 1024 * 1024 + 1))
            subprocess.run(
                [
                    str(programs["query"]),
                    str(oversized),
                    "reject",
                    "rewrite",
                ],
                cwd=root,
                check=True,
                capture_output=True,
                text=True,
                timeout=10,
            )

    def fake_server_result(
        self,
        client: Path,
        root: Path,
        payload: bytes,
    ) -> subprocess.CompletedProcess[str]:
        socket_path = root / "fake.sock"
        listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        listener.bind(str(socket_path))
        listener.listen(1)
        failure: list[BaseException] = []

        def serve() -> None:
            try:
                connection, _ = listener.accept()
                with connection:
                    connection.recv(4096)
                    if payload:
                        connection.sendall(payload)
            except BaseException as error:  # Propagate thread failures.
                failure.append(error)
            finally:
                listener.close()

        thread = threading.Thread(target=serve, daemon=True)
        thread.start()
        artifact = root / "fake-response.i"
        artifact.write_bytes(b"unchanged\n")
        result = self.run_client(
            client, artifact, socket_path, check=False
        )
        thread.join(timeout=5)
        self.assertFalse(thread.is_alive())
        if failure:
            raise failure[0]
        self.assertEqual(artifact.read_bytes(), b"unchanged\n")
        self.assertFalse(self.sidecar(artifact).exists())
        try:
            socket_path.unlink()
        except FileNotFoundError:
            pass
        return result

    def test_malformed_truncated_and_oversized_protocol_messages_are_rejected(self):
        with tempfile.TemporaryDirectory(
            prefix="archon-oracle-protocol-", dir=TMP_ROOT
        ) as directory:
            root = Path(directory)
            programs = self.build_runtime(root)
            before_shm = self.shm_snapshot()

            malformed = self.fake_server_result(
                programs["client"], root, b"not-an-oracle-response"
            )
            self.assertNotEqual(malformed.returncode, 0)

            truncated = self.fake_server_result(
                programs["client"], root, b""
            )
            self.assertNotEqual(truncated.returncode, 0)

            # Oversized artifacts are rejected locally before connecting.
            artifact = root / "oversized.i"
            original = b"x" * 128
            artifact.write_bytes(original)
            oversized = self.run_client(
                programs["client"],
                artifact,
                root / "must-not-connect-oversized.sock",
                check=False,
                max_artifact_bytes=32,
            )
            self.assertNotEqual(oversized.returncode, 0)
            self.assertEqual(artifact.read_bytes(), original)
            self.assertFalse(self.sidecar(artifact).exists())

            # A well-framed request declaring an over-limit shm region is
            # rejected by the generated server before it opens a shm object.
            request_socket = root / "oversized-request.sock"
            request_worker = self.start_worker(
                programs["worker"], request_socket
            )
            connection = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            connection.connect(str(request_socket))
            connection.sendall(struct.pack(
                "<8sIIQIIQQQIIII",
                b"ARCHORC1",
                1,  # protocol version
                1,  # request message
                1,  # nonzero request ID
                1,  # rewrite stage
                0,
                0,
                64 * 1024 * 1024 + 1,
                1,
                1,
                1,
                0,
                0,
            ))
            connection.close()
            request_worker.communicate(timeout=10)
            self.assertNotEqual(request_worker.returncode, 0)
            self.assertFalse(request_socket.exists())

            # The generated server must also reject a truncated request before
            # invoking the transformation callback.
            request_socket = root / "truncated-request.sock"
            request_worker = self.start_worker(
                programs["worker"], request_socket
            )
            connection = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            connection.connect(str(request_socket))
            connection.sendall(b"AO")
            connection.close()
            request_worker.communicate(timeout=10)
            self.assertNotEqual(request_worker.returncode, 0)
            self.assertFalse(request_socket.exists())
            self.assertEqual(self.shm_snapshot(), before_shm)


if __name__ == "__main__":
    unittest.main()
