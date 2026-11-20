"""CLI and term-to-SMT translation appended to the standalone proof engine."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import sys


CORPUS_MAX_BYTES = 16 * 1024 * 1024
_PC_SMT_MAX_BYTES = 256 * 1024
_PC_PROGRAM_MAX_BYTES = 64 * 1024
_PC_MAX_INPUTS = 32
_PC_MAX_DEPTH = 24
_PC_MAX_NODES = 128
_PC_MAX_PROGRAMS = 32
_PC_MAX_TRIALS = 128
_PC_MAX_VISITS = 200_000
_PC_NAME = re.compile(r"[A-Za-z_][A-Za-z_0-9]{0,63}")
_PC_RULE_MARKER = "; archon-rule "
_PC_OPS = {
    "add": "bvadd", "sub": "bvsub", "mul": "bvmul",
    "and": "bvand", "or": "bvor", "xor": "bvxor",
    "not": "bvnot", "shl": "bvshl", "lshr": "bvlshr", "ashr": "bvashr",
    "copy": None,
}
PROGRAM_ARITY = {op: 1 if op in ("copy", "not") else 2 for op in _PC_OPS}


def bv_literal(value, width):
    return ("_", f"bv{value & ((1 << width) - 1)}", width)


def extend_word(value, size, width, signed):
    if size == width:
        return value
    return (("_", "sign_extend" if signed else "zero_extend", width - size), value)


def primitive_expression(op, operands):
    opcode = _PC_OPS[op]
    return operands[0] if opcode is None else (opcode, *operands)


def bind_archsem(tree, operands):
    if isinstance(tree, str) and tree.startswith("@"):
        return operands[int(tree[1:])]
    if isinstance(tree, (list, tuple)):
        return tuple(bind_archsem(item, operands) for item in tree)
    return tree


def check_expression_size(tree):
    work = [(tree, 1)]
    size = 0
    while work:
        term, depth = work.pop()
        size += 1
        if size > 4000 or depth > 64:
            raise ValueError("expanded SMT expression exceeds the proof bound")
        if isinstance(term, (list, tuple)):
            work.extend((item, depth + 1) for item in term)


def _pc_smt(term):
    if isinstance(term, (tuple, list)):
        return "(" + " ".join(_pc_smt(item) for item in term) + ")"
    return str(term)


def equation_query(declarations, left, right):
    """An UNSAT answer proves value equality for every declared assignment."""
    difference = ("not", ("=", left, right))
    check_expression_size(difference)
    commands = ["(set-logic QF_BV)"]
    commands.extend(f"(declare-const {name} (_ BitVec {width}))" for name, width in declarations)
    commands.extend((f"(assert {_pc_smt(difference)})", "(check-sat)"))
    return "\n".join(commands) + "\n"


def _pc_read(path, limit):
    with path.open("rb") as stream:
        data = stream.read(limit + 1)
    if len(data) > limit:
        raise ValueError(f"{path}: input exceeds the {limit}-byte proof bound")
    try:
        return data.decode("utf-8")
    except UnicodeError as exc:
        raise ValueError(f"{path}: expected UTF-8 text") from exc


def _pc_inventory(path, digest):
    corpus = _pc_read(path, CORPUS_MAX_BYTES)
    if hashlib.sha256(corpus.encode("utf-8")).hexdigest() != digest:
        raise ValueError(f"{path}: proof corpus does not match this generated Proof.py")
    entries = {}
    entry = None
    query = []
    for line in corpus.splitlines():
        if line.startswith(_PC_RULE_MARKER):
            if entry is not None:
                entries[entry["id"]] = (entry, "".join(query))
            entry = json.loads(line[len(_PC_RULE_MARKER):])
            query = []
        elif entry is not None and line != "(reset)":
            query.append(line + "\n")
    if entry is not None:
        entries[entry["id"]] = (entry, "".join(query))
    return entries


def _pc_list(entries):
    print("Generated rule inventory and verification boundaries (not applied events):")
    for rule, _ in entries.values():
        unknown = rule.get("unknown")
        status = "unknown" if unknown else "modeled"
        detail = f"; {unknown}" if unknown else ""
        print(f"{rule['id']}\t{status}\t{rule['kind']}\t{rule['evidence']}{detail}")
    if not entries:
        print("(no generated rules)")
    return 0


def _pc_print_result(label, status, model, detail=""):
    detail = f": {detail}" if detail else ""
    if status == "unsat":
        print(f"{label}: proved (unsat; bounded value equivalence){detail}")
    elif status == "sat":
        print(f"{label}: disproved (sat; counterexample){detail}")
        print("model: " + json.dumps(model, sort_keys=True))
    else:
        print(f"{label}: unknown{detail}")


def _pc_verify_inventory(entries, solve_query):
    if not entries:
        _pc_print_result("inventory", "unknown", {}, "no generated rules")
        return 2
    counts = {"unsat": 0, "sat": 0, "unknown": 0}
    for rule, query in entries.values():
        status, model, detail = "unknown", {}, rule.get("unknown")
        if detail is None:
            try:
                status, model = solve_query(query)
                detail = rule["evidence"] if status != "unknown" else "solver bound reached"
            except ValueError as exc:
                detail = f"generated query rejected: {exc}"
        counts[status] += 1
        _pc_print_result(rule["id"], status, model, detail)
    print(f"{counts['unsat']} proved, {counts['sat']} disproved, {counts['unknown']} unknown")
    return 1 if counts["sat"] else 2 if counts["unknown"] else 0


def _pc_json_object(pairs):
    fields = {}
    for key, value in pairs:
        if key in fields:
            raise ValueError(f"duplicate JSON key {key!r}")
        fields[key] = value
    return fields


def _pc_models(entries):
    models = {}
    for rule, _ in entries.values():
        if "model" in rule:
            model = rule["model"]
            models[rule["target"], model["name"]] = model
    return models


def _pc_immediate_fits(bits, width, argument):
    size, signed = argument
    low = bits & ((1 << size) - 1)
    if signed and low & (1 << (size - 1)):
        low -= 1 << size
    return bits == (low & ((1 << width) - 1))


def _pc_operand_matches(term, width, argument):
    if argument is None:
        return type(term) is not int
    return type(term) is int and _pc_immediate_fits(term, width, argument)


def _pc_program(path, models):
    text = _pc_read(path, _PC_PROGRAM_MAX_BYTES)
    try:
        obj = json.loads(text, object_pairs_hook=_pc_json_object)
        if not isinstance(obj, dict) or set(obj) != {"target", "width", "inputs", "result"}:
            raise ValueError("expected exactly target, width, inputs and result")
        target, width, inputs = obj["target"], obj["width"], obj["inputs"]
        if not isinstance(target, str) or not target or len(target) > 256:
            raise ValueError("target must be a generated architecture name")
        if type(width) is not int or not 1 <= width <= 64:
            raise ValueError("width must be an integer from 1 through 64")
        if (not isinstance(inputs, list) or len(inputs) > _PC_MAX_INPUTS
                or any(not isinstance(name, str) or not _PC_NAME.fullmatch(name) for name in inputs)
                or len(set(inputs)) != len(inputs)):
            raise ValueError("inputs must be unique simple names (up to 32)")
        names = set(inputs)
        nodes = 0

        def term(value, depth=1):
            nonlocal nodes
            nodes += 1
            if nodes > _PC_MAX_NODES or depth > _PC_MAX_DEPTH:
                raise ValueError("term exceeds the node or nesting bound")
            if type(value) is int:
                if not -(1 << (width - 1)) <= value < (1 << width):
                    raise ValueError(f"literal does not fit the {width}-bit word")
                return value & ((1 << width) - 1)
            if isinstance(value, str) and value in names:
                return value
            if (not isinstance(value, list) or not value
                    or not isinstance(value[0], str)):
                raise ValueError("expected an input name, integer literal or operation array")
            op = value[0]
            if op == "target":
                if len(value) < 2 or not isinstance(value[1], str):
                    raise ValueError("target form requires an instruction name")
                name = value[1]
                model = models.get((target, name))
                if model is None or model["width"] != width:
                    raise ValueError(f"no modeled target value for {target}:{name} at width {width}")
                if len(value) - 2 != len(model["arguments"]):
                    raise ValueError(f"incorrect number of operands for target instruction {name}")
                args = tuple(term(item, depth + 1) for item in value[2:])
                for arg, descriptor in zip(args, model["arguments"]):
                    # Rewrite graphs can use a constant value in a register
                    # position. The selector, in contrast, requires a value
                    # node there. Only enforce immediate constraints here.
                    if descriptor is not None and not _pc_operand_matches(arg, width, descriptor):
                        raise ValueError(f"immediate is not an encodable literal for {name}")
                return ("target", name, *args)
            if op not in PROGRAM_ARITY:
                raise ValueError(f"unsupported operation {op!r}")
            if len(value) - 1 != PROGRAM_ARITY[op]:
                raise ValueError(f"incorrect number of operands for {op}")
            return (op, *(term(item, depth + 1) for item in value[1:]))

        return target, width, tuple(sorted(inputs)), term(obj["result"])
    except (ValueError, RecursionError) as exc:
        raise ValueError(f"{path}: expected a supported JSON value term (.i/MIR unsupported): {exc}") from exc


def _pc_lower(term, target, width, symbols, models):
    if type(term) is int:
        return bv_literal(term, width)
    if isinstance(term, str):
        return symbols[term]
    if term[0] != "target":
        return primitive_expression(
            term[0], [_pc_lower(arg, target, width, symbols, models) for arg in term[1:]]
        )
    model = models[target, term[1]]
    args = []
    for arg, descriptor in zip(term[2:], model["arguments"]):
        if descriptor is not None:
            args.append(bv_literal(arg, descriptor[0]))
        else:
            args.append(_pc_lower(arg, target, width, symbols, models))
    return bind_archsem(model["expression"], args)


def _pc_compare(program, rewritten, models, solve_query):
    target, width, names, initial = program
    symbols = {name: f"v{i}" for i, name in enumerate(names)}
    left = _pc_lower(initial, target, width, symbols, models)
    right = _pc_lower(rewritten, target, width, symbols, models)
    declarations = ((symbol, width) for symbol in symbols.values())
    status, model = solve_query(equation_query(declarations, left, right))
    names_model = {name: model[symbol] for name, symbol in symbols.items()} if status == "sat" else {}
    return status, names_model


def _pc_apply(rule, term):
    if not isinstance(term, tuple):
        return None
    kind = rule["kind"]
    op = rule["op"]
    if kind == "rename":
        if term[:2] == ("target", rule["model"]["name"]):
            return (op, *(term[2 + i] for i in rule["mapping"]))
        return None
    if term[0] != op:
        return None
    if kind == "identity":
        side = rule["side"]
        return term[2 - side] if term[1 + side] == rule["constant"] else None
    if kind == "commute":
        return op, term[2], term[1]
    if kind == "select" and all(
        _pc_operand_matches(arg, rule["width"], descriptor)
        for arg, descriptor in zip(term[1:], rule["model"]["arguments"])
    ):
        return ("target", rule["model"]["name"], *term[1:])
    return None


def _pc_replacements(rule, term, visits, path=()):
    visits[0] += 1
    if visits[0] > _PC_MAX_VISITS:
        raise ValueError("search visit bound reached")
    replacement = _pc_apply(rule, term)
    if replacement is not None and replacement != term:
        yield path, replacement
    if isinstance(term, tuple):
        offset = 2 if term[0] == "target" else 1
        for i in range(offset, len(term)):
            for site, replacement in _pc_replacements(rule, term[i], visits, (*path, i - offset)):
                yield site, (*term[:i], replacement, *term[i+1:])


def _pc_rule(entries, rule_id):
    if rule_id not in entries:
        raise ValueError(f"unknown generated rule ID {rule_id!r}; use --list-rewrites")
    return entries[rule_id][0]


def _pc_verify_pair(entries, rule_id, before, after, solve_query):
    rule = _pc_rule(entries, rule_id)
    unknown = rule.get("unknown")
    if unknown:
        _pc_print_result(rule_id, "unknown", {}, unknown)
        return 2
    models = _pc_models(entries)
    program = _pc_program(before, models)
    rewritten = _pc_program(after, models)
    if program[:3] != rewritten[:3]:
        raise ValueError("initial and rewritten terms must declare the same target, width and inputs")
    if program[:2] != (rule["target"], rule["width"]):
        raise ValueError("program target/width does not match the generated rule")
    matches = any(
        replacement == rewritten[3]
        for _, replacement in _pc_replacements(rule, program[3], [0])
    )
    status, model = _pc_compare(program, rewritten[3], models, solve_query)
    if status == "sat":
        if matches:
            _pc_print_result(rule_id, status, model, "provided term pair")
        else:
            _pc_print_result("provided term pair", status, model,
                             f"not a one-step instance of {rule_id}; does not disprove the rule")
        return 1
    if not matches:
        _pc_print_result(rule_id, "unknown", {}, "provided pair is not a one-step rule instance")
        return 2
    if status == "unknown":
        _pc_print_result(rule_id, status, {}, "solver bound reached")
        return 2
    _pc_print_result(rule_id, status, {}, "one-step term instance")
    return 0


def _pc_find_error(entries, rule_id, paths, solve_query):
    rules = [entry[0] for entry in entries.values()] if rule_id == "all" else [_pc_rule(entries, rule_id)]
    models = _pc_models(entries)
    visits = [0]
    trials = inconclusive = 0
    for path in paths:
        try:
            program = _pc_program(path, models)
        except (OSError, ValueError) as exc:
            _pc_print_result(str(path), "unknown", {}, str(exc))
            inconclusive += 1
            continue
        target, width, _, term = program
        for rule in rules:
            if rule["target"] != target:
                continue
            if "unknown" in rule:
                _pc_print_result(rule["id"], "unknown", {}, rule["unknown"])
                inconclusive += 1
                continue
            if rule["width"] != width:
                continue
            for site, rewritten in _pc_replacements(rule, term, visits):
                if trials >= _PC_MAX_TRIALS:
                    print(f"Search unknown: reached the {_PC_MAX_TRIALS}-query bound.")
                    return 2
                trials += 1
                try:
                    status, model = _pc_compare(program, rewritten, models, solve_query)
                except ValueError as exc:
                    inconclusive += 1
                    _pc_print_result(rule["id"], "unknown", {}, str(exc))
                    continue
                if status == "sat":
                    location = ".".join(map(str, site)) if site else "root"
                    _pc_print_result(rule["id"], status, model, f"{path}, site {location}")
                    print("rewritten: " + json.dumps(rewritten))
                    return 1
                if status == "unknown":
                    inconclusive += 1
                    _pc_print_result(rule["id"], status, {}, f"{path}: solver bound reached")
    print(f"No counterexample found in {trials} matching one-step term instance(s).")
    if inconclusive or not trials:
        print("Search unknown: unsupported input/rule, solver cutoff, or no matching instance.")
        return 2
    return 0


def _pc_main(solve_query, corpus_digest, argv=None):
    parser = argparse.ArgumentParser(
        description="Check generated rule equations and bounded pure bit-vector value terms.",
        epilog=(
            'A program is JSON: {"target":"RISCV32","width":32,"inputs":["x"],'
            '"result":["add","x",0]}. Terms are names, integer literals or operation '
            f'arrays for {", ".join(PROGRAM_ARITY)}, plus modeled ["target","ADD",...] '
            "values. Every term shares a word width from 1 through 64 bits. The check compares "
            "pure, defined results, not register state. .i/MIR and runtime traces "
            "are not proof inputs."
        ),
    )
    action = parser.add_mutually_exclusive_group(required=True)
    action.add_argument("--list-rewrites", action="store_true", help="show static inventory, not applications")
    action.add_argument("--verify", nargs="+", metavar="ARG",
                        help="check all generated obligations, or verify ID INITIAL REWRITTEN")
    action.add_argument("--run", type=Path, metavar="FILE.smt",
                        help="solve one standalone SMT-LIB2 query")
    action.add_argument("--find-error", action="store_true",
                        help="search one-step rule instances in supplied value terms")
    parser.add_argument("--rewrite", metavar="ID|all", help="rule(s) for --find-error")
    parser.add_argument("--programs", type=Path, nargs="+", metavar="PROGRAM",
                        help="initial JSON value term file(s) for --find-error")
    parser.add_argument("--proofs", "--proof-file", type=Path, metavar="PROOFS.smt",
                        help="path to a copy of the matching generated Proofs.smt")
    args = parser.parse_args(argv)
    if args.find_error and (args.rewrite is None or not args.programs):
        parser.error("--find-error requires --rewrite ID|all and --programs PROGRAM ...")
    if not args.find_error and (args.rewrite is not None or args.programs is not None):
        parser.error("--rewrite and --programs are only for --find-error")
    if args.verify is not None and args.verify != ["all"] and (
        len(args.verify) != 3 or args.verify[0] == "all"
    ):
        parser.error("expected --verify all or --verify ID INITIAL REWRITTEN")
    if args.programs and len(args.programs) > _PC_MAX_PROGRAMS:
        parser.error(f"--programs accepts at most {_PC_MAX_PROGRAMS} files")
    try:
        if args.run is not None:
            if args.proofs is not None:
                parser.error("--run takes a standalone query, not the generated corpus")
            status, model = solve_query(_pc_read(args.run, _PC_SMT_MAX_BYTES))
            print(status)
            if status == "sat":
                print("model: " + json.dumps(model, sort_keys=True))
            return 2 if status == "unknown" else 0

        path = args.proofs or Path(__file__).resolve().with_name("Proofs.smt")
        entries = _pc_inventory(path, corpus_digest)
        if args.list_rewrites:
            return _pc_list(entries)
        if args.find_error:
            return _pc_find_error(entries, args.rewrite, args.programs, solve_query)
        if args.verify == ["all"]:
            return _pc_verify_inventory(entries, solve_query)
        rule_id, before, after = args.verify
        return _pc_verify_pair(entries, rule_id, Path(before), Path(after), solve_query)
    except (OSError, ValueError) as exc:
        print(f"Proof: unknown/error: {exc}", file=sys.stderr)
        return 2
