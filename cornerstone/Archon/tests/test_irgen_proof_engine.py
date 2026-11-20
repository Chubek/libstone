"""Direct tests for the standalone solver embedded in generated Proof.py."""

from itertools import product
import unittest

from archon.irgen.tool_templates import proof_engine


def _bv(value, width):
    return f"#b{value:0{width}b}"


class ProofEngineTests(unittest.TestCase):
    def test_dpll_agrees_with_exhaustive_two_variable_cnf(self):
        binary = tuple((a, b) for a in (-1, 1) for b in (-2, 2))
        possible_clauses = ((), (-1,), (1,), (-2,), (2,)) + binary
        assignments = list(product((False, True), repeat=2))

        def satisfies(clauses, assignment):
            return all(
                any(assignment[abs(lit) - 1] == (lit > 0) for lit in clause)
                for clause in clauses
            )

        for present in product((False, True), repeat=len(possible_clauses)):
            clauses = [c for c, keep in zip(possible_clauses, present) if keep]
            witness = proof_engine._proof_dpll(clauses, 2, proof_engine._ProofBudget(1000))
            with self.subTest(clauses=clauses):
                expected_sat = any(satisfies(clauses, assignment) for assignment in assignments)
                self.assertEqual(witness is not None, expected_sat)
                if witness is not None:
                    self.assertTrue(satisfies(clauses, [v > 0 for v in witness[1:]]))

    def test_boolean_commands_and_total_model(self):
        text = """
        ; The implication associates to the right, so it is true.
        (set-option :produce-models true)
        (set-logic QF_UF)
        (declare-const p Bool)
        (declare-fun q () Bool)
        (declare-fun unused () Bool)
        (assert (=> false true false))
        (assert (or p q))
        (assert (not p))
        (assert (xor p q false))
        (check-sat)
        (get-model)
        (exit)
        """
        status, model = proof_engine.solve(text)
        self.assertEqual((status, model), ("sat", {"p": False, "q": True, "unused": False}))
        self.assertIs(type(model["q"]), bool)
        self.assertEqual(proof_engine.solve("(assert (distinct true false true))"), ("unsat", {}))

    def test_let_is_simultaneous_and_nullary_definitions_are_not_model_variables(self):
        text = """
        (set-logic QF_BV)
        (declare-const x (_ BitVec 3))
        (declare-const unused (_ BitVec 1))
        (define-fun next () (_ BitVec 3) (bvadd x (_ bv0 3) (_ bv1 3)))
        (assert (= (let ((x next) (previous x)) (bvsub x previous)) #b001))
        (assert (= (let ((x next) (previous x)) (concat x previous)) #b000111))
        (assert (ite (distinct x next) (= x #b111) false))
        """
        status, model = proof_engine.solve(text)
        self.assertEqual((status, model), ("sat", {"x": 7, "unused": 0}))
        self.assertIs(type(model["unused"]), int)

    def test_bit_vector_operations_against_exhaustive_small_input_values(self):
        for width in range(1, 4):
            size = 1 << width
            mask = size - 1
            sign = size >> 1
            for x, y in product(range(size), repeat=2):
                sx, sy = (x ^ sign) - sign, (y ^ sign) - sign
                prelude = f"""
                (set-logic QF_BV)
                (declare-const x (_ BitVec {width}))
                (declare-const y (_ BitVec {width}))
                (assert (= x {_bv(x, width)}))
                (assert (= y {_bv(y, width)}))
                """
                arithmetic = [
                    ("(bvadd x y)", x + y), ("(bvsub x y)", x - y),
                    ("(bvmul x y)", x * y), ("(bvneg x)", -x),
                    ("(bvand x y)", x & y), ("(bvor x y)", x | y),
                    ("(bvxor x y)", x ^ y), ("(bvnot x)", ~x),
                    ("(bvnand x y)", ~(x & y)), ("(bvnor x y)", ~(x | y)),
                    ("(bvxnor x y)", ~(x ^ y)), ("(bvshl x y)", x << y),
                    ("(bvlshr x y)", x >> y), ("(bvashr x y)", sx >> y),
                ]
                checks = [f"(= {term} {_bv(value & mask, width)})" for term, value in arithmetic]
                for suffix, u, s in (
                    ("lt", x < y, sx < sy), ("le", x <= y, sx <= sy),
                    ("gt", x > y, sx > sy), ("ge", x >= y, sx >= sy),
                ):
                    for kind, holds in (("u", u), ("s", s)):
                        term = f"(bv{kind}{suffix} x y)"
                        checks.append(term if holds else f"(not {term})")
                checks.append(f"(= (bvcomp x y) {_bv(int(x == y), 1)})")
                checks.append(f"(= (ite (bvult x y) x y) {_bv(min(x, y), width)})")
                conjunction = f"(and {' '.join(checks)})"
                with self.subTest(width=width, x=x, y=y):
                    self.assertEqual(
                        proof_engine.solve(f"{prelude}(assert {conjunction})"),
                        ("sat", {"x": x, "y": y}),
                    )
                    # Looking for a counterexample tests the other direction:
                    # a circuit gate whose result was left underconstrained
                    # would allow a false output even for these fixed inputs.
                    self.assertEqual(
                        proof_engine.solve(f"{prelude}(assert (not {conjunction}))"),
                        ("unsat", {}),
                    )

    def test_symbolic_sat_model_and_contradictions(self):
        prelude = """
        (declare-fun x () (_ BitVec 3))
        (declare-fun y () (_ BitVec 3))
        (assert (bvult x y))
        (assert (= (bvmul x y) #b110))
        (assert (distinct x y #b000))
        """
        status, model = proof_engine.solve(prelude)
        expected = {
            (x, y) for x, y in product(range(1, 8), repeat=2) if x < y and (x * y) % 8 == 6
        }
        self.assertEqual(status, "sat")
        self.assertIn((model["x"], model["y"]), expected)
        self.assertEqual(proof_engine.solve(prelude + "(assert (= x y))"), ("unsat", {}))
        self.assertEqual(proof_engine.solve(prelude + "(assert (= x #b100))"), ("unsat", {}))

    def test_indexed_operations_and_width_boundaries(self):
        text = """
        (declare-const nibble (_ BitVec 4))
        (declare-const word (_ BitVec 64))
        (assert (= nibble ((_ extract 7 4) (concat #xA nibble))))
        (assert (= ((_ zero_extend 4) nibble) #x0a))
        (assert (= ((_ sign_extend 4) nibble) #xFa))
        (assert (= (bvadd word (_ bv1 64)) (_ bv0 64)))
        """
        self.assertEqual(
            proof_engine.solve(text), ("sat", {"nibble": 10, "word": (1 << 64) - 1}),
        )
        self.assertEqual(
            proof_engine.solve("""
                (declare-const x (_ BitVec 64))
                (declare-const y (_ BitVec 64))
                (assert (not (= (bvadd x y) (bvadd y x))))
            """),
            ("unsat", {}),
        )

    def test_malformed_or_unsupported_input_is_not_used_as_a_proof(self):
        for text in (
            "", "(assert true", "()", "(assert true))", "(assert 1)",
            "(declare-const x (_ BitVec 0))",
            "(assert (= #b10 (_ bv4 2)))",
            "(assert (= #b0 #b00))",
            "(assert (= true #b1))",
            "(assert #b1)",
            "(assert (ite true #b1 false))",
            "(assert (bvshl #b1 #b00))",
            "(assert (= ((_ extract 0 1) #b01) #b00))",
            "(assert (= #xgg #b0))",
            "(declare-const x Int)",
            "(declare-fun f (Bool) Bool)",
            "(declare-const |quoted name| Bool)",
            "(declare-const true Bool)",
            "(define-fun loop () Bool loop)",
            "(assert (let ((a true) (b a)) b))",
            "(assert (let ((a true) (a false)) a))",
            "(declare-const x Bool)(declare-const x Bool)",
            "(set-logic QF_LIA)",
            "(set-logic QF_BV)(set-logic QF_UF)",
            "(declare-const x Bool)(set-logic QF_BV)",
            "(set-logic QF_UF)(declare-const x (_ BitVec 8))",
            "(set-logic QF_UF)(assert (= #b1 #b1))",
            "(assert false)(assert (= (bvudiv #b1 #b1) #b1))",
            "(assert (and false undeclared))",
            "(push 1)(assert false)(pop 1)",
            "(check-sat)(assert false)",
            "(check-sat)(check-sat)",
            "(get-model)",
            "(exit)(assert true)",
        ):
            with self.subTest(text=text):
                with self.assertRaises(ValueError):
                    proof_engine.solve(text)

    def test_work_limit_and_hard_bounds_return_unknown_without_model(self):
        text = """
        (declare-const p Bool)
        (declare-const q Bool)
        (assert (or p q))
        (assert (or p (not q)))
        (assert (or (not p) q))
        (assert (or (not p) (not q)))
        """
        self.assertEqual(proof_engine.solve(text), ("unsat", {}))
        self.assertEqual(proof_engine.solve(text, max_steps=100), ("unknown", {}))
        self.assertEqual(proof_engine.solve("(assert true)", max_steps=0), ("unknown", {}))
        self.assertEqual(
            proof_engine.solve("(declare-const x (_ BitVec 65))(assert false)"),
            ("unknown", {}),
        )
        self.assertEqual(
            proof_engine.solve("(assert " + "(not " * 128 + "false" + ")" * 129),
            ("unknown", {}),
        )
        with self.assertRaises(ValueError):
            proof_engine.solve("(assert true)", max_steps=-1)


if __name__ == "__main__":
    unittest.main()
