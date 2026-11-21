"""Regression tests for bundled QML syntax and compiled rewrite decisions."""
from __future__ import annotations
import importlib.util
from pathlib import Path
import random
import subprocess
import sys
import unittest

QML = Path(__file__).resolve().parents[1]
ROOT = QML.parents[0]

spec = importlib.util.spec_from_file_location('cephyr_qml_runtime', QML / 'runtime.py')
rt = importlib.util.module_from_spec(spec)
spec.loader.exec_module(rt)
spec_bundle = importlib.util.spec_from_file_location('cephyr_qml_bundle', QML / 'bundle.py')
bundle = importlib.util.module_from_spec(spec_bundle)
spec_bundle.loader.exec_module(bundle)

class QMLTests(unittest.TestCase):
    def test_manifest_and_all_scaffolds_exist(self):
        self.assertEqual(len(bundle.ORDER), 9)
        for item in (*bundle.ORDER, 'Orchestrator.qml'):
            self.assertTrue((QML/item).is_file())
            self.assertTrue((QML/item).stat().st_size > 0)
        self.assertTrue((ROOT/'QML.llsynth').is_file())
        self.assertIn('.import "LLSynth/stdlib/QMLCore.llsynth"',
                      (ROOT/'QML.llsynth').read_text())
        self.assertNotIn('\n..\n', (ROOT/'QML.llsynth').read_text())

    def test_bundle_parses(self):
        ds, t = rt.parse(bundle.assemble())
        self.assertEqual(len(ds), 51)
        self.assertEqual(ds[-1], ('pass', 'sema'))
        self.assertEqual(t[1], 'pretty')
        self.assertEqual(bundle.assemble(), (QML/'bundled.qml').read_text())

    def test_demo_reduces_to_42(self):
        ds, t = rt.parse(bundle.assemble())
        self.assertEqual(rt.execute(ds,t), (('int',42),'Int'))

    def test_reference_pattern_compilation(self):
        r = rt.rule('p','nonlinear',rt.n('pair',rt.meta('x'),rt.meta('x')),rt.meta('x'))
        rt.validate_rule(r)
        compiled=rt.compile_rule(r)
        self.assertEqual(rt.try_rule(compiled,rt.n('pair',rt.integer(7),rt.integer(7))),rt.integer(7))
        self.assertIsNone(rt.try_rule(compiled,rt.n('pair',rt.integer(7),rt.integer(8))))

    def test_decision_vs_reference_matcher(self):
        rand=random.Random(17433)
        def term(depth):
            if depth==0 or rand.randrange(4)==0:
                return rt.integer(rand.randrange(4))
            return rt.n(rand.choice(('a','b','c')),*(term(depth-1) for _ in range(rand.randrange(3))))
        pat=rt.n('a',rt.meta('x'),rt.n('b',rt.meta('x')))
        tree=rt.compile_decision(pat,(),('leaf',rt.meta('x')),('fail',))
        for _ in range(2000):
            subject=term(3)
            env={}
            expected=rt.instantiate(rt.meta('x'),env) if rt.match(pat,subject,env) else None
            self.assertEqual(rt.execute_decision(tree,subject,{}),expected)

    def test_semantic_rejection(self):
        text='''reject sema forbidden : poison(); pass sema; run poison();'''
        d,t=rt.parse(text)
        with self.assertRaisesRegex(RuntimeError, 'Rejected: forbidden'):
            rt.execute(d,t)

    def test_lexical_shadowing(self):
        d,t=rt.parse('''pass sema; run let(x, 40, let(x, true, if(var(x), 42, 0)));''')
        _,typ=rt.execute(d,t)
        self.assertEqual(typ,'Int')

    def test_invalid_replacement_rejected(self):
        d,t=rt.parse('''rewrite user broken : f(?a) => g(?b); pass user; run f(2);''')
        with self.assertRaisesRegex(RuntimeError,'unbound replacement metavariable b'):
            rt.execute(d,t)

    def test_integer_overflow(self):
        d,t=rt.parse('''pass optimize; run add(9223372036854775807, 1);''')
        with self.assertRaisesRegex(RuntimeError,'overflow'):
            rt.execute(d,t)

    def test_alternate_term(self):
        alt=QML/'examples'/'alternate-run.qml'
        text=bundle.assemble(term_file=alt)
        d,t=rt.parse(text)
        tree,typ=rt.execute(d,t)
        self.assertEqual((tree,typ),(('int',42),'Int'))

if __name__ == '__main__':
    unittest.main()
