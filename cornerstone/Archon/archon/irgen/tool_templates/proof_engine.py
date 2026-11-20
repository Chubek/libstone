"""Standalone, bounded SAT engine for Archon's generated proof tool."""

# The DPLL search is adapted from third_party/satie/include/SatieDPLL.hpp.
# Tseitin gates and modular ripple-carry addition are adapted from
# third_party/satie/include/SatieTheoryUtils.hpp and SatieBV.hpp.
# Satie's README credits GPT 3.5 Codex for help creating the library.
#
# MIT License
#
# Copyright (c) 2026 Chubak Bidpaa <chubakbidpaa@riseup.net>
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in all
# copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.

import re as _proof_re


_PROOF_MAX_INPUT = 256 * 1024
_PROOF_MAX_TOKENS = 32 * 1024
_PROOF_MAX_NESTING = 96
_PROOF_MAX_WIDTH = 64
_PROOF_MAX_VARIABLES = 32 * 1024
_PROOF_MAX_CLAUSES = 96 * 1024
_PROOF_DEFAULT_MAX_STEPS = 4_000_000

# SMT Boolean terms are signed SAT literals; BV terms are LSB-first tuples
# of literals, including at width one. Literal 1 is the constant true.
_ProofValue = int | tuple[int, ...]
_PROOF_TRUE, _PROOF_FALSE = 1, -1


class _ProofLimit(Exception):
    """Internal resource cutoff; a cutoff carries no SAT or UNSAT evidence."""


class _ProofBudget:
    def __init__(self, steps: int):
        self.remaining = steps

    def spend(self, steps: int = 1) -> None:
        self.remaining -= steps
        if self.remaining < 0:
            raise _ProofLimit


def _proof_assign(literal: int, values: list[int], trail: list[int]) -> None:
    variable = abs(literal)
    values[variable] = 1 if literal > 0 else -1
    trail.append(variable)


def _proof_propagate(clauses, values, trail, budget):
    """Force unit clauses to a fixed point, or detect a falsified clause."""
    while True:
        mark = len(trail)
        for clause in clauses:
            budget.spend(1 + len(clause))
            unassigned = 0
            multiple = False
            for literal in clause:
                value = values[abs(literal)]
                if value == 0:
                    multiple |= unassigned != 0
                    unassigned = literal
                elif (value > 0) == (literal > 0):
                    break
            else:
                if unassigned == 0:
                    return False
                if not multiple:
                    _proof_assign(unassigned, values, trail)
        if len(trail) == mark:
            return True


def _proof_unresolved(clauses, values, budget):
    """Count polarities and frequencies over unresolved clauses only."""
    polarity, frequency = {}, {}
    for clause in clauses:
        budget.spend(1 + len(clause))
        pending = []
        for literal in clause:
            value = values[abs(literal)]
            if value == 0:
                pending.append(literal)
            elif (value > 0) == (literal > 0):
                break
        else:
            # Unit propagation has already ruled out falsified clauses.
            for literal in pending:
                variable = abs(literal)
                sign = 1 if literal > 0 else -1
                polarity[variable] = sign if polarity.get(variable, sign) == sign else 0
                frequency[variable] = frequency.get(variable, 0) + 1
    return polarity, frequency


def _proof_dpll(clauses, nvars, budget):
    """Satie-style chronological DPLL with an explicit backtracking trail."""
    values = [0] * (nvars + 1)
    trail = []
    # A saved frame represents the negative sibling of a positive decision.
    # Pop it before visiting that sibling, so no branch can be visited twice.
    decisions = []
    while True:
        budget.spend()
        if _proof_propagate(clauses, values, trail, budget):
            polarity, frequency = _proof_unresolved(clauses, values, budget)
            if not frequency:
                # Every clause has an assigned true literal. Unassigned bits
                # may now be completed with either value without falsifying it.
                return values
            mark = len(trail)
            for variable, sign in polarity.items():
                if sign:
                    _proof_assign(sign * variable, values, trail)
            if len(trail) != mark:
                continue
            branch = max(frequency, key=frequency.get)
            decisions.append((len(trail), -branch))
            _proof_assign(branch, values, trail)
        elif decisions:
            mark, sibling = decisions.pop()
            while len(trail) > mark:
                values[trail.pop()] = 0
            _proof_assign(sibling, values, trail)
        else:
            return None


class _ProofCircuit:
    def __init__(self, budget: _ProofBudget):
        self.budget = budget
        self.nvars = 1
        self.clauses: list[tuple[int, ...]] = []
        self.gates: dict[tuple[str, int, int], int] = {}

    def fresh(self) -> int:
        if self.nvars >= _PROOF_MAX_VARIABLES:
            raise _ProofLimit
        self.budget.spend()
        self.nvars += 1
        return self.nvars

    def clause(self, *literals: int) -> None:
        # Gate clauses contain distinct nonconstant literals. Root assertions
        # can additionally be constants, including a false (empty) clause.
        if _PROOF_TRUE in literals:
            return
        if len(self.clauses) >= _PROOF_MAX_CLAUSES:
            raise _ProofLimit
        self.clauses.append(tuple(lit for lit in literals if lit != _PROOF_FALSE))

    def land(self, a: int, b: int) -> int:
        self.budget.spend()
        if a == _PROOF_FALSE or b == _PROOF_FALSE or a == -b:
            return _PROOF_FALSE
        if a == _PROOF_TRUE or a == b:
            return b
        if b == _PROOF_TRUE:
            return a
        a, b = sorted((a, b))
        key = ("and", a, b)
        r = self.gates.get(key)
        if r is None:
            r = self.gates[key] = self.fresh()
            self.clause(-a, -b, r)
            self.clause(a, -r)
            self.clause(b, -r)
        return r

    def lor(self, a: int, b: int) -> int:
        return -self.land(-a, -b)

    def lxor(self, a: int, b: int) -> int:
        self.budget.spend()
        # Pull input negation out of XOR, both for constant folding and
        # to share gates between (a XOR b) and (a XOR NOT b).
        sign = -1 if (a < 0) != (b < 0) else 1
        a, b = sorted((abs(a), abs(b)))
        if a == b:
            return sign * _PROOF_FALSE
        if a == _PROOF_TRUE:
            return sign * -b
        key = ("xor", a, b)
        r = self.gates.get(key)
        if r is None:
            r = self.gates[key] = self.fresh()
            self.clause(-a, -b, -r)
            self.clause(a, b, -r)
            self.clause(-a, b, r)
            self.clause(a, -b, r)
        return sign * r

    def mux(self, select: int, yes: int, no: int) -> int:
        self.budget.spend()
        if select == _PROOF_TRUE or yes == no:
            return yes
        if select == _PROOF_FALSE:
            return no
        return self.lor(self.land(select, yes), self.land(-select, no))

    def equal(self, a: _ProofValue, b: _ProofValue) -> int:
        if isinstance(a, int):
            return -self.lxor(a, b)
        result = _PROOF_TRUE
        for x, y in zip(a, b):
            result = self.land(result, -self.lxor(x, y))
        return result

    def add(self, a, b, carry=_PROOF_FALSE):
        result = []
        for i, (x, y) in enumerate(zip(a, b)):
            propagate = self.lxor(x, y)
            result.append(self.lxor(propagate, carry))
            if i + 1 < len(a):
                carry = self.lor(self.land(x, y), self.land(propagate, carry))
        return tuple(result)  # Discard the carry beyond the fixed width.

    def mul(self, a, b):
        # Shift and add only the partial products still within the result.
        # Canonicalize order for commutative sharing; prefer constant rows.
        def row_key(bits):
            return sum(abs(bit) != _PROOF_TRUE for bit in bits), bits

        if row_key(a) < row_key(b):
            a, b = b, a
        result = (_PROOF_FALSE,) * len(a)
        for shift, bit in enumerate(b):
            self.budget.spend()
            if bit != _PROOF_FALSE:
                row = (_PROOF_FALSE,) * shift + tuple(
                    self.land(bit, a[i]) for i in range(len(a) - shift)
                )
                result = self.add(result, row)
        return result

    def ult(self, a, b):
        # Each higher bit overrides the lower-bit comparison if it differs.
        less = _PROOF_FALSE
        for x, y in zip(a, b):
            less = self.mux(self.lxor(x, y), y, less)
        return less

    def shift(self, op, value, amount):
        width = len(value)
        fill = value[-1] if op == "bvashr" else _PROOF_FALSE
        for i, enable in enumerate(amount):
            self.budget.spend()
            if enable == _PROOF_FALSE:
                continue
            distance = 1 << i
            if distance >= width:
                shifted = (fill,) * width
            elif op == "bvshl":
                shifted = (fill,) * distance + value[:-distance]
            else:
                shifted = value[distance:] + (fill,) * distance
            value = tuple(self.mux(enable, v, w) for v, w in zip(shifted, value))
        return value


_PROOF_TOKEN = _proof_re.compile(r"[ \t\r\n]+|;[^\r\n]*|[()]|[^() \t\r\n;]+")
_PROOF_SIMPLE = _proof_re.compile(r"[A-Za-z~!@$%^&*_\-+=<>.?/][A-Za-z0-9~!@$%^&*_\-+=<>.?/]*")
_PROOF_NUMERAL = _proof_re.compile(r"0|[1-9][0-9]*")
_PROOF_BINARY = _proof_re.compile(r"#b[01]+")
_PROOF_HEX = _proof_re.compile(r"#x[0-9a-fA-F]+")
_PROOF_BOOL_FOLDS = frozenset(("and", "or", "xor", "=>", "=", "distinct"))
_PROOF_BV_UNARY = frozenset(("bvnot", "bvneg"))
_PROOF_BV_FOLDS = frozenset(("bvadd", "bvmul", "bvand", "bvor", "bvxor"))
_PROOF_BV_BINARY = frozenset((
    "bvsub", "bvnand", "bvnor", "bvxnor", "bvcomp", "concat",
    "bvult", "bvule", "bvugt", "bvuge", "bvslt", "bvsle", "bvsgt", "bvsge",
    "bvshl", "bvlshr", "bvashr",
))
_PROOF_RESERVED = (
    _PROOF_BOOL_FOLDS | _PROOF_BV_UNARY | _PROOF_BV_FOLDS | _PROOF_BV_BINARY | frozenset((
        "true", "false", "not", "ite", "Bool", "BitVec",
        "let", "forall", "exists", "match", "par", "as", "_", "!",
        "BINARY", "DECIMAL", "HEXADECIMAL", "NUMERAL", "STRING",
        "bvudiv", "bvurem", "bvsdiv", "bvsrem", "bvsmod",
        "extract", "zero_extend", "sign_extend", "repeat", "rotate_left", "rotate_right",
    ))
)


def _proof_forms(text):
    """Yield complete top-level command forms with bounded tokens and depth."""
    if len(text) > _PROOF_MAX_INPUT:
        raise _ProofLimit
    stack = []
    count = 0
    for match in _PROOF_TOKEN.finditer(text):
        token = match[0]
        if token[0] in " \t\r\n;":
            continue
        count += 1
        if count > _PROOF_MAX_TOKENS:
            raise _ProofLimit
        if token == "(":
            if len(stack) >= _PROOF_MAX_NESTING:
                raise _ProofLimit
            stack.append([])
        elif not stack:
            raise ValueError(f"expected an SMT-LIB command, got {token!r}")
        elif token == ")":
            form = stack.pop()
            if stack:
                stack[-1].append(form)
            else:
                yield form
        else:
            stack[-1].append(token)
    if stack:
        raise ValueError("unclosed parenthesis in SMT-LIB input")
    if count == 0:
        raise ValueError("expected an SMT-LIB command")


def _proof_name(token):
    if not isinstance(token, str) or not _PROOF_SIMPLE.fullmatch(token):
        raise ValueError(f"expected an unquoted SMT-LIB identifier, got {token!r}")
    if token in _PROOF_RESERVED:
        raise ValueError(f"reserved SMT-LIB identifier: {token!r}")
    return token


def _proof_natural(token, label):
    if not isinstance(token, str) or not _PROOF_NUMERAL.fullmatch(token):
        raise ValueError(f"{label} must be a decimal numeral")
    if len(token) > 20:
        raise _ProofLimit
    return int(token)


def _proof_bits(value, width):
    return tuple(_PROOF_TRUE if value & (1 << i) else _PROOF_FALSE for i in range(width))


def _proof_sort(value):
    return None if isinstance(value, int) else len(value)


def _proof_as_bool(value, op):
    if not isinstance(value, int):
        raise ValueError(f"{op} requires Bool")
    return value


def _proof_as_bv(value, op):
    if not isinstance(value, tuple):
        raise ValueError(f"{op} requires a bit-vector")
    return value


def _proof_arity(op, args, count):
    if len(args) != count:
        raise ValueError(f"{op} expects {count} argument(s), got {len(args)}")


class _ProofQuery:
    def __init__(self, budget: _ProofBudget):
        self.circuit = _ProofCircuit(budget)
        self.logic = None
        self.variables: dict[str, _ProofValue] = {}
        self.bindings: dict[str, _ProofValue] = {}

    def width(self, n):
        if n <= 0:
            raise ValueError("bit-vector width must be positive")
        if self.logic == "QF_UF":
            raise ValueError("bit-vectors are not supported under QF_UF")
        if n > _PROOF_MAX_WIDTH:
            raise _ProofLimit
        return n

    def sort(self, node):
        if node == "Bool":
            return None
        if isinstance(node, list) and len(node) == 3 and node[:2] == ["_", "BitVec"]:
            return self.width(_proof_natural(node[2], "bit-vector width"))
        raise ValueError(f"unsupported SMT-LIB sort: {node!r}")

    def atom(self, token, scopes):
        if token == "true":
            return _PROOF_TRUE
        if token == "false":
            return _PROOF_FALSE
        if token.startswith(("#b", "#x")):
            if _PROOF_BINARY.fullmatch(token):
                width = self.width(len(token) - 2)
                return _proof_bits(int(token[2:], 2), width)
            if _PROOF_HEX.fullmatch(token):
                width = self.width(4 * (len(token) - 2))
                return _proof_bits(int(token[2:], 16), width)
            raise ValueError(f"malformed bit-vector literal: {token!r}")
        for scope in scopes:
            if token in scope:
                return scope[token]
        raise ValueError(f"undeclared or unsupported SMT-LIB symbol: {token!r}")

    def let(self, args, scopes):
        _proof_arity("let", args, 2)
        pairs, body = args
        if not isinstance(pairs, list) or not pairs:
            raise ValueError("let requires a nonempty binding list")
        scope = {}
        for pair in pairs:
            if not isinstance(pair, list) or len(pair) != 2:
                raise ValueError("a let binding must be (name term)")
            name = _proof_name(pair[0])
            if name in scope:
                raise ValueError(f"duplicate let binding: {name!r}")
            # Bindings are simultaneous, so each RHS uses the outer scopes.
            scope[name] = self.term(pair[1], scopes)
        return self.term(body, (scope, *scopes))

    def indexed(self, head, args, scopes):
        if len(head) < 2 or head[0] != "_":
            raise ValueError(f"unsupported qualified SMT-LIB operator: {head!r}")
        op = head[1]
        if op not in ("extract", "zero_extend", "sign_extend"):
            raise ValueError(f"unsupported indexed SMT-LIB operator: {op!r}")
        _proof_arity(op, head[2:], 2 if op == "extract" else 1)
        _proof_arity(op, args, 1)
        bits = _proof_as_bv(self.term(args[0], scopes), op)
        index = _proof_natural(head[2], f"{op} index")
        if op == "extract":
            lo = _proof_natural(head[3], "extract index")
            if not lo <= index < len(bits):
                raise ValueError("extract requires 0 <= lo <= hi < operand width")
            return bits[lo:index + 1]
        self.width(len(bits) + index)
        fill = bits[-1] if op == "sign_extend" else _PROOF_FALSE
        return bits + (fill,) * index

    def term(self, node, scopes):
        circuit = self.circuit
        circuit.budget.spend()
        if isinstance(node, str):
            return self.atom(node, scopes)
        if not node:
            raise ValueError("empty SMT-LIB term")
        op, *args = node
        if isinstance(op, list):
            return self.indexed(op, args, scopes)
        if op == "_":
            if len(args) != 2 or not isinstance(args[0], str) or not args[0].startswith("bv"):
                raise ValueError(f"unsupported indexed SMT-LIB constant: {node!r}")
            width = self.width(_proof_natural(args[1], "bit-vector width"))
            value = _proof_natural(args[0][2:], "bit-vector value")
            if value.bit_length() > width:
                raise ValueError("decimal bit-vector constant does not fit its width")
            return _proof_bits(value, width)
        if op == "let":
            return self.let(args, scopes)
        if op in _PROOF_BOOL_FOLDS or op in _PROOF_BV_FOLDS:
            if len(args) < 2:
                raise ValueError(f"{op} expects at least two arguments")
        elif op == "not" or op in _PROOF_BV_UNARY:
            _proof_arity(op, args, 1)
        elif op in _PROOF_BV_BINARY:
            _proof_arity(op, args, 2)
        elif op == "ite":
            _proof_arity(op, args, 3)
        else:
            raise ValueError(f"unsupported SMT-LIB operator: {op!r}")
        values = [self.term(arg, scopes) for arg in args]
        if op in ("=", "distinct"):
            sort = _proof_sort(values[0])
            if any(_proof_sort(v) != sort for v in values[1:]):
                raise ValueError(f"{op} requires arguments of the same sort and width")
            result = _PROOF_TRUE
            if op == "=":  # n-ary equality is chainable
                for a, b in zip(values, values[1:]):
                    result = circuit.land(result, circuit.equal(a, b))
            else:
                if len(values) > (2 if sort is None else 1 << sort):
                    return _PROOF_FALSE
                for i, a in enumerate(values):
                    for j in range(i):
                        result = circuit.land(result, -circuit.equal(a, values[j]))
                        if result == _PROOF_FALSE:
                            return result
            return result
        if op == "ite":
            select, yes, no = values
            select = _proof_as_bool(select, op)
            if _proof_sort(yes) != _proof_sort(no):
                raise ValueError("ite branches must have the same sort and width")
            if isinstance(yes, int):
                return circuit.mux(select, yes, no)
            return tuple(circuit.mux(select, a, b) for a, b in zip(yes, no))
        if op == "not":
            return -_proof_as_bool(values[0], op)
        if op in _PROOF_BOOL_FOLDS:
            bits = [_proof_as_bool(v, op) for v in values]
            if op == "=>":  # Implication associates to the right.
                result = bits[-1]
                for bit in reversed(bits[:-1]):
                    result = circuit.lor(-bit, result)
                return result
            gate = {"and": circuit.land, "or": circuit.lor, "xor": circuit.lxor}[op]
            result = bits[0]
            for bit in bits[1:]:
                result = gate(result, bit)
            return result
        bits = [_proof_as_bv(v, op) for v in values]
        if op in _PROOF_BV_FOLDS:
            result = bits[0]
            for word in bits[1:]:
                result = self.bv_term(op, (result, word))
            return result
        return self.bv_term(op, bits)

    def bv_term(self, op, values):
        circuit = self.circuit
        a = values[0]
        if op == "bvnot":
            return tuple(-bit for bit in a)
        if op == "bvneg":
            return circuit.add((_PROOF_FALSE,) * len(a), tuple(-bit for bit in a), _PROOF_TRUE)
        b = values[1]
        if op == "concat":
            self.width(len(a) + len(b))
            return b + a
        if len(a) != len(b):
            raise ValueError(f"{op} requires equal bit-vector widths")
        if op == "bvadd":
            return circuit.add(a, b)
        if op == "bvsub":
            return circuit.add(a, tuple(-bit for bit in b), _PROOF_TRUE)
        if op == "bvmul":
            return circuit.mul(a, b)
        if op == "bvcomp":
            return (circuit.equal(a, b),)
        if op in ("bvshl", "bvlshr", "bvashr"):
            return circuit.shift(op, a, b)
        if op[3:] in ("lt", "le", "gt", "ge"):
            if op[2] == "s":  # Bias both signed operands by flipping the sign bit.
                a, b = a[:-1] + (-a[-1],), b[:-1] + (-b[-1],)
            if op[3:] in ("le", "gt"):
                a, b = b, a
            less = circuit.ult(a, b)
            return -less if op.endswith("e") else less
        gates = {
            "bvand": circuit.land, "bvnand": circuit.land,
            "bvor": circuit.lor, "bvnor": circuit.lor,
            "bvxor": circuit.lxor, "bvxnor": circuit.lxor,
        }
        gate = gates[op]
        sign = -1 if op in ("bvnand", "bvnor", "bvxnor") else 1
        return tuple(sign * gate(x, y) for x, y in zip(a, b))

    def commands(self, forms):
        tail = None
        allow_logic = True
        scopes = (self.bindings,)
        for form in forms:
            if not form or not isinstance(form[0], str):
                raise ValueError("expected a named SMT-LIB command")
            op, *args = form
            if tail == "exit" or (tail and op not in ("get-model", "exit")):
                raise ValueError(f"{op} cannot follow {tail} in a single query")
            if op == "set-logic":
                _proof_arity(op, args, 1)
                if not allow_logic:
                    raise ValueError("set-logic can only appear once, before declarations and assertions")
                if args[0] not in ("QF_BV", "QF_UF"):
                    raise ValueError(f"unsupported SMT-LIB logic: {args[0]!r}")
                self.logic = args[0]
            elif op == "set-option":
                if args not in ([":produce-models", "true"], [":produce-models", "false"]):
                    raise ValueError(f"unsupported SMT-LIB option: {args!r}")
            elif op == "assert":
                _proof_arity(op, args, 1)
                self.circuit.clause(_proof_as_bool(self.term(args[0], scopes), op))
            elif op in ("declare-const", "declare-fun", "define-fun"):
                _proof_arity(op, args, {"declare-const": 2, "declare-fun": 3, "define-fun": 4}[op])
                name = _proof_name(args[0])
                if name in self.bindings:
                    raise ValueError(f"duplicate SMT-LIB declaration: {name!r}")
                if op != "declare-const" and args[1] != []:
                    raise ValueError(f"{op} supports only nullary functions")
                sort = self.sort(args[1] if op == "declare-const" else args[2])
                if op == "define-fun":
                    value = self.term(args[3], scopes)
                    if _proof_sort(value) != sort:
                        raise ValueError(f"definition of {name!r} has the wrong sort or width")
                else:
                    fresh = self.circuit.fresh
                    value = fresh() if sort is None else tuple(fresh() for _ in range(sort))
                    self.variables[name] = value
                self.bindings[name] = value
            elif op in ("check-sat", "get-model", "exit"):
                _proof_arity(op, args, 0)
                if op == "get-model" and tail != "check-sat":
                    raise ValueError("get-model requires one preceding check-sat")
                tail = op
            else:
                raise ValueError(f"unsupported SMT-LIB command: {op!r}")
            if op != "set-option":
                allow_logic = False

    def model(self, values):
        # Declarations are positive literals; fill only genuine don't-cares.
        return {
            name: (
                values[value] > 0 if isinstance(value, int)
                else sum(1 << i for i, bit in enumerate(value) if values[bit] > 0)
            )
            for name, value in self.variables.items()
        }


def solve(text: str, *, max_steps: int = _PROOF_DEFAULT_MAX_STEPS) -> tuple[str, dict[str, bool | int]]:
    """Solve one SMT-LIB2 query (QF_BV or the Boolean subset of QF_UF).

    Supports simple symbols, declarations, nullary definitions, simultaneous
    ``let``, Boolean connectives, and bit-vectors up to 64 bits. Bit-vector
    operations include equality, comparisons, modular arithmetic, bitwise
    operations, shifts, extraction, concatenation, and sign/zero extension;
    division and remainder are not supported. Decimal bit-vector constants
    must fit their widths. ``check-sat`` is optional; ``get-model`` is a
    presentation command, not a second query.

    Malformed/unsupported input raises ``ValueError``. Fixed input/circuit
    bounds and the deterministic circuit/search work budget return ``unknown``
    with no partial model. Only a satisfying assignment produces a model.
    """
    if not isinstance(text, str):
        raise ValueError("expected SMT-LIB input as text")
    if type(max_steps) is not int or max_steps < 0:
        raise ValueError("max_steps must be a nonnegative integer")
    budget = _ProofBudget(max_steps)
    query = _ProofQuery(budget)
    try:
        query.commands(_proof_forms(text))
        values = _proof_dpll(query.circuit.clauses, query.circuit.nvars, budget)
    except _ProofLimit:
        return "unknown", {}
    if values is None:
        return "unsat", {}
    return "sat", query.model(values)
