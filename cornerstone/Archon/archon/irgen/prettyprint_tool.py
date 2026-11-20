"""Emit a standalone formatter for native and shared Archon macro streams."""

STAGE_ORDER = 1200


def emit(context: object) -> dict[str, str]:
    """Generate the same target-independent script for every representation."""
    return {"PrettyPrint.py": _SOURCE}


_SOURCE = r'''#!/usr/bin/env python3
"""Render Archon .i records as a LaTeX or troff listing, without running C."""

import argparse
import io
import os
from pathlib import Path
import re
import sys
import tempfile
from typing import NamedTuple


MAX_INPUT = 8 * 1024 * 1024
MAX_TEMPLATE = 256 * 1024
MAX_BODY = 32 * 1024 * 1024
MAX_TITLE = 512
MAX_NESTING = 128
LINE_WIDTH = 72

_HELP = """\
Input is UTF-8 (optional BOM); use - for stdin or stdout. C-style line
continuations are joined, but # directives are skipped, never evaluated.
Records from every conditional branch are shown. Uppercase macro calls can
span multiple lines; their argument whitespace is normalized. Quoted C tokens
keep their escapes; comments inside calls are treated as spaces. Other text,
including top-level comments, is shown as a literal listing. Pairs of
*_BEGIN and *_END and adjacent operand records guide indentation. The listing
wraps long lines; it is not a C or IR validator.

A custom UTF-8 template is trusted LaTeX/troff source. Put exactly one
{{body}} on its own line, in document text (not in an argument, definition,
or verbatim block). Optional {{title}} inserts escaped inline text, intended
for ordinary text or a LaTeX heading, not troff request arguments. Use standard
LaTeX syntax and troff control/escape characters. Apart from newline
normalization, other template text is copied unchanged: no expressions, loops,
or recursive substitutions. The tool never runs LaTeX or troff.

Limits: input 8 MiB, template 256 KiB, title 512 characters (up to 16 title
placeholders), up to 128 nested argument delimiters including the call
parentheses, and body 32 MiB. Non-ASCII and nonprinting characters in
displayed tokens are written as escapes.
"""

_DEFAULT_TEMPLATES = {
    "latex": r"""\documentclass{article}
\usepackage[T1]{fontenc}
\begin{document}
\section*{{{title}}}
{{body}}
\end{document}
""",
    "troff": r""".\" Archon PrettyPrint
.po 0.6i
.ll 7.3i
.ad l
\fB\s+3{{title}}\s-3\fP
.br
.sp
{{body}}
""",
}

_BODY_LINE = re.compile(r"^[ \t]*\{\{body\}\}[ \t]*$", re.MULTILINE)
_PLACEHOLDER = re.compile(r"\{\{(body|title)\}\}")
_TOKEN = re.compile(
    r"(?P<space>[ \t\n\f\v]+)|(?P<block>/\*)|(?P<comment>//)|"
    r"""(?P<quote>["'])|(?P<word>[A-Za-z0-9_]+)|(?P<punct>.)"""
)
_MACRO = re.compile(r"[A-Z_][A-Z0-9_]*")
_QUOTE_MARKS = {'"': re.compile(r'["\\\n]'), "'": re.compile(r"['\\\n]")}
_CLOSE = {"(": ")", "[": "]", "{": "}"}

# Group flat operand records with the adjacent owning record when the first
# argument matches. Never move records or infer relationships across a gap.
_CHILDREN = {
    "IR_NODE": {"IR_INPUT"},
    "IR_OP": {"IR_INPUT", "IR_TARGETS"},
    "ARCHON_PROGRAM_OP": {
        "ARCHON_PROGRAM_OPERAND", "ARCHON_PROGRAM_LITERAL", "ARCHON_PROGRAM_SYMBOL",
        "ARCHON_PROGRAM_TEXT", "ARCHON_PROGRAM_ATTRIBUTE", "ARCHON_PROGRAM_SUCCESSOR",
    },
    "ARCHON_MIR_INSTRUCTION": {
        "ARCHON_MIR_DEF", "ARCHON_MIR_USE", "ARCHON_MIR_IMMEDIATE",
        "ARCHON_MIR_SUCCESSOR", "ARCHON_MIR_DEPENDENCY",
    },
    "ARCHON_MIR_NODE": {"ARCHON_MIR_OPERAND"},
    "MIR_OP": {
        "MIR_INPUT_VALUE", "MIR_INPUT_INTEGER", "MIR_INPUT_BOOLEAN",
        "MIR_INPUT_SYMBOL", "MIR_DEPENDENCY",
    },
    "ARCHON_INSTR": {"ARCHON_INSTR_OPERAND"},
}
_ANCHORS = ("_FUNCTION", "_BLOCK", "_LABEL", "_ENTRY", "_ROOT", "_SCHEMA")
_CONTROL_ESCAPES = {n: f"\\x{n:02x}" for n in (*range(32), 127)}


class _Token(NamedTuple):
    kind: str
    text: str
    offset: int


class _Record(NamedTuple):
    name: str
    args: list
    suffix: str


def _line_end(source, offset):
    end = source.find("\n", offset)
    return len(source) if end == -1 else end


def _quoted_token(source, start):
    quote = source[start]
    marks = _QUOTE_MARKS[quote]
    offset = start + 1
    while True:
        mark = marks.search(source, offset)
        if mark is None:
            return "bad_quote", len(source)
        offset = mark.start()
        if source[offset] == quote:
            return "literal", offset + 1
        if source[offset] == "\n":
            return "bad_quote", offset
        offset += 2  # Preserve both characters of a C-style escape.


def _fail(source, offset, reason):
    line = source.count("\n", 0, offset) + 1
    raise ValueError(f"logical input line {line}: {reason}")


def _tokens(source):
    offset = 0
    line_start = True
    directive = False
    while offset < len(source):
        start = offset
        match = _TOKEN.match(source, offset)
        kind, offset = match.lastgroup, match.end()
        if kind == "space":
            if "\n" in match.group():
                line_start = True
                directive = False
        elif kind == "block":
            end = source.find("*/", offset)
            if end == -1:
                _fail(source, start, "unterminated block comment")
            offset = end + 2
            if not directive and "\n" in source[start:offset]:
                line_start = True
            kind = "comment"
        elif kind == "comment":
            offset = _line_end(source, offset)
        else:
            if kind == "punct" and match.group() == "#" and line_start:
                directive = True
            line_start = False
            if kind == "quote":
                kind, offset = _quoted_token(source, start)
        if not directive:
            yield _Token(kind, source[start:offset], start)


def _arguments(tokens, source, call):
    expected = [")"]
    args, parts = [], []
    space = False
    for token in tokens:
        part = token.text
        if token.kind in ("space", "comment"):
            space = bool(parts)
            continue
        if token.kind == "bad_quote":
            _fail(source, token.offset, f"unterminated quoted token in {call.text}")
        if part in _CLOSE:
            if len(expected) >= MAX_NESTING:
                _fail(source, token.offset, f"too many nested argument delimiters in {call.text}")
            expected.append(_CLOSE[part])
        elif part in _CLOSE.values():
            if expected.pop() != part:
                _fail(source, token.offset, f"mismatched argument delimiter {part!r} in {call.text}")
            if not expected:
                if parts or args:
                    args.append("".join(parts))
                return args
        elif part == "," and len(expected) == 1:
            args.append("".join(parts))
            parts, space = [], False
            continue
        if space:
            parts.append(" ")
        parts.append(part)
        space = False
    _fail(source, call.offset, f"unterminated call to {call.text}")


def _records(source):
    tokens = iter(_tokens(source))
    current = next(tokens, None)
    while current is not None:
        token, current = current, next(tokens, None)
        if token.kind == "space":
            continue
        if token.kind == "comment":
            yield token.text
            continue
        parts = [token.text]
        if token.kind == "word" and _MACRO.fullmatch(token.text):
            while current is not None and current.kind in ("space", "comment"):
                parts.append(current.text)
                current = next(tokens, None)
            if current is not None and current.text == "(":
                args = _arguments(tokens, source, token)
                current = next(tokens, None)
                if current is not None and current.kind == "space":
                    current = next(tokens, None)
                suffix = ""
                if current is not None and current.text == ";":
                    suffix, current = ";", next(tokens, None)
                yield _Record(token.text, args, suffix)
                continue
            if any("\n" in part for part in parts):
                yield "".join(parts).rstrip()
                continue
        # A stream can also contain small C fragments (e.g. a Memory SSA
        # operand array). Display these lines without parsing them as C.
        while current is not None:
            if current.kind == "space" and "\n" in current.text:
                break
            parts.append(current.text)
            current = next(tokens, None)
        yield "".join(parts).rstrip()


def _record_key(name):
    if name.startswith(("ARCHON_IR_", "ARCHON_CPS_")):
        return name[7:]
    if name.startswith("SC_"):
        return "IR_" + name[3:]
    return name


def _visible(text):
    """Represent nonprinting/Unicode data without target font dependencies."""
    return text.encode("ascii", "backslashreplace").decode("ascii").translate(_CONTROL_ESCAPES)


def _wrapped(text, indent, heading=False):
    width = LINE_WIDTH - indent
    start = 0
    while len(text) - start > width:
        cut = text.rfind(" ", start + width // 2, start + width + 1)
        if cut < 0:
            cut = start + width
        yield indent, heading, text[start:cut]
        start = cut + (text[cut] == " ")
    if start < len(text):
        yield indent, heading, text[start:]


def _call_lines(record, indent, heading):
    first = record.name + "("
    last = ")" + record.suffix
    args = [_visible(arg) for arg in record.args]
    single = first + ", ".join(args) + last
    if not args or len(single) + indent <= LINE_WIDTH:
        yield from _wrapped(single, indent, heading)
        return
    yield from _wrapped(first, indent, heading)
    indent += 2
    line = ""
    for index, arg in enumerate(args):
        field = arg + (last if index == len(args) - 1 else ",")
        if line and indent + len(line) + 1 + len(field) > LINE_WIDTH:
            yield from _wrapped(line, indent)
            line = ""
        line = line + " " + field if line else field
    yield from _wrapped(line, indent)


def _lines(source):
    scopes = []
    parent = None
    for record in _records(source.replace("\\\n", "")):
        if isinstance(record, str):
            for line in record.split("\n"):
                yield from _wrapped(_visible(line.expandtabs(4)), min(len(scopes), 8) * 2)
            continue
        key = _record_key(record.name)
        scope, _, boundary = key.rpartition("_")
        # ANF's IR_{LET,CALL}_NAMED_BEGIN use the ordinary *_END records.
        if scope.endswith("_NAMED"):
            scope = scope[:-6]
        if boundary == "END" and scopes and scopes[-1] == scope:
            scopes.pop()
        indent = min(len(scopes), 8) * 2
        if parent and record.args and record.args[0] == parent[0] and key in parent[1]:
            indent += 2
        else:
            parent = (record.args[0], _CHILDREN[key]) if record.args and key in _CHILDREN else None
        heading = boundary in ("BEGIN", "END") or key.endswith(_ANCHORS)
        yield from _call_lines(record, indent, heading)
        if boundary == "BEGIN":
            scopes.append(scope)


_LATEX_ESCAPE = str.maketrans({
    "\\": r"\textbackslash{}", "{": r"\{", "}": r"\}", "%": r"\%",
    "$": r"\$", "&": r"\&", "_": r"\_", "#": r"\#",
    "^": r"\textasciicircum{}", "~": r"\textasciitilde{}",
    "<": r"\textless{}", ">": r"\textgreater{}", "|": r"\textbar{}",
    '"': r"\textquotedbl{}", "'": r"\textquotesingle{}", "`": r"\textasciigrave{}",
})


def _latex_body(source):
    yield r"{\small\ttfamily\raggedright"
    for indent, heading, text in _lines(source):
        code = text.translate(_LATEX_ESCAPE).replace(" ", r"\ ")
        if heading:
            code = r"{\sffamily\bfseries " + code + "}"
        margin = r"\hspace*{" + f"{indent * 0.5:g}em" + "}" if indent else ""
        yield r"\noindent{}" + margin + code + r"\par"
    yield "}"


def _troff_text(text):
    # The zero-width start prevents a literal '.' or apostrophe at the
    # beginning of a new output line from being recognized as a request.
    return r"\&" + text.replace("\\", r"\e")


def _troff_body(source):
    yield ".nf"
    for indent, heading, text in _lines(source):
        code = _troff_text(" " * indent + text)
        yield r"\fB" + code + r"\fP" if heading else code
    yield ".fi"


def _body(lines):
    output = io.StringIO()
    for line in lines:
        if output.tell() + len(line) + 1 > MAX_BODY:
            raise ValueError(f"formatted body exceeds {MAX_BODY} bytes")
        output.write(line)
        output.write("\n")
    return output.getvalue()[:-1]


def _render(source, target, template, title):
    body_marker = _BODY_LINE.search(template)
    if body_marker is None or template.count("{{body}}") != 1:
        raise ValueError("template needs exactly one {{body}} on its own line")
    if template.count("{{title}}") > 16:
        raise ValueError("template has more than 16 {{title}} placeholders")
    template = template[:body_marker.start()] + "{{body}}" + template[body_marker.end():]
    if target == "latex":
        title = _visible(title).translate(_LATEX_ESCAPE)
        body = _body(_latex_body(source))
    else:
        title = _troff_text(_visible(title))
        body = _body(_troff_body(source))
    values = {"title": title, "body": body}
    result = _PLACEHOLDER.sub(lambda match: values[match[1]], template)
    # In particular, the final troff request needs an ending newline.
    return result if result.endswith("\n") else result + "\n"


def _read(path, limit, label):
    if path is None:
        data = sys.stdin.buffer.read(limit + 1)
    else:
        with path.open("rb") as stream:
            data = stream.read(limit + 1)
    if len(data) > limit:
        raise ValueError(f"{label} exceeds {limit} bytes")
    try:
        return data.decode("utf-8-sig").replace("\r\n", "\n").replace("\r", "\n")
    except UnicodeDecodeError as exc:
        raise ValueError(f"{label} is not valid UTF-8 (byte {exc.start + 1})") from None


def _check_output(path, inputs):
    if path.is_symlink() or (path.exists() and not path.is_file()):
        raise ValueError(f"output {str(path)!r} must be a regular file or a new path")
    for source in inputs:
        if source is not None and (
            path.resolve() == source.resolve()
            or (path.exists() and source.exists() and path.samefile(source))
        ):
            raise ValueError(f"output must not overwrite input or template {str(source)!r}")


def _write_atomic(path, document):
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(
            "w", encoding="utf-8", newline="\n", dir=path.parent,
            prefix=".PrettyPrint-", delete=False,
        ) as stream:
            temporary = Path(stream.name)
            stream.write(document)
        os.replace(temporary, path)
        temporary = None
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def main(argv=None):
    parser = argparse.ArgumentParser(
        description=__doc__, epilog=_HELP, allow_abbrev=False,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("input", metavar="INPUT.i", help="macro stream path, or - for stdin")
    parser.add_argument("--format", choices=("latex", "troff"), default="latex")
    parser.add_argument("--template", type=Path, help="custom document template")
    parser.add_argument("--title", help="document title (defaults to the input basename)")
    parser.add_argument("-o", "--out", default="-", help="output path (default: stdout)")
    args = parser.parse_args(argv)

    source_path = None if args.input == "-" else Path(args.input)
    destination = None if args.out == "-" else Path(args.out)
    title = args.title
    if title is None:
        title = source_path.name if source_path is not None else "Standard input"
    try:
        if len(title) > MAX_TITLE:
            raise ValueError(f"title exceeds {MAX_TITLE} characters")
        if destination is not None:
            _check_output(destination, (source_path, args.template))
        source = _read(source_path, MAX_INPUT, "input")
        template = (
            _DEFAULT_TEMPLATES[args.format] if args.template is None
            else _read(args.template, MAX_TEMPLATE, "template")
        )
        document = _render(source, args.format, template, title)
        if destination is None:
            # Keep the same UTF-8 bytes and LF newlines on stdout as on disk.
            sys.stdout.buffer.write(document.encode("utf-8"))
        else:
            _write_atomic(destination, document)
    except (OSError, RuntimeError, ValueError) as exc:
        parser.exit(1, f"{parser.prog}: error: {exc}\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
'''
