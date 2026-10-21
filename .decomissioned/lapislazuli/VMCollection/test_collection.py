#!/usr/bin/env python3
"""Regeneration, C99/header checks and executable tests for every collection VM.

Run from anywhere with Python 3.10+, Lua 5.4 and a C compiler. Override LUA, CC
and CFLAGS (e.g. '-O1 -g -fsanitize=undefined -fno-sanitize-recover=all').
Temporary artifacts live under /tmp/opencode and are removed on completion.
"""
from dataclasses import dataclass
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile


HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
LUA = shlex.split(os.environ.get("LUA", "lua5.4"))
CC = shlex.split(os.environ.get("CC", "cc"))
CFLAGS = ["-std=c99", "-Wall", "-Wextra", "-Werror"] + shlex.split(os.environ.get("CFLAGS", ""))


@dataclass
class Case:
    code: str
    out: int | None = None
    status: int = 0
    compile_status: int = 0
    budget: int = 4096
    setup: str = ""
    check: str = "1"


# Names are the generated public C prefixes, not necessarily directory names.
CASES = {
    "ART": ("ART", [
        Case("CONST 0 6 CONST 1 7 MUL_INT 2 0 1 PRINT 2 HALT", 42),
        Case("CONST 1 41 INVOKE_STATIC1 &f 1 MOVE_RESULT 2 PRINT 2 HALT f: CONST 1 1 ADD_INT 0 0 1 RETURN 0", 42),
        Case("CONST 0 2147483647 CONST 1 1 ADD_INT 0 0 1 PRINT 0 HALT", -2147483648),
        Case("CONST 16 1 HALT", status=-4),
        Case("CONST 0 1 CONST 1 0 DIV_INT 0 0 1 HALT", status=-10),
    ]),
    "BEAM": ("BEAM", [
        Case("MOVE_INT 0 -6 MOVE_INT 1 -7 MUL 2 0 1 PRINT 2 HALT", 42),
        Case("MOVE_INT 0 42 PUT_NIL 1 PUT_LIST 2 0 1 GET_LIST 2 3 4 PRINT 3 HALT", 42),
        Case("MOVE_INT 0 42 SEND 0 MOVE_INT 0 0 RECEIVE 1 &empty PRINT 1 HALT empty: HALT", 42),
        Case("RECEIVE 0 &empty MOVE_INT 0 0 PRINT 0 HALT empty: MOVE_INT 0 42 PRINT 0 HALT", 42),
        Case("MOVE_INT 0 42 STORE_Y 0 0 CALL &f LOAD_Y 0 0 PRINT 0 HALT f: MOVE_INT 0 7 STORE_Y 0 0 RETURN", 42),
        Case("MOVE_INT -1 0 HALT", status=-4),
        Case("PUT_NIL 0 GET_LIST 0 1 2 HALT", status=-4),
        Case("MOVE_INT 0 1 " + "SEND 0 " * 65 + "HALT", status=-3),
    ]),
    "B-Machine": ("BMachine", [
        Case("LOADC -6 LOADC -7 MUL PRINT STOP", 42),
        Case("MKCLOS &outer 1 0 LOADC 42 APP1 &outer PRINT STOP outer: MKCLOS &inner 1 0 LOADC 99 APP1 &inner PRINT LOAD 0 RET inner: LOAD 0 RET", 42),
    ]),
    "C-Machine": ("CMachine", [
        Case("LOADC 6 LOADC 7 MUL PRINT STOP", 42),
        Case("MARK CALL0 &f PRINT STOP f: LOADC 42 RETURN", 42),
        Case("MARK LOADC 41 CALL1 &f PRINT STOP f: LOADR 4 LOADC 1 ADD RETURN1", 42),
    ]),
    "CAM": ("CAM", [
        Case("QUOTE -42 PRINT STOP", -42),
        Case("QUOTE -6 QUOTE -7 MUL PRINT STOP", 42),
        Case("QUOTE 42 QUOTE 0 CONS FST PRINT STOP", 42),
        Case("QUOTE 0 FST STOP", status=-4),
        Case("FST STOP", status=-4, setup="vm.vw_stack[0] = (9999 << 3) | 2; vm.vw_sp = 1;"),
    ]),
    "CESK": ("CESK", [
        Case("CONST 6 CONST 7 MUL PRINT STOP", 42),
        Case("CONST 41 LAM &f APP1 &f PRINT STOP f: VAR 0 CONST 1 ADD RET", 42),
        Case("CONST 2 CONST 40 LAM &make APP1 &make CONST 99 LAM &noise APP1 &noise PRINT APP1 &body PRINT STOP make: LAM &body RET body: VAR 1 VAR 0 ADD RET noise: VAR 0 RET", 42),
        Case("CONST 1 CONST 40 LAM &make APP1 &make APP1 &body PRINT STOP make: LAM &body CONST 41 SET 0 RET body: VAR 1 VAR 0 ADD RET", 42),
        Case("CONST 1 CONST 0 DIV STOP", status=-10),
        Case("VAR -1 STOP", status=-4),
    ]),
    "CLR": ("CLR", [
        Case("LDC_I4 6 LDC_I4 7 MUL PRINT HALT", 42),
        Case("LDC_I4 41 CALL1 &f PRINT HALT f: LDARG 0 LDC_I4 1 ADD RET", 42),
        Case("LDC_I4 2147483647 LDC_I4 1 ADD PRINT HALT", -2147483648),
        Case("LDC_I4 2147483647 LDC_I4 1 ADD_OVF HALT", status=-10),
        Case("LDC_I4 -2147483648 LDC_I4 -1 DIV HALT", status=-10),
        Case("LDLOC 16 HALT", status=-4),
        Case("RET HALT", status=-2),
    ]),
    "CMCM": ("CMCM", [
        Case("MKINT -42 MKVAL VAR 0 GVAL PRINT GVAL PRINT STOP", -42),
        Case("MKINT 42 MKVAL MPAIR 1 PROJ 0 GVAL PRINT STOP", 42),
        Case("MKINT 42 MKVAL MPAIR 1 CLOSURE &f 0 APPLY &f STOP f: VAR 1 GVAL PRINT RET", 42),
        Case("MPAIR 1 STOP", status=-2),
    ]),
    "FAM": ("FAM", [
        Case("INT -42 PRINT STOP", -42),
        Case("INT -6 INT -7 MUL PRINT STOP", 42),
        Case("INT 0 BRANCHIF &bad INT 42 PRINT STOP bad: INT 0 PRINT STOP", 42),
        Case("INT 42 CLOSURE &f 0 APPLY1 &f PRINT STOP f: ACCESS 0 RETURN", 42),
        Case("ALLOC 1 INT 42 CLOSURE &f 1 POPACC PUSHACC SETFIELD 0 PUSHACC GETFIELD 0 PRINT STOP f: STOP", 42),
        Case("INT 42 CLOSURE &outer 0 APPLY1 &outer PRINT STOP outer: INT 99 CLOSURE &inner 0 APPLY1 &inner POPACC ACCESS 0 RETURN inner: ACCESS 0 RETURN", 42),
    ]),
    "FORTH": ("FORTH", [
        Case("LIT 6 LIT 7 MUL PRINT BYE", 42),
        Case("LIT 42 VARADDR 0 STORE VARADDR 0 FETCH PRINT BYE", 42),
        Case("LIT 0 LIT 3 LIT 0 DO loop: I ADD LOOP &loop PRINT BYE", 3),
        Case("CALL &f PRINT BYE f: LIT 42 EXIT", 42),
        Case("LIT 1 LIT 0 DIV BYE", status=-10),
        Case("DROP BYE", status=-2),
        Case("LIT -1 FETCH BYE", status=-4),
    ]),
    "FUL": ("FUL", [
        Case("CONST -6 CONST -7 MUL PRINT HALT", 42),
        Case("CONST 40 BIND CLOSURE &f UNBIND CONST 99 BIND CONST 2 APPLY PRINT HALT f: ACCESS 1 ACCESS 0 ADD RETURN", 42),
        Case("ACCESS 0 HALT", status=-4),
        Case("CONST 1 CONST 2 APPLY HALT", status=-4),
    ]),
    "G-Machine": ("GMachine", [
        Case("PUSHINT 42 EVAL PRINT STOP", 42),
        Case("PUSHFUN &f EVAL PRINT STOP f: PUSHINT 42 UPDATE1 RET0", 42),
        Case("PUSHFUN &f PUSHINT 42 MKAP EVAL PRINT STOP f: PUSH 0 UPDATE2 RET1", 42),
        Case("PUSH 0 PRINT STOP", status=-4, setup="vm.s[0] = 500; vm.sp = 1;"),
        Case("ALLOC1 EVAL STOP", status=-4),
    ]),
    "JVM": ("JVM", [
        Case("ICONST 6 ICONST 7 IMUL PRINT HALT", 42),
        Case("ICONST 41 INVOKESTATIC1 &f PRINT HALT f: ILOAD 0 ICONST 1 IADD IRETURN", 42),
        Case("ICONST 42 ISTORE 0 INVOKESTATIC &f ILOAD 0 PRINT HALT f: ICONST 0 ISTORE 0 RETURN", 42),
        Case("ICONST -2147483648 ICONST -1 IDIV PRINT HALT", -2147483648),
        Case("ICONST 2147483647 ICONST 1 IADD PRINT HALT", -2147483648),
        Case("ICONST 1 ICONST 0 IDIV HALT", status=-10),
        Case("ILOAD -1 HALT", status=-4),
        Case("IRETURN HALT", status=-2),
        Case("again: GOTO &again", status=-7, budget=10),
    ]),
    "Krivine": ("Krivine", [
        Case("CONST -42 PRINT STOP", -42),
        Case("CONST -6 CONST -7 MUL PRINT STOP", 42),
        Case("PUSH &f ENTER PUSHVAL PRINT STOP f: CONST 42 POPVAL RET", 42),
    ]),
    "LuaVM": ("LuaVM", [
        Case("LOADI 0 6 LOADI 1 7 MUL 2 0 1 PRINT 2 HALT", 42),
        Case("NEWTABLE 0 LOADI 1 42 SETTABLE 0 3 1 GETTABLE 2 0 3 PRINT 2 HALT", 42),
        Case("LOADI 0 -7 LOADI 1 3 IDIV 2 0 1 PRINT 2 HALT", -3),
        Case("LOADI 0 9223372036854775807 LOADI 1 1 ADD 2 0 1 PRINT 2 HALT", -9223372036854775808),
        Case("LOADI 0 41 LOADFUNC 1 &f CALL1 2 1 0 PRINT 2 HALT f: LOADI 1 1 ADD 0 0 1 RETURN1 0", 42),
        Case("LOADI 0 0 TEST 0 &ok LOADI 1 0 PRINT 1 HALT ok: LOADI 1 42 PRINT 1 HALT", 42),
        Case("LOADI 16 1 HALT", status=-4),
        Case("NEWTABLE 0 GETTABLE 1 0 0 HALT", status=-4),
        Case("LOADI 0 1 LOADI 1 0 IDIV 2 0 1 HALT", status=-10),
    ]),
    "P-Code": ("PCode", [
        Case("LDC 6 LDC 7 MPI WRINT STP", 42),
        Case("LDC 42 STO 0 CALL &f LOD 0 WRINT STP f: LDC 0 STO 0 RET", 42),
        Case("LDC 41 CALL1 &f WRINT STP f: LOD 0 LDC 1 ADI RETV", 42),
        Case("LDC 1 LDC 0 DVI STP", status=-10),
    ]),
    "SECD": ("SECD", [
        Case("LDC 6 LDC 7 MUL PRINT STOP", 42),
        Case("LDC 42 NIL CONS CAR PRINT STOP", 42, setup="SECD_allocator a = { NULL, allocate, resize, release }; if(SECD_memory_set(&vm,a)) return 1;"),
        Case("LDF &f LDC 41 AP1 &f PRINT STOP f: LD 0 LDC 1 ADD RTN", 42),
        Case("LDF &make LDC 40 AP1 &make LDF &noise LDC 99 AP1 &noise PRINT LDC 2 AP1 &body PRINT STOP make: LDF &body RTN body: LD 1 LD 0 ADD RTN noise: LD 0 RTN", 42),
        Case("LDC 1 CAR STOP", status=-4),
    ]),
    "WASM": ("WASM", [
        Case("I32_CONST 6 I32_CONST 7 I32_MUL PRINT HALT", 42),
        Case("I32_CONST 1 I32_CONST 42 I32_STORE 0 I32_CONST 1 I32_LOAD 0 PRINT HALT", 42,
             check="vm.memory[1] == 42 && vm.memory[2] == 0"),
        Case("I32_CONST 65532 I32_CONST -1 I32_STORE 0 I32_CONST 65532 I32_LOAD 0 PRINT HALT", -1),
        Case("I32_CONST 41 CALL1 &f PRINT HALT f: LOCAL_GET 0 I32_CONST 1 I32_ADD RETURN", 42),
        Case("I32_CONST 2147483647 I32_CONST 1 I32_ADD PRINT HALT", -2147483648),
        Case("I32_CONST -2147483648 I32_CONST -1 I32_DIV_S HALT", status=-10),
        Case("I32_CONST 65533 I32_LOAD 0 HALT", status=-4),
        Case("I32_CONST -1 I32_LOAD 0 HALT", status=-4),
        Case("UNREACHABLE HALT", status=-10),
        Case("LOCAL_GET 16 HALT", status=-4),
    ]),
    "YARVM": ("YARVM", [
        Case("PUTOBJECT -6 PUTOBJECT -7 OPT_MULT PRINT HALT", 42),
        Case("PUTOBJECT 1 PUTOBJECT 42 NEWARRAY2 PUTOBJECT -1 OPT_AREF PRINT HALT", 42),
        Case("PUTOBJECT 0 BRANCHIF &ok PUTOBJECT 0 PRINT HALT ok: PUTOBJECT 42 PRINT HALT", 42),
        Case("PUTFALSE BRANCHUNLESS &ok PUTOBJECT 0 PRINT HALT ok: PUTOBJECT 42 PRINT HALT", 42),
        Case("PUTOBJECT 40 PUTOBJECT 2 SEND1 &f PRINT HALT f: PUTSELF GETLOCAL 0 OPT_PLUS LEAVE", 42),
        Case("GETLOCAL -1 HALT", status=-4),
        Case("PUTNIL PUTOBJECT 0 OPT_AREF HALT", status=-4),
    ]),
}

for directory, name, push, local, field, store, ret in [
    ("SOM", "SOM", "PUSH_INT", "PUSH_ARGUMENT", "PUSH_FIELD", "POP_FIELD", "RETURN_LOCAL"),
    ("Smalltalk-80", "Smalltalk80", "PUSH_CONSTANT", "PUSH_TEMPORARY", "PUSH_FIELD", "STORE_FIELD", "RETURN_TOP"),
]:
    CASES[directory] = (name, [
        Case(f"{push} -6 {push} -7 MUL PRINT HALT", 42),
        Case(f"METHOD 1 7 &f NEW 1 {push} 42 SEND 7 PRINT HALT f: {local} 0 {store} 0 {field} 0 {ret}", 42),
        Case(f"NEW 1 {push} 0 SEND 99 HALT", status=-4),
        Case(f"{local} 16 HALT", status=-4),
        Case("NEW 1 " * 65 + "HALT", status=-3),
    ])

for directory, var, integer, unify, match, choice, retry, trust, nil, cons in [
    ("WAM", "PUT_VARIABLE", "PUT_INTEGER", "GET_VALUE", "GET_INTEGER", "TRY_ME_ELSE", "RETRY_ME_ELSE", "TRUST_ME", "PUT_NIL", "PUT_LIST"),
    ("PROL", "VARIABLE", "INTEGER", "UNIFY", "MATCH_INTEGER", "CHOICE", "RETRY", "COMMIT", "NIL", "CONS"),
]:
    CASES[directory] = (directory, [
        Case(f"{var} 0 {integer} 1 42 {unify} 0 1 PRINT 0 HALT", 42),
        Case(f"{var} 0 {choice} &alt {integer} 1 7 {unify} 0 1 FAIL alt: {trust} {integer} 1 42 {unify} 0 1 PRINT 0 HALT", 42,
             check="vm.choice_top == 0 && vm.trail_top == 0"),
        Case(f"{var} 0 {choice} &alt {match} 0 1 FAIL alt: {retry} &last {match} 0 2 FAIL last: {trust} {match} 0 42 PRINT 0 HALT", 42),
        Case(f"{var} 0 {nil} 1 {cons} 2 0 1 {integer} 3 42 {cons} 4 3 1 {unify} 2 4 PRINT 0 HALT", 42),
        Case(f"{var} 0 CALL &predicate {match} 0 42 PRINT 0 HALT predicate: {choice} &alt {integer} 1 7 {unify} 0 1 "
             + ("PROCEED" if directory == "WAM" else "RETURN")
             + f" alt: {trust} {match} 0 42 " + ("PROCEED" if directory == "WAM" else "RETURN"), 42),
        Case(f"{integer} 0 1 {match} 0 2 HALT", status=-11),
        Case("FAIL HALT", status=-11),
        Case(f"{var} 16 HALT", status=-4),
        Case(f"{choice} &end " * 33 + "end: HALT", status=-3),
    ])

# Pointer-width edge cases are seeded in C, so these tests also work on ILP32.
for directory, add, div, stop in [
    ("B-Machine", "ADD", "DIV", "STOP"), ("C-Machine", "ADD", "DIV", "STOP"),
    ("CESK", "ADD", "DIV", "STOP"), ("CMCM", "ADD", "DIV", "STOP"),
    ("FORTH", "ADD", "DIV", "BYE"), ("G-Machine", "ADD", "DIV", "STOP"),
    ("P-Code", "ADI", "DVI", "STP"), ("SECD", "ADD", "DIV", "STOP"),
]:
    CASES[directory][1].extend([
        Case(f"{add} {stop}", setup="vm.vw_stack[0] = INTPTR_MAX; vm.vw_stack[1] = 1; vm.vw_sp = 2;",
             check="vm.vw_sp == 1 && vm.vw_stack[0] == INTPTR_MIN"),
        Case(f"{div} {stop}", status=-10,
             setup="vm.vw_stack[0] = INTPTR_MIN; vm.vw_stack[1] = -1; vm.vw_sp = 2;"),
    ])
for directory in ["CAM", "FAM", "Krivine"]:
    CASES[directory][1].append(Case("MUL PRINT STOP", 1, setup=
        "vm.vw_stack[0] = (intptr_t)(((uintptr_t)(INTPTR_MAX / 8) << 3) | 1u); "
        "vm.vw_stack[1] = vm.vw_stack[0]; vm.vw_sp = 2;"))


def run(command, **kwargs):
    return subprocess.run(command, check=True, text=True, **kwargs)


def test_vm(directory, name, cases, temp):
    """Link the public header against its separate generated implementation."""
    lines = [f'#include "{directory}/{directory}.h"', '#include <stdio.h>']
    if directory == "SECD":
        lines += [
            "static void *allocate(void *ctx, size_t n) { (void)ctx; return malloc(n); }",
            "static void *resize(void *ctx, void *p, size_t n) { (void)ctx; return realloc(p,n); }",
            "static void release(void *ctx, void *p) { (void)ctx; free(p); }",
        ]
    lines.append("int main(void) {")
    # Reuse one VM/tape: sanitizer lifetime instrumentation otherwise reserves
    # one large stack slot per case even when their C block scopes do not overlap.
    lines += [f"  {name}_vm vm; {name}_instruction data[4096];",
              f"  {name}_tape tape = {{ data, 0, 4096 }}; int status;"]
    for index, case in enumerate(cases):
        lines += [
            "{", f"  {name}_init(&vm); tape.size = 0;", f"  {case.setup}",
            f"  status = {name}_compile({json.dumps(case.code)}, &tape);",
            f'  if(status != {case.compile_status}) {{ fprintf(stderr,"{directory} case {index}: compile %d\\n",status); return 1; }}',
        ]
        if not case.compile_status:
            lines += [
                f"  status = {name}_run(&vm, &tape, {case.budget});",
                f'  if(status != {case.status}) {{ fprintf(stderr,"{directory} case {index}: run %d at %zu\\n",status,vm.vw_pc); return 1; }}',
                f'  if(!({case.check})) {{ fprintf(stderr,"{directory} case {index}: state check failed\\n"); return 1; }}',
            ]
            if case.out is not None:
                value = "INT64_MIN" if case.out == -(2**63) else f"INT64_C({case.out})"
                lines.append(f'  if(vm.out != {value}) {{ fprintf(stderr,"{directory} case {index}: output %lld\\n",(long long)vm.out); return 1; }}')
        if directory == "SECD":
            lines.append("  SECD_object_clear(&vm);")
        lines.append("}")
    lines += ["return 0;", "}"]
    source, binary = temp / f"{name}_test.c", temp / f"{name}_test"
    source.write_text("\n".join(lines) + "\n")
    run(CC + CFLAGS + ["-I", str(HERE), str(source), str(HERE / directory / f"{directory}.c"), "-o", str(binary)])
    run([str(binary)], timeout=20)
    print(f"PASS {directory}: {len(cases)} execution/compiler cases", flush=True)


def main():
    directories = {p.name for p in HERE.iterdir() if p.is_dir() and (p / f"{p.name}.vmw").is_file()}
    assert directories == CASES.keys(), f"missing or extra test coverage: {directories ^ CASES.keys()}"
    run(LUA + [str(HERE / "regenerate.lua"), "--check"], cwd=ROOT)
    with tempfile.TemporaryDirectory(prefix="vmcollection-", dir="/tmp/opencode") as work:
        temp = Path(work)
        # Absolute invocation, invocation within the collection, and selected VMs
        # must agree with the checked-in, canonical #line paths.
        run(LUA + [str(HERE / "regenerate.lua"), "--check", "WAM", "JVM"], cwd=temp)
        run(LUA + ["regenerate.lua", "--check", "SOM"], cwd=HERE)
        for invalid in ["--unknown", "NotAVM"]:
            result = subprocess.run(LUA + [str(HERE / "regenerate.lua"), invalid], capture_output=True)
            assert result.returncode != 0, f"accepted invalid selection {invalid}"
        listing = run(LUA + [str(HERE / "regenerate.lua"), "--list"], capture_output=True)
        assert listing.stdout.splitlines() == sorted(CASES)
        # Stale/missing artifacts must be detected without rewriting user files.
        copy = temp / "collection"
        shutil.copytree(HERE, copy)
        shutil.copytree(ROOT / "Weavers/VMWeave", temp / "Weavers/VMWeave")
        stale = copy / "JVM/JVM.h"
        stale.write_text("stale\n")
        result = subprocess.run(LUA + [str(copy / "regenerate.lua"), "--check", "JVM"], capture_output=True)
        assert result.returncode != 0 and stale.read_text() == "stale\n"
        stale.write_bytes((HERE / "JVM/JVM.h").read_bytes())
        (copy / "JVM/JVM.stk00").unlink()
        result = subprocess.run(LUA + [str(copy / "regenerate.lua"), "--check", "JVM"], capture_output=True)
        assert result.returncode != 0 and not (copy / "JVM/JVM.stk00").exists()
        run(LUA + [str(copy / "regenerate.lua"), "JVM"], cwd=temp)
        for extension in ["h", "c", "stk00"]:
            assert (copy / f"JVM/JVM.{extension}").read_bytes() == (HERE / f"JVM/JVM.{extension}").read_bytes()
        for directory, (name, cases) in sorted(CASES.items()):
            # Every generated parser must reject malformed and unknown input.
            cases = cases + [Case("NOT_AN_OPCODE", compile_status=-4), Case(""),
                             Case(cases[0].code, budget=0, status=-7)]
            test_vm(directory, name, cases, temp)
        headers = temp / "headers.c"
        headers.write_text("\n".join(f'#include "{d}/{d}.h"' for d in sorted(CASES)) + "\nint main(void) { return 0; }\n")
        run(CC + CFLAGS + ["-I", str(HERE), str(headers), "-o", str(temp / "headers")])
        # Retain coverage of VMWeave validation and dispatch hooks.
        run(LUA + [str(ROOT / "Weavers/VMWeave/test_vmweave.lua"), str(temp / "dsl.c")],
            env={**os.environ, "LUA_PATH": str(ROOT / "Weavers/VMWeave/?.lua") + ";;"})
        run(CC + CFLAGS + [str(temp / "dsl.c"), "-o", str(temp / "dsl")])
        run([str(temp / "dsl")])
    count = sum(len(cases) + 3 for _, cases in CASES.values())
    print(f"All 23 collection VMs ({count} cases), combined headers and VMWeave checks passed.")


if __name__ == "__main__":
    main()
