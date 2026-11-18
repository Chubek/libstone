"""Compile generated artifacts and test semantic observations, not source snapshots."""
import argparse
import pathlib
import random
import shlex
import subprocess
import sys

p = argparse.ArgumentParser()
p.add_argument('--cli', required=True)
p.add_argument('--source', required=True)
p.add_argument('--work', required=True)
p.add_argument('--target', required=True)
p.add_argument('--compiler', required=True)
p.add_argument('--flags', default='')
p.add_argument('--dialect', default='')
p.add_argument('--define', action='append', default=[])
args = p.parse_args()
work = pathlib.Path(args.work)
work.mkdir(parents=True, exist_ok=True)


def run(command, input=None, expected=0):
    r = subprocess.run([str(x) for x in command], input=input, capture_output=True, timeout=45)
    if r.returncode != expected:
        raise AssertionError(f'{command}: exit {r.returncode}, expected {expected}, input={input!r}\n'
                             f'{r.stdout.decode(errors="replace")}\n{r.stderr.decode(errors="replace")}')
    return r.stdout.decode()


def compile_spec(spec, stem, defines=None):
    ext = {'c': 'c', 'python': 'py', 'rust': 'rs', 'pascal': 'pas', 'latex': 'tex'}[args.target]
    output = work / f'{stem}.{ext}'
    command = [args.cli]
    if args.dialect:
        command += ['--dialect', args.dialect]
    for definition in (defines if defines is not None else args.define):
        command += ['--define', definition]
    run(command + ['--target', args.target, spec, '-o', output])
    if args.target == 'python':
        run([args.compiler, '-m', 'py_compile', output])
        return [args.compiler, output]
    if args.target == 'latex':
        run([args.compiler, '-interaction=nonstopmode', '-halt-on-error', '-no-shell-escape', '-output-directory', work, output])
        assert (work / f'{stem}.pdf').stat().st_size > 0
        return []
    binary = work / stem
    if args.target == 'c':
        run([args.compiler, '-std=c11', '-Wall', '-Wextra', '-Werror', '-pedantic', *shlex.split(args.flags), output, '-o', binary])
    elif args.target == 'rust':
        run([args.compiler, '--edition=2021', '-Dwarnings', output, '-o', binary])
    else:
        run([args.compiler, '-Mobjfpc', '-Cr', '-Co', '-Ci', '-Sa', f'-o{binary}', f'-FU{work}', output])
    return [binary]


command = compile_spec(args.source, 'expressions')
if args.target == 'latex':
    escaped = work / 'escaped.acsem'
    escaped.write_text('language A_b { start E; syntax E = N_x(a_b: integer); rule N_x = give a_b < 42; }')
    compile_spec(escaped, 'escaped')
    if args.dialect == '2':
        for stem in ('functions', 'closures', 'continuations', 'generators', 'imperative_language',
                     'pattern_matching'):
            compile_spec(pathlib.Path(args.source).parent / f'{stem}.acsem', stem)
    sys.exit(0)

cases = [
    ('Num(42)', 'completed 42'),
    ('Add(Num(20), Num(22))', 'completed 42'),
    ('Num(-2147483648)', 'completed -2147483648'),
    ('Num(00000000000000000000000000000000000000000000001)', 'completed 1'),
    ('Add(Num(2147483647), Num(1))', 'failed'),
    ('Sub(Num(-2147483648), Num(1))', 'failed'),
    ('Mul(Num(-2147483648), Num(-2147483648))', 'failed'),
    ('Div(Num(-2147483648), Num(-1))', 'failed'),
    ('Div(Num(1), Num(0))', 'failed'),
    ('Div(Num(-7), Num(2))', 'completed -3'),
    ('Div(Num(7), Num(-2))', 'completed -3'),
    ('Var(x)', 'failed'),
    ('Given()', 'failed'),
    ('Skip()', 'completed'),
    ('Require(Num(0))', 'failed'),
    ('Require(Num(-1))', 'completed'),
    ('Let(x, Num(40), Add(Var(x), Num(2)))', 'completed 42'),
    ('Let(x, Num(40), Let(y, Num(2), Add(Var(x), Var(y))))', 'completed 42'),
    ('Let(x, Num(1), Pair(Let(x, Num(2), Var(x)), Var(x)))', 'completed 2 1'),
    ('Pair(Let(x, Num(1), Var(x)), Var(x))', 'failed'),
    ('Seq(Num(7), Given())', 'completed 7'),
    ('Seq(Num(7), Pair(Num(2), Given()))', 'completed 2 7'),
    ('Pair(Num(7), Given())', 'failed'),
    ('Raise(Num(8))', 'escaped 8'),
    ('Catch(Raise(Num(8)), Add(Given(), Num(1)))', 'completed 9'),
    ('Catch(Require(Num(0)), Num(9))', 'failed'),
    ('Catch(Num(8), Num(9))', 'completed 8'),
    ('Fallback(Require(Num(0)), Num(9))', 'completed 9'),
    ('Fallback(Raise(Num(8)), Num(9))', 'escaped 8'),
    ('Fallback(Num(8), Num(9))', 'completed 8'),
    ('New()', 'completed cell:0'),
    ('Pair(New(), New())', 'completed cell:0 cell:1'),
    ('Get(New())', 'failed'),
    ('Get(Num(0))', 'failed'),
    ('Add(New(), Num(1))', 'failed'),
    ('Set(New(), New())', 'failed'),
    ('Let(x, New(), Seq(Set(Var(x), Num(40)), Add(Get(Var(x)), Num(2))))', 'completed 42'),
    ('Let(x, New(), Fallback(Seq(Set(Var(x), Num(7)), Require(Num(0))), Get(Var(x))))', 'completed 7'),
    ('Let(x, New(), Seq(Catch(Seq(Set(Var(x), Num(7)), Raise(Num(4))), Skip()), Get(Var(x))))', 'completed 7'),
]
# Independent arithmetic oracle with bounds; deterministic heterogeneous ASTs.
rng = random.Random(728)


def tree(depth):
    if depth == 0 or rng.randrange(4) == 0:
        n = rng.choice([-2147483648, 2147483647, 0, 1, -1, rng.randrange(-100, 101)])
        return f'Num({n})', n
    a, x = tree(depth - 1)
    b, y = tree(depth - 1)
    op = rng.choice(['Add', 'Sub', 'Mul', 'Div'])
    z = None
    if x is not None and y is not None:
        if op == 'Add': z = x + y
        elif op == 'Sub': z = x - y
        elif op == 'Mul': z = x * y
        elif y: z = (abs(x) // abs(y)) * (-1 if (x < 0) != (y < 0) else 1)
        if z is not None and not -2147483648 <= z <= 2147483647: z = None
    return f'{op}({a},{b})', z


for _ in range(80):
    text, value = tree(4)
    cases.append((text, 'failed' if value is None else f'completed {value}'))
for text, expected in cases:
    code = 0 if expected.startswith('completed') else 1
    actual = run(command, (text + '\n').encode(), code).strip()
    assert actual == expected, (args.target, text, actual, expected)
for text in ['', 'Unknown()', 'Num()', 'Num(1,2)', 'Num(+1)', 'Num(2147483648)',
             'Num(-2147483649)', 'Num(999999999999999999999999999999999)',
             'Num(1)junk', 'Var(1)', 'Num(x)', 'Num(1)\0', 'Num(1)é', ' ' * 65537,
             'Seq(Skip(),' * 140 + 'Skip()' + ')' * 140]:
    run(command, text.encode(), 2)
# Transient/resource limits cannot be caught as ordinary failure.
wide = 'Num(0)'
for _ in range(6):
    wide = f'Pair({wide},{wide})'
assert run(command, wide.encode()).startswith('completed')
run(command, f'Fallback(Pair({wide},Num(1)),Num(9))'.encode(), 2)

# Separate syntax sorts, all primitives, binding conflict and hence flow.
micro = work / 'micro.acsem'
micro.write_text('''language Micro_test {
start E;
syntax E = From(s: S) | Conflict() | Scope() | Flow() | Missing() | Eq() | Lt() | Token() | Bare();
syntax S = Literal(n: integer);
rule From = evaluate(s);
rule Literal = give n;
rule Conflict = bind "x" to 1 and then bind "x" to 2;
rule Scope = bind "x" to 7 hence (bind "y" to 9 hence give bound("x"));
rule Flow = (give 5 and then bind "x" to 7) hence give bound("x");
rule Missing = fail;
rule Eq = give 3 == 3;
rule Lt = give -2 < 1;
rule Token = give "name";
rule Bare = complete;
}''')
micro_command = compile_spec(micro, 'micro')
for text, expected in [('From(Literal(42))', 'completed 42'), ('Conflict()', 'failed'),
                       ('Scope()', 'failed'), ('Flow()', 'completed 5 7'), ('Missing()', 'failed'),
                       ('Eq()', 'completed 1'), ('Lt()', 'completed 1'), ('Token()', 'completed token:name'),
                       ('Bare()', 'completed')]:
    assert run(micro_command, text.encode(), 1 if expected == 'failed' else 0).strip() == expected
run(micro_command, b'Literal(1)', 2)
run(micro_command, b'From(Bare())', 2)
minimal = work / 'minimal.acsem'
minimal.write_text('language Minimal { start E; syntax E = Skip(); rule Skip = complete; }')
assert run(compile_spec(minimal, 'minimal'), b'Skip()').strip() == 'completed'
resources = work / 'resources.acsem'
resources.write_text('''language Resources {
start E;
syntax E = Empty() | New() | Twice(x: E) | B(k: identifier) | Both(x: E, y: E) | Try(x: E) | Pass(x: E);
rule Empty = complete;
rule New = allocate cell;
rule Twice = evaluate(x) then evaluate(x);
rule B = bind k to 1;
rule Both = evaluate(x) and then evaluate(y);
rule Try = evaluate(x) otherwise complete;
rule Pass = evaluate(x);
}''')
resource_command = compile_spec(resources, 'resources')
depth_boundary = ('Pass(' * 128 + 'B(x)' + ')' * 128).encode()
run(resource_command, depth_boundary, 0 if args.dialect == '2' else 2)
assert run(resource_command, ('Pass(' * 127 + 'B(x)' + ')' * 127).encode()).strip() == 'completed'
# The original recursive evaluator counts action entry/yielders, not delivery.
step_boundary = ('Both(' + 'Twice(' * 14 + 'Empty()' + ')' * 14 + ',' +
                 'Twice(' * 13 + 'Empty()' + ')' * 13 + ')').encode()
run(resource_command, step_boundary, 2 if args.dialect == '2' else 0)
assert run(resource_command, ('Twice(' * 10 + 'New()' + ')' * 10).encode()).strip() == 'completed cell:1023'
run(resource_command, ('Try(' + 'Twice(' * 11 + 'New()' + ')' * 11 + ')').encode(), 2)
run(resource_command, ('Try(' + 'Twice(' * 16 + 'Empty()' + ')' * 16 + ')').encode(), 2)


def balanced(xs):
    if len(xs) == 1:
        return xs[0]
    k = len(xs) // 2
    return f'Both({balanced(xs[:k])},{balanced(xs[k:])})'


bindings = balanced([f'B(k{i})' for i in range(64)])
assert run(resource_command, bindings.encode()).strip() == 'completed'
run(resource_command, f'Try(Both({bindings},B(extra)))'.encode(), 2)
# Binding incompatibility is checked before the result's capacity, on every target.
assert run(resource_command, f'Both({bindings},Both(B(extra),B(k0)))'.encode(), 1).strip() == 'failed'
run(resource_command, balanced(['Empty()'] * 1025).encode(), 2)
run(resource_command, balanced([f'B(k{i})' for i in range(257)]).encode(), 2)

# Dialect 2: first-class functions. Closure capture, escaping closures,
# recursion, mutual recursion, tail calls, mutable capture, and returns.
if args.dialect == '2':
    functions = pathlib.Path(args.source).parent / 'functions.acsem'
    function_command = compile_spec(functions, 'functions', defines=['steps=10000000'])
    function_cases = [
        ('Num(42)', 'completed 42'),
        ('Call(Const(7), Num(0))', 'completed 7'),
        ('Call(Call(Adder(), Num(3)), Num(4))', 'completed 7'),
        ('Call(Call(Adder(), Num(10)), Num(-3))', 'completed 7'),
        ('Compose(Const(2), Const(3), Num(42))', 'completed 2'),
        ('Fact(10)', 'completed 3628800'),
        ('Fact(12)', 'completed 479001600'),
        ('Fact(13)', 'failed'),
        ('Even(4)', 'completed 1'),
        ('Even(5)', 'completed 0'),
        ('Let(k, Counter(), Pairx(Call(Var(k), Num(0)), Call(Var(k), Num(0))))', 'completed 1 2'),
        ('Call(Early(), Num(5))', 'completed 5'),
        ('Call(Early(), Num(50))', 'completed 10'),
        ('Call(Num(1), Num(2))', 'failed'),
        ('Loop(10000)', 'completed 10000'),
    ]
    for text, expected in function_cases:
        code = 0 if expected.startswith('completed') else 1
        actual = run(function_command, (text + '\n').encode(), code).strip()
        assert actual == expected, (args.target, text, actual, expected)
    # Tail calls keep frame depth bounded: deep loops finish within DEPTH_MAX.
    assert run(function_command, b'Loop(100000)').strip() == 'completed 100000'
    control = pathlib.Path(args.source).parent / 'continuations.acsem'
    control_command = compile_spec(control, 'continuations', defines=['steps=1000000', 'heap=128'])
    control_cases = [
        ('Immediate()', 'completed 42'), ('Context()', 'completed 42'),
        ('Normal()', 'completed 42'), ('Invalid()', 'failed'),
        ('Shared()', 'completed 42'), ('Replay()', 'completed 42'),
        ('Count(10000)', 'completed 0'), ('Outside()', 'failed'),
        ('NonTail(3)', 'completed 0 99 99 99'),
    ]
    for text, expected in control_cases:
        assert run(control_command, text.encode(), 1 if expected == 'failed' else 0).strip() == expected
    run(control_command, b'Diverge()', 2)
    # Force repeated collection with escaping closures and recursive env cycles.
    gc_command = compile_spec(functions, 'functions_gc', defines=['steps=10000000', 'heap=128'])
    assert run(gc_command, b'Loop(100000)').strip() == 'completed 100000'
    assert run(gc_command, b'Call(Call(Adder(),Num(40)),Num(2))').strip() == 'completed 42'
    gen_command = compile_spec(pathlib.Path(args.source).parent / 'generators.acsem',
                               'generators', defines=['heap=128', 'steps=1000000'])
    generator_cases = [
        ('Once()', 'completed 1 42 0 0'), ('Sequence()', 'completed 1 20 1 22 0 99 0'),
        ('Lazy()', 'completed 42'), ('Closed()', 'completed 0'),
        ('Failing()', 'completed 42 0'), ('Escaping()', 'escaped 42'),
        ('Outside()', 'failed'), ('Nested()', 'completed 1 42'),
        ('Recursive(3)', 'completed 1 3 1 2 1 1 0 99'), ('Reentrant()', 'failed'),
    ]
    for text, expected in generator_cases:
        assert run(gen_command, text.encode(), 0 if expected.startswith('completed') else 1).strip() == expected
    assert run(gen_command, b'Stress(10000)').strip() == 'completed 10000'
    loop_command = compile_spec(pathlib.Path(args.source).parent / 'imperative_language.acsem', 'imperative')
    loop_cases = [
        ('Loop(0)', 'completed 0'), ('Loop(4)', 'completed 4'), ('Loop(10000)', 'completed 5'),
        ('Choice(0)', 'completed 7'), ('Choice(-1)', 'completed 42'), ('Failure()', 'failed'),
        ('Escape()', 'escaped 42'), ('Outside()', 'failed'), ('Crossing()', 'failed'),
    ]
    for text, expected in loop_cases:
        assert run(loop_command, text.encode(), 0 if expected.startswith('completed') else 1).strip() == expected
    run(loop_command, b'Forever()', 2)
    closures_command = compile_spec(pathlib.Path(args.source).parent / 'closures.acsem', 'closures',
                                   defines=['heap=128'])
    closure_cases = [
        ('Apply(Apply(Lambda(x,Lambda(y,Add(Var(x),Var(y)))),Num(40)),Num(2))', 'completed 42'),
        ('Let(x,Num(40),Let(f,Lambda(y,Add(Var(x),Var(y))),Let(x,Num(99),Apply(Var(f),Num(2)))))', 'completed 42'),
        ('Let(c,Ref(Num(0)),Let(f,Lambda(x,Seq(Set(Var(c),Add(Get(Var(c)),Num(1))),Get(Var(c)))),Pair(Apply(Var(f),Num(0)),Apply(Var(f),Num(0)))))', 'completed 1 2'),
        ('Let(f,Ref(Lambda(x,Add(Var(x),Num(2)))),Apply(Get(Var(f)),Num(40)))', 'completed 42'),
        ('Let(r,Ref(Num(0)),Seq(Set(Var(r),Var(r)),Get(Var(r))))', 'completed cell:0'),
    ]
    for text, expected in closure_cases:
        assert run(closures_command, text.encode()).strip() == expected
    cases += function_cases
    cases += control_cases
    cases += generator_cases
    cases += loop_cases
    cases += closure_cases
    patterns = pathlib.Path(args.source).parent / 'pattern_matching.acsem'
    pattern_command = compile_spec(patterns, 'pattern_matching', defines=['heap=128'])
    pattern_cases = [
        ('Empty()', 'completed variant:Nil'),
        ((patterns.parent / 'pattern_matching.term').read_text().strip(), 'completed 42'),
        ('First(Prepend(42,Empty()))', 'completed 42'),
        ('First(Empty())', 'failed'),
        ('Reject(Tagged(a))', 'failed'),
        ('Reject(Prepend(1,Empty()))', 'failed'),
        ('InvalidConstructor(BoxValue(Empty()))', 'failed'),
        ('Inspect(Prepend(42,Empty()),1)', 'completed 2 42'),
        ('Inspect(Prepend(42,Empty()),2)', 'completed 2 variant:Nil'),
        ('Inspect(Empty(),1)', 'failed'),
        ('Inspect(Tagged(a),1)', 'failed'),
        ('Compare(Prepend(1,Empty()),Prepend(1,Empty()))', 'completed 1 0'),
        ('Compare(Prepend(1,Empty()),Prepend(2,Empty()))', 'completed 0 0'),
        ('Compare(Empty(),BoxValue(Empty()))', 'completed 0 0'),
        ('Compare(Tagged(yes),Tagged(yes))', 'completed 1 1'),
        ('Pick(1,yes)', 'completed 42'),
        ('Pick(0,yes)', 'completed token:yes'),
        ('Pick(1,no)', 'completed token:no'),
        ('Partial(BoxValue(Prepend(42,Empty())))', 'completed 42'),
        ('Partial(BoxValue(Prepend(42,Prepend(0,Empty()))))', 'failed'),
        ('Capture(Prepend(42,Empty()))', 'completed 42'),
        ('Scope()', 'completed 7 42'),
        ('FallbackGuard()', 'completed 42'),
        ('Alternatives(Prepend(42,Empty()))', 'completed 42'),
        ('Alternatives(Prepend(99,Prepend(42,Empty())))', 'completed 42'),
        ('Alternatives(Empty())', 'failed'),
        ('Alias(Empty())', 'completed 1'),
        ('Alias(Prepend(42,Empty()))', 'completed 1'),
        ('Unpack(Prepend(42,Empty()))', 'completed 42 variant:Nil'),
        ('Unpack(Empty())', 'completed'),
        ('Unpack(Tagged(a))', 'failed'),
        ('Retain(Grow(45))', 'completed 42'),
        ('Cycle()', 'completed 1'),
        ('Compare(Grow(45),Grow(45))', 'completed 1 0'),
    ]
    for text, expected in pattern_cases:
        assert run(pattern_command, text.encode(), 0 if expected.startswith('completed') else 1).strip() == expected, text
    run(pattern_command, b'Compare(Grow(100),Grow(100))', 2)
    depth_command = compile_spec(patterns, 'pattern_depth', defines=['depth=32'])
    assert run(depth_command, b'Compare(Grow(5),Grow(5))').strip() == 'completed 1 0'
    run(depth_command, b'Compare(Grow(45),Grow(45))', 2)
    cases += pattern_cases
dialect_note = f' (dialect {args.dialect})' if args.dialect else ''
print(f'{args.target}{dialect_note}: {len(cases)} semantic/oracle cases, malformed input, resource limits, micro and minimal languages passed')
