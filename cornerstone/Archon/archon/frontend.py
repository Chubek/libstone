"""Lexer and parser for Archon's declaration syntax."""

from bisect import bisect_right
from dataclasses import dataclass
import json
import re

from .diagnostics import ArchonError, Location


_IDENT = re.compile(r"[A-Za-z_][A-Za-z_0-9]*")
_NUMBER = re.compile(r"[0-9][A-Za-z_0-9]*")
_INTEGER = re.compile(r"(?:[0-9]+|0[xX][0-9a-fA-F]+|0[bB][01]+)")
_NEWLINE = re.compile(r"\r\n?|\n")
_SETTINGS = {"endian", "address_width", "description", "version"}
_BIT_TYPES = {"bits", "sint", "uint"}
_MAX_TEMPLATE_ARITY = 32
_MAX_TEMPLATE_INTEGER_DIGITS = 4096


@dataclass(frozen=True)
class _Token:
    kind: str
    text: str
    offset: int


class _Lexer:
    def __init__(self, source: str, filename: str):
        self.source = source
        self.filename = filename
        self.offset = 0
        self.line_starts = [0, *(m.end() for m in _NEWLINE.finditer(source))]

    def location(self, offset: int) -> Location:
        line = bisect_right(self.line_starts, offset)
        return Location(self.filename, line, offset - self.line_starts[line - 1] + 1)

    def _comment_end(self, start: int) -> int | None:
        if self.source.startswith(("//", "#"), start):
            newline = _NEWLINE.search(self.source, start)
            return newline.start() if newline else len(self.source)
        if self.source.startswith("/*", start):
            end = self.source.find("*/", start + 2)
            if end == -1:
                raise ArchonError("unterminated block comment", self.location(start))
            return end + 2
        return None

    def _string_end(self, start: int) -> int:
        quote = self.source[start]
        pos = start + 1
        while pos < len(self.source):
            char = self.source[pos]
            if char == quote:
                return pos + 1
            if char in "\r\n":
                break
            pos += 2 if char == "\\" else 1
        raise ArchonError("unterminated string literal", self.location(start))

    def next(self) -> _Token:
        source = self.source
        while self.offset < len(source):
            if source[self.offset] in " \t\r\n\f\v":
                self.offset += 1
                continue
            pos = self._comment_end(self.offset)
            if pos is None:
                break
            self.offset = pos

        start = self.offset
        if start == len(source):
            return _Token("eof", "", start)

        char = source[start]
        if match := _IDENT.match(source, start):
            kind, end = "identifier", match.end()
        elif match := _NUMBER.match(source, start):
            kind, end = "integer", match.end()
            if not _INTEGER.fullmatch(match[0]):
                raise ArchonError(f"invalid integer literal {match[0]!r}", self.location(start))
        elif char == '"':
            kind, end = "string", self._string_end(start)
        elif source.startswith("..", start):
            kind, end = "..", start + 2
        elif char in "{}[]()<>,:;.=@*+-":
            kind, end = char, start + 1
        else:
            raise ArchonError(f"unexpected character {char!r}", self.location(start))

        self.offset = end
        return _Token(kind, source[start:end], start)

    def raw_block(self, opening: int) -> dict:
        """Read a balanced opaque block after the already-lexed opening brace."""
        source = self.source
        start = pos = self.offset
        depth = 1
        while pos < len(source):
            end = self._comment_end(pos)
            if end is not None:
                pos = end
                continue
            char = source[pos]
            if char in "\"'":
                pos = self._string_end(pos)
                continue
            if char == "{":
                depth += 1
            elif char == "}":
                depth -= 1
                if depth == 0:
                    self.offset = pos + 1
                    return {"source": source[start:pos], "loc": self.location(start)}
            pos += 1
        raise ArchonError("unterminated semantics block", self.location(opening))


class _Parser:
    def __init__(self, source: str, filename: str):
        self.lexer = _Lexer(source, filename)
        self.current = self.lexer.next()

    def _loc(self, token: _Token) -> Location:
        return self.lexer.location(token.offset)

    def _error(self, message: str, token: _Token | None = None) -> ArchonError:
        return ArchonError(message, self._loc(token or self.current))

    def _advance(self) -> _Token:
        token = self.current
        self.current = self.lexer.next()
        return token

    def _accept(self, text: str) -> bool:
        if self.current.text != text:
            return False
        self._advance()
        return True

    def _expect(self, text: str) -> _Token:
        if self.current.text != text:
            raise self._expected(repr(text))
        return self._advance()

    def _expected(self, description: str) -> ArchonError:
        found = "end of file" if self.current.kind == "eof" else repr(self.current.text)
        return self._error(f"expected {description}, got {found}")

    def _identifier(self) -> _Token:
        if self.current.kind != "identifier":
            raise self._expected("an identifier")
        return self._advance()

    def _path(self) -> str:
        parts = [self._identifier().text]
        while self._accept("."):
            parts.append(self._identifier().text)
        return ".".join(parts)

    def _integer(self, *, max_digits: int | None = None) -> int:
        sign = -1 if self._accept("-") else 1
        if sign == 1:
            self._accept("+")
        if self.current.kind != "integer":
            raise self._expected("an integer")
        token = self._advance()
        base = 0 if token.text[:2].lower() in {"0x", "0b"} else 10
        if max_digits is not None and len(token.text) - (2 if base == 0 else 0) > max_digits:
            raise self._error(f"template integer literal exceeds {max_digits} digits", token)
        try:
            return sign * int(token.text, base)
        except ValueError:
            raise self._error("integer literal is too large", token) from None

    def _string(self) -> str:
        if self.current.kind != "string":
            raise self._expected("a string literal")
        token = self._advance()
        try:
            return json.loads(token.text)
        except json.JSONDecodeError as exc:
            loc = self.lexer.location(token.offset + exc.pos)
            raise ArchonError(f"invalid string literal: {exc.msg}", loc) from None

    def _number_or_name(self) -> int | str:
        if self.current.kind == "identifier":
            return self._identifier().text
        return self._integer()

    def _value(self) -> int | str:
        return self._string() if self.current.kind == "string" else self._number_or_name()

    def _type_spec(self) -> dict:
        name = self._identifier()
        if self._accept("<"):
            if name.text not in _BIT_TYPES:
                raise self._error(f"unsupported parameterized type {name.text!r}", name)
            arg = self._number_or_name()
            self._expect(">")
            return {"kind": name.text, "arg": arg}
        if name.text in _BIT_TYPES:
            raise self._expected(f"a width for {name.text!r}")
        return {"kind": "named", "arg": name.text}

    def _node(self, keyword: _Token, name: str, **properties) -> dict:
        return {"kind": keyword.text, "name": name, "loc": self._loc(keyword), **properties}

    def _member(self, allowed: set[str], context: str) -> _Token:
        if self.current.kind == "eof":
            raise self._expected(f"a {context} member or '}}'")
        if self.current.text not in allowed:
            raise self._error(f"unsupported {context} member {self.current.text!r}")
        return self._advance()

    def _unique(self, members, key: _Token, context: str) -> None:
        if key.text in members:
            raise self._error(f"duplicate {context} {key.text!r}", key)

    def parse(self) -> dict:
        kind = self.current
        if kind.text not in {"architecture", "module"}:
            raise self._expected("'architecture' or 'module'")
        self._advance()
        name = self._path() if kind.text == "module" else self._identifier().text
        self._expect("{")
        declarations = []
        while not self._accept("}"):
            declarations.append(self._declaration())
        if self.current.kind != "eof":
            raise self._expected("end of file")
        return self._node(kind, name, declarations=declarations)

    def _declaration(self) -> dict:
        keyword = self.current
        if keyword.text in _SETTINGS:
            self._advance()
            self._expect("=")
            value = self._value()
            self._expect(";")
            return {"kind": "setting", "name": keyword.text, "loc": self._loc(keyword), "value": value}
        if keyword.text == "import":
            self._advance()
            path = self._path()
            self._expect(";")
            return self._node(keyword, path, path=path)

        handlers = {
            "parameter": self._parameter,
            "feature": self._feature,
            "type": self._type,
            "register_file": self._register_file,
            "register_class": self._register_class,
            "state": self._state,
            "stack": self._stack,
            "memory": self._memory,
            "format": self._format,
            "instruction": self._instruction,
            "template": self._template,
            "def": self._definition,
        }
        if handler := handlers.get(keyword.text):
            self._advance()
            name = self._identifier().text
            return self._node(keyword, name, **handler(keyword))
        if keyword.kind == "eof":
            raise self._expected("a declaration or '}'")
        raise self._error(f"unsupported declaration {keyword.text!r}", keyword)

    def _template(self, keyword: _Token) -> dict:
        self._expect("<")
        parameters = []
        names = set()
        if not self._accept(">"):
            while True:
                if len(parameters) == _MAX_TEMPLATE_ARITY:
                    raise self._error(f"template may have at most {_MAX_TEMPLATE_ARITY} parameters")
                name = self._identifier()
                self._unique(names, name, "template parameter")
                names.add(name.text)
                self._expect(":")
                type_name = self._identifier()
                spec = {"kind": type_name.text}
                if type_name.text == "bits":
                    self._expect("<")
                    spec["width"] = self._integer(max_digits=_MAX_TEMPLATE_INTEGER_DIGITS)
                    self._expect(">")
                elif type_name.text not in {"nat", "string"}:
                    raise self._error("expected template parameter type 'string', 'nat', "
                                      f"or 'bits<N>', got {type_name.text!r}", type_name)
                parameters.append({"name": name.text, "spec": spec, "loc": self._loc(name)})
                if not self._accept(","):
                    break
            self._expect(">")
        self._expect("{")
        instruction = self._expect("instruction")
        self._expect("self")
        body = self._node(instruction, "self", **self._instruction(instruction, names))
        self._expect("}")
        return {"parameters": parameters, "body": body}

    def _definition(self, keyword: _Token) -> dict:
        self._expect("=")
        template = self._identifier()
        self._expect("<")
        arguments = []
        if not self._accept(">"):
            while True:
                if len(arguments) == _MAX_TEMPLATE_ARITY:
                    raise self._error(f"template application may have at most {_MAX_TEMPLATE_ARITY} arguments")
                loc = self._loc(self.current)
                if self.current.kind == "string":
                    value = self._string()
                elif self.current.kind in {"integer", "+", "-"}:
                    value = self._integer(max_digits=_MAX_TEMPLATE_INTEGER_DIGITS)
                else:
                    raise self._expected("a template argument (string or integer literal)")
                arguments.append({"value": value, "loc": loc})
                if not self._accept(","):
                    break
            self._expect(">")
        self._expect(";")
        return {"template": template.text, "template_loc": self._loc(template), "arguments": arguments}

    def _parameter(self, keyword: _Token) -> dict:
        self._expect(":")
        self._expect("nat")
        self._expect("in")
        self._expect("{")
        values = [self._integer()]
        while self._accept(","):
            values.append(self._integer())
        self._expect("}")
        self._expect("=")
        default = self._integer()
        self._expect(";")
        return {"values": values, "default": default}

    def _feature(self, keyword: _Token) -> dict:
        enabled = False
        explicit = False
        requires = []
        while self.current.text in {"=", "requires"}:
            clause = self._advance()
            if clause.text == "=":
                if explicit:
                    raise self._error("duplicate feature default", clause)
                if self.current.text not in {"enabled", "disabled"}:
                    raise self._expected("'enabled' or 'disabled'")
                enabled = self._advance().text == "enabled"
                explicit = True
            else:
                if requires:
                    raise self._error("duplicate feature requires clause", clause)
                while True:
                    dependency = self._identifier()
                    self._unique(requires, dependency, "feature dependency")
                    requires.append(dependency.text)
                    if not self._accept(","):
                        break
        self._expect(";")
        return {"enabled": enabled, "requires": requires}

    def _type(self, keyword: _Token) -> dict:
        self._expect("=")
        spec = self._type_spec()
        self._expect(";")
        return {"spec": spec}

    def _state(self, keyword: _Token) -> dict:
        """Parse one named scalar architectural-state cell."""
        self._expect(":")
        spec = self._type_spec()
        self._expect(";")
        return {"spec": spec}

    def _bounded_storage(self, keyword: _Token, member: str, context: str) -> int | str:
        self._expect("{")
        if self.current.text == "}":
            raise self._error(f"{context} requires {member!r}", keyword)
        key = self._member({member}, context)
        self._expect("=")
        value = self._number_or_name()
        self._expect(";")
        if self.current.text == member:
            raise self._error(f"duplicate {context} member {member!r}")
        if self.current.kind != "eof" and self.current.text != "}":
            raise self._error(f"unsupported {context} member {self.current.text!r}")
        self._expect("}")
        return value

    def _stack(self, keyword: _Token) -> dict:
        self._expect(":")
        spec = self._type_spec()
        capacity = self._bounded_storage(keyword, "capacity", "stack")
        return {"spec": spec, "capacity": capacity}

    def _memory(self, keyword: _Token) -> dict:
        return {"size": self._bounded_storage(keyword, "size", "memory")}

    def _register_file(self, keyword: _Token) -> dict:
        self._expect("{")
        values = {}
        aliases = {}
        targets = {}
        while not self._accept("}"):
            member = self._member({"count", "width", "zero", "register", "alias"}, "register_file")
            if member.text == "alias":
                name = self._identifier()
                self._unique(aliases, name, "register alias")
                self._expect("=")
                targets[name.text] = self._identifier()
                self._expect("[")
                aliases[name.text] = self._integer()
                self._expect("]")
            else:
                self._unique(values, member, "register_file member")
                if member.text == "register":
                    prefix = self._identifier().text
                    self._expect("[")
                    start = self._integer()
                    self._expect("..")
                    end = self._integer()
                    self._expect("]")
                    values["register"] = (prefix, start, end)
                else:
                    self._expect("=")
                    values[member.text] = (
                        self._number_or_name() if member.text == "width" else self._integer()
                    )
            self._expect(";")
        for required in ("count", "width", "register"):
            if required not in values:
                raise self._error(f"register_file requires {required!r}", keyword)
        prefix, start, end = values["register"]
        for target in targets.values():
            if target.text != prefix:
                raise self._error(f"expected register prefix {prefix!r} in alias target", target)
        return {
            "count": values["count"], "width": values["width"], "prefix": prefix,
            "start": start, "end": end, "zero": values.get("zero"), "aliases": aliases,
        }

    def _register_class(self, keyword: _Token) -> dict:
        self._expect("{")
        if self.current.text == "}":
            raise self._error("register_class requires 'members'", keyword)
        self._member({"members"}, "register_class")
        self._expect("=")
        file = self._identifier().text
        self._expect(".")
        self._expect("*")
        self._expect(";")
        if self.current.text == "members":
            raise self._error("duplicate register_class member 'members'")
        if self.current.kind != "eof" and self.current.text != "}":
            raise self._error(f"unsupported register_class member {self.current.text!r}")
        self._expect("}")
        return {"file": file}

    def _format(self, keyword: _Token) -> dict:
        self._expect(":")
        self._expect("bits")
        self._expect("<")
        width = self._integer()
        self._expect(">")
        self._expect("{")
        fields = []
        names = set()
        while not self._accept("}"):
            field = self._member({"field"}, "format")
            name = self._identifier()
            self._unique(names, name, "format field")
            names.add(name.text)
            self._expect(":")
            spec = self._type_spec()
            self._expect("@")
            self._expect("[")
            msb = self._integer()
            self._expect(":")
            lsb = self._integer()
            self._expect("]")
            self._expect(";")
            fields.append({"name": name.text, "spec": spec, "msb": msb, "lsb": lsb, "loc": self._loc(field)})
        return {"width": width, "fields": fields}

    def _operands(self) -> list[dict]:
        self._expect("{")
        operands = []
        names = set()
        while not self._accept("}"):
            direction = self._member({"in", "out", "inout"}, "operand")
            name = self._identifier()
            self._unique(names, name, "operand")
            names.add(name.text)
            self._expect(":")
            operand_type = self._identifier().text
            if operand_type == "imm":
                self._expect("<")
                operand_type = self._type_spec()
                self._expect(">")
            self._expect(";")
            operands.append({
                "name": name.text, "direction": direction.text, "type": operand_type,
                "loc": self._loc(direction),
            })
        return operands

    def _encoding(self, template_parameters: frozenset[str] | set[str]) -> dict:
        self._expect("{")
        encoding = {}
        while not self._accept("}"):
            field = self._identifier()
            self._unique(encoding, field, "encoding field")
            self._expect("=")
            if self.current.kind == "identifier":
                name = self._identifier()
                value = name.text
                if self._accept("("):
                    if value != "encode_register":
                        raise self._error(f"unsupported encoding call {value!r}", name)
                    value = self._identifier().text
                    self._expect(")")
                elif value in template_parameters:
                    value = {"parameter": value, "loc": self._loc(name)}
            else:
                value = self._integer()
            self._expect(";")
            encoding[field.text] = value
        return encoding

    def _assembly(self) -> str:
        if self._accept("{"):
            self._member({"syntax"}, "assembly")
            assembly = self._string()
            self._expect(";")
            if self.current.text == "syntax":
                raise self._error("duplicate assembly member 'syntax'")
            if self.current.kind != "eof" and self.current.text != "}":
                raise self._error(f"unsupported assembly member {self.current.text!r}")
            self._expect("}")
            return assembly
        assembly = self._string()
        self._expect(";")
        return assembly

    def _instruction(
        self, keyword: _Token, template_parameters: frozenset[str] | set[str] = frozenset(),
    ) -> dict:
        self._expect("{")
        blocks = {}
        requires = []
        while not self._accept("}"):
            member = self._member(
                {"operands", "requires", "encoding", "assembly", "semantics"}, "instruction"
            )
            if member.text == "requires":
                self._expect("feature")
                self._expect("(")
                feature = self._identifier()
                self._unique(requires, feature, "required instruction feature")
                requires.append(feature.text)
                self._expect(")")
                self._expect(";")
                continue
            self._unique(blocks, member, "instruction member")
            if member.text == "operands":
                blocks["operands"] = self._operands()
            elif member.text == "encoding":
                blocks["format"] = self._identifier().text
                blocks["encoding"] = self._encoding(template_parameters)
            elif member.text == "assembly":
                blocks["assembly"] = self._assembly()
            else:
                # Do not lex the block contents as declarations; ArchSem parses
                # the unchanged body with this source location as its origin.
                if self.current.text != "{":
                    raise self._expected("'{'")
                blocks["semantics"] = self.lexer.raw_block(self.current.offset)
                self.current = self.lexer.next()
        for required in ("encoding", "assembly"):
            if required not in blocks:
                raise self._error(f"instruction requires {required!r}", keyword)
        return {"operands": [], "semantics": None, "requires": requires, **blocks}


def parse(source: str, filename: str) -> dict:
    """Parse one architecture or module, raising ArchonError on invalid syntax."""
    return _Parser(source, filename).parse()
