"""Regression tests for bundled QML syntax and compiled rewrite decisions."""
from __future__ import annotations
import importlib.util
import os
from pathlib import Path
import random
import subprocess
import sys
import tempfile
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


class GeneratedQMLTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        work = tempfile.TemporaryDirectory()
        cls.addClassCleanup(work.cleanup)
        cls.work = Path(work.name)
        cls.frontend = cls.work / 'qml_frontend.py'
        generator = ROOT.parent / 'LLSynth' / 'scripts' / 'llsynth.pl'
        subprocess.run(
            ['perl', generator, '--mode=interpret', '-T', 'python3',
             '-o', cls.frontend, ROOT / 'QML.llsynth'],
            check=True, capture_output=True, text=True,
        )

    def frontend_command(self, *args, source=None):
        return subprocess.run(
            [sys.executable, '-I', '-B', self.frontend, *args],
            input=source, capture_output=True, text=True,
        )

    def compile(self, *files):
        output = self.work / 'compiled.qmb'
        output.unlink(missing_ok=True)
        result = subprocess.run(
            [sys.executable, '-I', '-B', QML / 'compile_rules.py',
             '--frontend', self.frontend, '--output', output, *files],
            capture_output=True, text=True, timeout=10,
        )
        return result, output.read_bytes() if output.exists() else None

    def fragment(self, name, text):
        path = self.work / name
        path.write_text(text, encoding='utf-8')
        return path

    def test_generated_expression_tool(self):
        for source in (bundle.assemble(),
                       bundle.assemble(term_file=QML / 'examples' / 'alternate-run.qml')):
            result = self.frontend_command('-', source=source)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout, "['int', 42]\n")
        negative = 'run -9223372036854775808;'
        directives, tree = rt.parse(negative)
        self.assertEqual(rt.execute(directives, tree), (('int', -(2**63)), 'Int'))
        result = self.frontend_command('-', source=negative)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, "['int', -9223372036854775808]\n")

    def test_generated_frontend_exports_directive_records(self):
        result = self.frontend_command('-', source='rewrite p id : op(?x) => ?x;')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, "[['R', 'p', 'id', ['node', 'op', [['meta', 'x']]], ['meta', 'x']]]\n")

    def test_compile_rules_preserves_order_and_encodes_every_tree_tag(self):
        first = self.fragment('first.qpass', '''# A fragment, not an expression program
pass canonicalize;
rewrite canonicalize id : add(?x, 000) => ?x;
reject canonicalize forbidden : node(_,
    true, false, name, empty(), -9223372036854775808, 9223372036854775807);
''')
        second = self.fragment('second.qpass', 'pass another; # no final newline')
        result, compiled = self.compile(first, second)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(compiled,
                         b'CEPHYR-QML-1\n'
                         b'P canonicalize\n'
                         b'R canonicalize id n add 2 m x i 0 m x\n'
                         b'X canonicalize forbidden n node 7 w b 1 b 0 s name n empty 0 '
                         b'i -9223372036854775808 i 9223372036854775807\n'
                         b'P another\n')

    def test_compile_builtins(self):
        result, compiled = self.compile(QML / 'NativeCanonicalize.qpass',
                                        QML / 'NativeStrengthReduce.qpass')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(compiled.splitlines(), [
            b'CEPHYR-QML-1',
            b'R canonicalize add_zero_right n add 2 m x i 0 m x',
            b'R canonicalize add_zero_left n add 2 i 0 m x m x',
            b'R canonicalize sub_zero n sub 2 m x i 0 m x',
            b'R canonicalize mul_one_right n mul 2 m x i 1 m x',
            b'R canonicalize mul_one_left n mul 2 i 1 m x m x',
            b'R canonicalize or_zero_right n or 2 m x i 0 m x',
            b'R canonicalize or_zero_left n or 2 i 0 m x m x',
            b'R canonicalize xor_zero_right n xor 2 m x i 0 m x',
            b'R canonicalize xor_zero_left n xor 2 i 0 m x m x',
            b'R canonicalize shl_zero n shl 2 m x i 0 m x',
            b'R canonicalize lshr_zero n lshr 2 m x i 0 m x',
            b'R canonicalize ashr_zero n ashr 2 m x i 0 m x',
            b'R strength_reduce double_right n mul 2 m x i 2 n add 2 m x m x',
            b'R strength_reduce double_left n mul 2 i 2 m x n add 2 m x m x',
        ])

    def test_fragments_reject_invalid_input_without_output(self):
        good = self.fragment('good.qpass', 'pass canonicalize;')
        cases = [
            ('rewrite p bad : f(?x) => ?y;', '!UnknownMeta'),
            ('rewrite p bad : f(?x) => _;', 'wildcard in replacement'),
            ('rewrite p bad : f(?x) => ?x', 'PARSE'),
            ('pass canonicalize; @', 'LEX'),
            ('reject p big : 9223372036854775808;', 'INTEGER_OVERFLOW'),
            ('reject p small : -9223372036854775809;', 'INTEGER_OVERFLOW'),
            ('run div(1, 0);', "'run' is only supported by the expression tool"),
            ('#' + ' ' * 65536, 'total rule input exceeds'),
            ('pass p;\n' * 128, 'too many directives'),
            ('reject p deep : ' + 'f(' * 32 + '_' + ')' * 32 + ';', 'tree nesting exceeds'),
            ('reject p wide : f(' + ','.join(['_'] * 33) + ');', 'more than 32 arguments'),
            (''.join(f'reject p rule_{i} : f(f({",".join(["_"] * 31)}), _);\n'
                     for i in range(125)), 'too many tree terms'),
            ('pass ' + 'a' * 65 + ';', 'identifier is longer than 64 characters'),
        ]
        for text, message in cases:
            with self.subTest(message=message):
                second = self.fragment('bad.qpass', text)
                result, compiled = self.compile(good, second)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn('bad.qpass:', result.stderr)
                self.assertIn(message, result.stderr)
                self.assertIsNone(compiled)

    def test_fragments_are_parsed_independently(self):
        first = self.fragment('incomplete.qpass', 'rewrite p incomplete : add(?x, 0) =>')
        second = self.fragment('continuation.qpass', '?x;')
        result, compiled = self.compile(first, second)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('incomplete.qpass:', result.stderr)
        self.assertIsNone(compiled)

    def test_named_pipe_does_not_block_rule_compilation(self):
        pipe = self.work / 'pipe.qpass'
        os.mkfifo(pipe)
        result, compiled = self.compile(pipe)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('expected a regular rule file', result.stderr)
        self.assertIsNone(compiled)


if __name__ == '__main__':
    unittest.main()
