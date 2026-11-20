"""Exercise the standalone proof CLI and its connection to generated rule data."""

import copy
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest

from archon.compiler import compile_file
from archon.irgen import generate_ir, graphs, isel, proof_tool, rewrite


ROOT = Path(__file__).resolve().parents[1]
IDENTITY = "rewrite:RISCV32:identity.%2B%25.32.1:bits32.bits32.bits32"
COMMUTE = "rewrite:RISCV32:commute.%2B%25.32:bits32.bits32.bits32"
ADDI = "isel:RISCV32:add:ADDI"


class GeneratedProofTool(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.rv32 = compile_file(ROOT / "stdspec" / "riscv" / "rv32i.arch")
        cls.rv64 = compile_file(ROOT / "stdspec" / "riscv" / "rv64i.arch")

    def setUp(self):
        directory = tempfile.TemporaryDirectory(dir="/tmp/opencode")
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name)
        self.out = self.root / "generated"
        self.out.mkdir()
        self.emit(self.rv32)

    def emit(self, db, **options):
        for name, contents in proof_tool.emit({"architectures": [db], "options": options}).items():
            (self.out / name).write_text(contents, encoding="utf-8")

    def run_cli(self, *args):
        # Run outside both the generated directory and the source tree; only
        # stdlib files plus the tool and explicit input should be needed.
        return subprocess.run(
            [sys.executable, "-I", str(self.out / "Proof.py"), *map(str, args)],
            cwd=self.root,
            text=True,
            capture_output=True,
            timeout=15,
        )

    def write_term(self, name, result, inputs=("x",)):
        path = self.root / name
        path.write_text(json.dumps({
            "target": "RISCV32", "width": 32, "inputs": list(inputs), "result": result,
        }), encoding="utf-8")
        return path

    def test_real_inventory_and_deterministic_id_scope(self):
        listing = self.run_cli("--list-rewrites")
        self.assertEqual(listing.returncode, 0, listing.stderr)
        self.assertIn("not applied events", listing.stdout)
        self.assertIn(f"{IDENTITY}\tmodeled\tidentity\tPatternBank_RISCV32.i", listing.stdout)
        self.assertIn(f"{ADDI}\tmodeled\tselect\tISelPattern_RISCV32.i", listing.stdout)
        self.assertIn("isel:RISCV32:target%3ASRA:SRA\tunknown\texplicit", listing.stdout)
        self.assertIn("late:RISCV32:register-homes\tunknown\tboundary", listing.stdout)

        rw = rewrite.emit(self.rv32)["PatternBank_RISCV32.i"]
        self.assertIn('ARCHON_IDENTITY_RULE("identity.+%.32.1"', rw)
        sel = isel.emit(self.rv32)["ISelPattern_RISCV32.i"]
        self.assertIn('"ADDI", "add", 32u, 2u, 2u, 2u, 32u, 12u', sel)

        both = proof_tool.emit({"architectures": [self.rv32, self.rv64]})
        self.assertEqual(both, proof_tool.emit({"architectures": [self.rv64, self.rv32]}))
        self.assertIn(IDENTITY, both["Proofs.smt"])
        self.assertIn("rewrite:RISCV64:identity.%2B%25.64.1:bits64.bits64.bits64", both["Proofs.smt"])

        result = self.run_cli("--verify", "all")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(f"{IDENTITY}: proved (unsat", result.stdout)
        self.assertIn(f"{ADDI}: proved (unsat", result.stdout)
        self.assertIn("pipeline:RISCV32:rewrite-bridge: unknown", result.stdout)
        self.assertIn("0 disproved", result.stdout)

    def test_optional_external_transforms_are_unknown_boundaries_only_when_selected(self):
        for options in ({}, {"linker": "yld", "lto": False}, {"linker": "lld", "lto": True}):
            with self.subTest(options=options):
                self.emit(self.rv64, **options)
                listing = self.run_cli("--list-rewrites")
                self.assertEqual(listing.returncode, 0, listing.stderr)
                self.assertNotIn("sandstone-asmopt", listing.stdout)
                self.assertNotIn("yld-section-gc", listing.stdout)

        outputs = generate_ir("anf", [self.rv32, self.rv64], options={
            "target": "RISCV64", "assembler": "sandstone",
            "sandstone_bank": "bank.archbank", "sandstone_syntax": "default",
            "linker": "yld", "lto": True,
        })
        self.assertIn("AsmOPT.lua", outputs)
        self.assertIn("LTO.lua", outputs)
        for name in ("Proof.py", "Proofs.smt"):
            (self.out / name).write_text(outputs[name], encoding="utf-8")
        listing = self.run_cli("--list-rewrites")
        self.assertEqual(listing.returncode, 0, listing.stderr)
        asmopt = "late:RISCV64:sandstone-asmopt"
        section_gc = "late:RISCV64:yld-section-gc"
        self.assertIn(f"{asmopt}\tunknown\tboundary\tAsmOPT.lua", listing.stdout)
        self.assertIn("Sandstone checks conditional alias byte equivalence", listing.stdout)
        self.assertIn(f"{section_gc}\tunknown\tboundary\tLTO.lua", listing.stdout)
        self.assertIn("reachability, not value SMT", listing.stdout)
        result = self.run_cli("--verify", "all")
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertIn(f"{asmopt}: unknown", result.stdout)
        self.assertIn(f"{section_gc}: unknown", result.stdout)

    def test_renamed_corpus_is_resolved_explicitly_and_bound_to_tool(self):
        default = self.out / "Proofs.smt"
        other = self.out / "Renamed.smt"
        default.rename(other)
        absent = self.run_cli("--list-rewrites")
        self.assertNotEqual(absent.returncode, 0)
        self.assertIn("Proofs.smt", absent.stderr)
        renamed = self.run_cli("--proofs", other, "--list-rewrites")
        self.assertEqual(renamed.returncode, 0, renamed.stderr)
        self.assertIn(IDENTITY, renamed.stdout)
        with other.open("a", encoding="utf-8") as stream:
            stream.write("\n; not the original generated corpus\n")
        altered = self.run_cli("--proofs", other, "--verify", "all")
        self.assertNotEqual(altered.returncode, 0)
        self.assertIn("does not match", altered.stderr)
        self.assertNotIn("proved", altered.stdout)

    def test_standalone_smt_sat_unsat_and_unsupported(self):
        (self.out / "Proofs.smt").unlink()  # --run is independent of the inventory.
        query = self.root / "MyProof.smt"
        query.write_text(
            "(set-logic QF_BV)\n"
            "(declare-const k (_ BitVec 8))\n"
            "(declare-const enabled Bool)\n"
            "(assert (and enabled (= k #xa5)))\n"
            "(check-sat)\n(get-model)\n",
            encoding="ascii",
        )
        sat = self.run_cli("--run", query)
        self.assertEqual(sat.returncode, 0, sat.stderr)
        self.assertEqual(sat.stdout.splitlines()[0], "sat")
        self.assertEqual(json.loads(sat.stdout.split("model: ")[1]), {"enabled": True, "k": 165})

        query.write_text("(set-logic QF_BV) (assert false) (check-sat)\n", encoding="ascii")
        unsat = self.run_cli("--run", query)
        self.assertEqual(unsat.returncode, 0, unsat.stderr)
        self.assertEqual(unsat.stdout.strip(), "unsat")

        query.write_text("(set-logic ALL) (check-sat)\n", encoding="ascii")
        unsupported = self.run_cli("--run", query)
        self.assertNotEqual(unsupported.returncode, 0)
        self.assertIn("unsupported SMT-LIB logic", unsupported.stderr)

        query.write_text(
            "(set-logic QF_BV) (declare-const too_wide (_ BitVec 65)) (check-sat)\n",
            encoding="ascii",
        )
        unknown = self.run_cli("--run", query)
        self.assertNotEqual(unknown.returncode, 0)
        self.assertEqual(unknown.stdout.strip(), "unknown")

    def test_pair_verification_requires_one_actual_term_instance(self):
        before = self.write_term("before.json", ["xor", ["add", "x", 0], 17])
        after = self.write_term("after.json", ["xor", "x", 17])
        result = self.run_cli("--verify", IDENTITY, before, after)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("one-step term instance", result.stdout)

        unrelated = self.write_term("unrelated.json", ["xor", 17, "x"])
        result = self.run_cli("--verify", IDENTITY, before, unrelated)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("not a one-step rule instance", result.stdout)
        self.assertNotIn("proved", result.stdout)

        wrong = self.write_term("different.json", ["xor", "x", 16])
        result = self.run_cli("--verify", IDENTITY, before, wrong)
        self.assertEqual(result.returncode, 1)
        self.assertIn("counterexample", result.stdout)
        self.assertIn("does not disprove the rule", result.stdout)
        self.assertNotIn(f"{IDENTITY}: disproved", result.stdout)
        model = json.loads(result.stdout.split("model: ")[1])
        self.assertNotEqual(model["x"] ^ 17, model["x"] ^ 16)

        result = self.run_cli("--verify", "oracle:runtime-proposal", before, after)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("unknown generated rule ID", result.stderr)

        result = self.run_cli("--verify", "isel:RISCV32:target%3AADD:ADD", before, after)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("unknown: explicit target choice", result.stdout)

        unsupported = self.root / "Program.i"
        unsupported.write_text("ARCHON_REWRITE_IDENTITY(0)\n", encoding="ascii")
        result = self.run_cli("--verify", IDENTITY, unsupported, after)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(".i/MIR unsupported", result.stderr)

    def test_search_and_target_rename_direction(self):
        term = self.write_term("term.json", ["add", "x", 0])
        result = self.run_cli("--find-error", "--rewrite", IDENTITY, "--programs", term)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("1 matching one-step term instance", result.stdout)
        result = self.run_cli("--find-error", "--rewrite", "all", "--programs", term)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Search unknown", result.stdout)

        db = copy.deepcopy(self.rv32)
        sub = next(i for i in db["instructions"] if i["name"] == "SUB")
        expr = sub["semantics"]["statements"][0]["value"]
        expr["left"], expr["right"] = expr["right"], expr["left"]
        self.emit(db)

        before = self.write_term("target.json", ["target", "SUB", "x", "y"], ("x", "y"))
        after = self.write_term("value.json", ["sub", "y", "x"], ("x", "y"))
        rule = "rewrite:RISCV32:lower.SUB"
        result = self.run_cli("--verify", rule, before, after)
        self.assertEqual(result.returncode, 0, result.stderr)
        result = self.run_cli("--verify", rule, after, before)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("not a one-step rule instance", result.stdout)

    def test_disproves_wrong_generated_immediate_extension_with_model(self):
        db = copy.deepcopy(self.rv32)
        addi = next(i for i in db["instructions"] if i["name"] == "ADDI")
        extension = addi["semantics"]["statements"][0]["value"]["right"]
        # The actual selector still registers a signed immediate pattern, but
        # ArchSem now zero-extends its bits instead of sign-extending them.
        extension["op"] = "zext"
        extension["type"]["kind"] = "uint"
        self.emit(db)

        verified = self.run_cli("--verify", "all")
        self.assertEqual(verified.returncode, 1, verified.stderr)
        self.assertIn(f"{ADDI}: disproved", verified.stdout)
        encoded = json.loads(verified.stdout.split("model: ")[1].splitlines()[0])["v1"]
        self.assertTrue(encoded & (1 << 11))

        before = self.write_term("negative.json", ["add", "x", -1])
        after = self.write_term("selected.json", ["target", "ADDI", "x", -1])
        pair = self.run_cli("--verify", ADDI, before, after)
        self.assertEqual(pair.returncode, 1, pair.stderr)
        self.assertIn("provided term pair", pair.stdout)

        found = self.run_cli("--find-error", "--rewrite", ADDI, "--programs", before)
        self.assertEqual(found.returncode, 1, found.stderr)
        self.assertIn("site root", found.stdout)
        model = json.loads(found.stdout.split("model: ")[1].splitlines()[0])
        self.assertNotEqual((model["x"] - 1) & 0xffffffff, (model["x"] + 4095) & 0xffffffff)

        too_large = self.write_term("not-encodable.json", ["add", "x", 2048])
        no_match = self.run_cli("--find-error", "--rewrite", ADDI, "--programs", too_large)
        self.assertNotEqual(no_match.returncode, 0)
        self.assertIn("0 matching one-step term instance", no_match.stdout)

    def test_runtime_observer_uses_inventory_ids_without_changing_legacy_macros(self):
        command = shlex.split(os.environ.get("CC", "cc"))
        if not command or not shutil.which(command[0]):
            self.skipTest("C99 compiler unavailable")
        context = {"architectures": [self.rv32], "options": {"oracle": False}}
        files = {**graphs.emit(None), **rewrite.emit(context)}
        for name, contents in files.items():
            (self.out / name).write_text(contents, encoding="utf-8")
        (self.out / "observer.c").write_text(_OBSERVER_C, encoding="ascii")
        subprocess.run(
            [*command, "-std=c99", "-Wall", "-Wextra", "-Werror", "-pedantic",
             "Graphs.c", "Rewrite.c", "observer.c", "-o", "observer"],
            cwd=self.out, check=True, timeout=15,
        )
        observed = subprocess.run(
            [str(self.out / "observer")], cwd=self.out, check=True,
            timeout=15, text=True, capture_output=True,
        ).stdout.splitlines()
        self.assertEqual(observed[0], f"1 {IDENTITY}")
        self.assertIn(f"2 {COMMUTE}", observed)
        listing = self.run_cli("--list-rewrites").stdout
        for event in observed:
            self.assertIn(event.split(maxsplit=1)[1] + "\tmodeled", listing)


_OBSERVER_C = r"""
#include "Rewrite.h"
#include <assert.h>
#include <stdio.h>

static void report(void *user, const char *id, ArchonRewriteEngine engine) {
    size_t *seen = (size_t *)user;
    ++*seen;
    assert(id != NULL);
    assert(printf("%u %s\n", (unsigned)engine, id) > 0);
}

int main(void) {
    ArchonRewriteGraph graph;
    ArchonGraphsAPI api;
    ArchonGraphsNodeId inputs[2], initial, output;
    ArchonRewriteStats stats;
    size_t seen = 0;
    assert(archon_rewrite_generated_rule_id(archon_generated_rewrite_rule_count) == NULL);
    assert(archon_rewrite_graph_init(&graph) == ARCHON_REWRITE_OK);
    api = archon_rewrite_graph_api(&graph);
    assert(archon_rewrite_graph_intern(&graph,
        archon_rewrite_opcode("source:value:bits:32"), 1, ARCHON_GRAPHS_FLAG_PURE,
        NULL, 0, &inputs[0]) == ARCHON_REWRITE_OK);
    assert(archon_rewrite_graph_intern(&graph,
        archon_rewrite_opcode("sem:constant:bits:32"), 0,
        ARCHON_GRAPHS_FLAG_PURE | ARCHON_GRAPHS_FLAG_CONSTANT,
        NULL, 0, &inputs[1]) == ARCHON_REWRITE_OK);
    assert(archon_rewrite_graph_intern(&graph,
        archon_rewrite_opcode("sem:+%:bits:32"), 0, ARCHON_GRAPHS_FLAG_PURE,
        inputs, 2, &initial) == ARCHON_REWRITE_OK);
    assert(archon_rewrite_run_generated_observed(&api, initial,
        ARCHON_REWRITE_DIRECTED, NULL, &output, &stats, report, &seen) == ARCHON_REWRITE_OK);
    assert(seen == stats.applications && seen == 1 && output == inputs[0]);

    seen = 0;
    assert(archon_rewrite_run_generated_observed(&api, initial,
        ARCHON_REWRITE_EQUALITY_SATURATION, NULL, &output, &stats, report, &seen)
        == ARCHON_REWRITE_OK);
    assert(seen == stats.applications && seen >= 2);
    archon_rewrite_graph_dispose(&graph);
    return 0;
}
"""
