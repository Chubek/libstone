"""Differential execution against the actual scripts/acsem reference subset."""
import argparse
import pathlib
import random
import shlex
import subprocess


def invoke(command, data=b""):
    return subprocess.run([str(part) for part in command], input=data,
                          capture_output=True, timeout=45)


def checked(command):
    result = invoke(command)
    if result.returncode:
        raise AssertionError((command, result.returncode, result.stdout, result.stderr))


def compile_spec(args, spec, stem):
    extensions = {"c": "c", "python": "py", "rust": "rs", "pascal": "pas"}
    output = args.work / f"{stem}.{extensions[args.target]}"
    checked([args.cli, "--dialect", args.dialect, "--target", args.target,
             spec, "-o", output])
    if args.target == "python":
        checked([args.compiler, "-m", "py_compile", output])
        return [args.compiler, output]
    binary = args.work / stem
    if args.target == "c":
        checked([args.compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                 "-pedantic", *shlex.split(args.flags), output, "-o", binary])
    elif args.target == "rust":
        checked([args.compiler, "--edition=2021", "-Dwarnings", output, "-o", binary])
    else:
        checked([args.compiler, "-Mobjfpc", "-Cr", "-Co", "-Ci", "-Sa",
                 f"-o{binary}", f"-FU{args.work}", output])
    return [binary]


def compare(args, spec, command, term, expected=None):
    data = term.encode()
    reference = invoke([args.perl, args.reference, "--run", spec], data)
    actual = invoke(command, data)
    observation = (reference.returncode, reference.stdout)
    assert reference.returncode in (0, 1, 2), (term, reference.stderr)
    if expected is not None:
        assert observation == expected, (term, observation, expected, reference.stderr)
    assert (actual.returncode, actual.stdout) == observation, (
        args.target, term, observation, (actual.returncode, actual.stdout),
        reference.stderr, actual.stderr)
    if reference.returncode == 2:
        assert reference.stderr and actual.stderr and not actual.stdout
    else:
        assert not reference.stderr and not actual.stderr


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cli", required=True)
    parser.add_argument("--reference", required=True, type=pathlib.Path)
    parser.add_argument("--work", required=True, type=pathlib.Path)
    parser.add_argument("--target", required=True, choices=("c", "python", "rust", "pascal"))
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--perl", default="perl")
    parser.add_argument("--flags", default="")
    parser.add_argument("--dialect", choices=("1", "2"), default="1")
    args = parser.parse_args()
    args.work = args.work.resolve()
    args.reference = args.reference.resolve()
    args.work.mkdir(parents=True, exist_ok=True)
    examples = args.reference.parent / "examples"
    count = 0
    for stem, cases in (
        ("calculator", ("Num(42)", "Add(Num(20),Num(22))")),
        ("recovery", ("Failure()", "Escape()")),
    ):
        spec = examples / f"{stem}.acsem"
        command = compile_spec(args, spec, stem)
        for term in cases:
            compare(args, spec, command, term)
            count += 1

    # These equations exercise every script primitive and combinator, especially
    # the less obvious separation of transient flow and binding flow.
    rules = {
        "Literal": 'give "literal"',
        "Comparison": "(give -3 < 0) and then (give 2 == 2) and then (give 2 == 3)",
        "Precedence": "give 2 + 3 * 4 - 8 / 2",
        "Grouping": "give (2 + 3) * 4",
        "Collision": 'bind "x" to 1 and then bind "x" to 2',
        "SameKey": 'bind "x" to 1 and then bind "x" to 1',
        "Scope": 'bind "x" to 3 hence give bound("x")',
        "NotInherited": 'bind "x" to 3 then give bound("x")',
        "ReplaceEnv": 'bind "x" to 1 hence (bind "y" to 2 hence give bound("x"))',
        "Overlay": ('bind "x" to 1 hence (furthermore bind "y" to 2 hence '
                    '(give bound("x") and then give bound("y")))'),
        "ThenInputs": "give 7 then (give given(1) and then give given(1) + 1)",
        "AndInputs": "give 3 then ((give 9 then give given(1)) and then give given(1))",
        "HenceInputs": ('give 7 then ((give 3 and then bind "x" to 5) hence '
                        '(give given(1) and then give bound("x")))'),
        "MissingGiven": "give given(64)",
        "BadBound": "give 1 then give bound(given(1))",
        "BadContents": "give 1 then give contents(given(1))",
        "BadStore": "give 1 then store 7 in given(1)",
        "BadBind": "give 1 then bind given(1) to 2",
        "DirectFail": "fail",
        "FailureStore": ('(allocate cell then bind "c" to given(1)) hence '
                         '((store 7 in bound("c") then fail) otherwise give contents(bound("c")))'),
        "EscapeStore": ('(allocate cell then bind "c" to given(1)) hence '
                        '((store 9 in bound("c") then escape 5) trapping give contents(bound("c")))'),
    }
    source = (examples / "expressions.acsem").read_text()
    constructors = " | ".join(f"{name}()" for name in rules)
    source = source.replace("| Skip();", f"| Skip() | {constructors};")
    source = source.rstrip().removesuffix("}")
    source += "\n".join(f"rule {name} = {action};" for name, action in rules.items()) + "\n}"
    spec = args.work / "parity.acsem"
    spec.write_text(source)
    command = compile_spec(args, spec, "parity")
    cases = [
        "Num(-2147483648)", "Num(2147483647)", "Num(-00000000)", "Token(x)",
        "Add(Num(2147483647),Num(1))", "Sub(Num(-2147483648),Num(1))",
        "Mul(Num(2147483647),Num(2147483647))", "Div(Num(-2147483648),Num(-1))",
        "Div(Num(1),Num(0))", "Div(Num(-7),Num(2))", "Div(Num(7),Num(-2))",
        "Div(Num(-7),Num(-2))", "Let(x,Token(y),Var(x))", "Var(missing)", "Given()",
        "Let(x,Num(1),Let(y,Num(2),Add(Var(x),Var(y))))",
        "Let(x,Num(4),Let(x,Num(5),Var(x)))",
        "Let(x,New(),Seq(Set(Var(x),Num(40)),Add(Get(Var(x)),Num(2))))",
        "Catch(Raise(Num(7)),Given())", "Catch(Num(1),Num(9))",
        "Catch(DirectFail(),Num(9))", "Fallback(Require(Num(0)),Num(9))",
        "Fallback(Raise(Num(5)),Num(9))", "Catch(Raise(Token(problem)),Given())",
        "Pair(Num(1),Raise(Num(2)))", "Pair(Num(1),DirectFail())",
        "Fallback(Num(1),New())", "Pair(Num(1),Num(2))", "Seq(Num(1),Num(2))",
        "Get(New())", "Pair(New(),New())", "Set(New(),Num(4))",
        "Set(New(),Token(x))", "Require(Token(x))", "Require(Num(-1))", "Skip()",
        "Get(Num(0))", "Add(New(),Num(1))", "Seq(Num(7),Given())",
    ] + [f"{name}()" for name in rules]
    for term in cases:
        compare(args, spec, command, term)
        count += 1

    rng = random.Random(20261121)

    def arithmetic(depth):
        if depth == 0 or rng.randrange(4) == 0:
            value = rng.choice((-2147483648, 2147483647, -1, 0, 1, rng.randrange(-100, 101)))
            return f"Num({value})"
        return f'{rng.choice(("Add", "Sub", "Mul", "Div"))}({arithmetic(depth-1)},{arithmetic(depth-1)})'

    for _ in range(60):
        compare(args, spec, command, arithmetic(4))
        count += 1
    for term in ("", "Unknown()", "Num()", "Num(1,2)", "Add(Num(1))",
                 "Num(2147483648)", "Num(-2147483649)", "Token(1)", "Num(+1)",
                 'Token("x")', "Skip() trailing", "Skip()\0", "Skip() // comment",
                 " " * 65537):
        compare(args, spec, command, term, (2, b""))
        count += 1
    print(f"{args.target} dialect {args.dialect}: {count} script-reference observations matched")


if __name__ == "__main__":
    main()
