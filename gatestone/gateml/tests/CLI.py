import pathlib
import platform
import subprocess
import sys
import tempfile

cli = sys.argv[1]


def invoke(*args, status=0):
    result = subprocess.run([cli, *args], text=True, capture_output=True, timeout=30)
    assert result.returncode == status, (args, result.returncode, result.stdout, result.stderr)
    return result


assert invoke("run", "-e", "List.sum [10;20;12]").stdout == "42\n"
assert invoke("run", "-e", "List.fold_left (+) 0 [10;20;12]").stdout == "42\n"
assert invoke("run", "-e", "(*) 6 7", "--no-stdlib").stdout == "42\n"
assert invoke("run", "-e", "List.map ((-) 10) [1;2;3]").stdout == "[9; 8; 7]\n"
assert "module bodies" in invoke("check", "-e", "module M = struct 42 end;; 0", status=1).stderr
assert invoke("type", "-e", "fun x -> x").stdout == "('a -> 'a)\n"
assert invoke("check", "-e", "42").stdout == "ok\n"
assert "constant" in invoke("ir", "-e", "42").stdout
assert "List.map" in invoke("stdlib").stdout
invoke("run", "-e", "1 + true", status=1)
invoke("run", "-e", "42", "--max-steps", "bad", status=2)
invoke("run", "-e", "42", "extra.gml", status=2)
invoke("build", "-e", "42", status=2)
invoke("bogus", status=2)
invoke("--help")
invoke("--version")

with tempfile.TemporaryDirectory(prefix="gateml-") as directory:
    directory = pathlib.Path(directory)
    source = directory / "main.gml"
    source.write_text("let twice f x = f (f x);; twice succ 40\n")
    assert invoke("run", str(source)).stdout == "42\n"
    source.write_text("\nif 1 then true else false")
    assert f"{source}:2:" in invoke("check", str(source), status=1).stderr
    image = directory / "program"
    samples = [
        ("40 + 2", 42, ""),
        ("let rec f n = if n = 0 then 1 else n * f (n - 1) in f 5", 120, ""),
        ("let add x y = x + y in add 12 (add 10 20)", 42, ""),
        ("let x = 3 in let f y = x + y in let x = 100 in f 4", 7, ""),
        ("IO.println_int 42; 7", 7, "42\n"),
        ("if true || (1 / 0 = 0) then 7 else 9", 7, ""),
        ("Int.abs (-42)", 42, ""),
        ("let inc x = x + 1 in let f x = inc x in f 41", 42, ""),
        ("let rec outer n = let get x = n + x in if n = 0 then get 2 else outer (n - 1) + get 1 in outer 3", 11, ""),
        ("let x = 8 in let f y = x - y in let g x = f x + x in g 3", 8, ""),
        ("let x = 3 in let rec x n = if n = 0 then 42 else x (n-1) in x 3", 42, ""),
        ("let rec f n = if n = 0 then 42 else f (n-1) in let g = f in g 5", 42, ""),
    ]
    for text, code, output in samples:
        invoke("build", "-e", text, "-o", str(image))
        assert image.read_bytes()[:4] == b"\x7fELF"
        if sys.platform == "linux" and platform.machine() in ("x86_64", "AMD64"):
            image.chmod(0o700)
            result = subprocess.run([str(image)], text=True, capture_output=True, timeout=10)
            assert (result.returncode, result.stdout) == (code, output), (text, result)
    invoke("build", "-e", "List.sum [1;2]", "-o", str(image), status=1)
    invoke("build", "-e", "(+) 20 22", "-o", str(image), status=1)

    # Ground-truth scalar cases test function values against infix semantics,
    # and use parameters to exercise native operations rather than only folding
    # closed constant expressions at compile time.
    operators = [
        ("+", "13", "5", "18"), ("-", "13", "5", "8"),
        ("*", "13", "5", "65"), ("/", "-13", "5", "-2"),
        ("mod", "-13", "5", "-3"),
        ("=", "13", "5", "false"), ("<>", "13", "5", "true"),
        ("<", "13", "5", "false"), (">", "13", "5", "true"),
        ("<=", "5", "5", "true"), (">=", "5", "5", "true"),
        ("&&", "true", "false", "false"), ("||", "false", "true", "true"),
    ]
    for operator, left, right, expected in operators:
        direct = f"({left}) {operator} ({right})"
        function = f"let operation = ({operator}) in operation ({left}) ({right})"
        assert invoke("run", "--no-stdlib", "-e", direct).stdout == expected + "\n"
        assert invoke("run", "--no-stdlib", "-e", function).stdout == expected + "\n"
        function = f"let operation x y = x {operator} y in operation ({left}) ({right})"
        if expected in ("true", "false"):
            function = f"if ({function}) then 1 else 0"
            code = int(expected == "true")
        else:
            code = int(expected) & 255
        invoke("build", "--no-stdlib", "-e", function, "-o", str(image))
        if sys.platform == "linux" and platform.machine() in ("x86_64", "AMD64"):
            image.chmod(0o700)
            result = subprocess.run([str(image)], capture_output=True, timeout=10)
            assert result.returncode == code, (operator, left, right, result)
