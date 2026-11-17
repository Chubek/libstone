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
args = p.parse_args()
work = pathlib.Path(args.work)
work.mkdir(parents=True, exist_ok=True)


def run(command, input=None, expected=0):
    r = subprocess.run([str(x) for x in command], input=input, capture_output=True, timeout=45)
    if r.returncode != expected:
        raise AssertionError(f'{command}: exit {r.returncode}, expected {expected}\n{r.stdout.decode(errors="replace")}\n{r.stderr.decode(errors="replace")}')
    return r.stdout.decode()


def compile_spec(spec, stem):
    ext = {'c': 'c', 'python': 'py', 'rust': 'rs', 'pascal': 'pas', 'latex': 'tex'}[args.target]
    output = work / f'{stem}.{ext}'
    run([args.cli, '--target', args.target, spec, '-o', output])
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
syntax E = Empty() | New() | Twice(x: E) | B(k: identifier) | Both(x: E, y: E) | Try(x: E);
rule Empty = complete;
rule New = allocate cell;
rule Twice = evaluate(x) then evaluate(x);
rule B = bind k to 1;
rule Both = evaluate(x) and then evaluate(y);
rule Try = evaluate(x) otherwise complete;
}''')
resource_command = compile_spec(resources, 'resources')
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
print(f'{args.target}: {len(cases)} semantic/oracle cases, malformed input, resource limits, micro and minimal languages passed')
