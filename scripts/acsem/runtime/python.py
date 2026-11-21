#!/usr/bin/env python3
"""Standalone acsem @VERSION@ interpreter. Generated code: MIT-0."""
import sys
import re

MODEL = @MODEL@
MIN, MAX = -2147483648, 2147483647


class AcsemError(Exception):
    pass


def error(message):
    raise AcsemError('ACSEM_ERROR: ' + message)


def limit(message):
    raise AcsemError('ACSEM_LIMIT: ' + message)


def read_source(stream):
    source = stream.read(65537)
    if len(source) > 65536:
        limit('input exceeds 64 KiB')
    return source


class Runtime:
    def __init__(self, source):
        lines = iter(MODEL.splitlines())
        self.start, nc, nn = map(int, next(lines).split())
        self.ctors = []
        self.names = {}
        for i in range(nc):
            row = next(lines).split()
            self.names[row[0]] = i
            self.ctors.append((row[0], int(row[1]), list(map(int, row[4:])), int(row[3])))
        self.code = []
        for _ in range(nn):
            row = next(lines).split()
            self.code.append((*map(int, row[:4]), row[4]))
        self.cells, self.terms, self.symbols = [], [], set()
        self.steps = 0
        if len(source) > 65536:
            limit('source exceeds 64 KiB')
        self.source = source.decode('latin1')
        self.tokens = []
        pos = 0
        token = re.compile(r'[A-Za-z_][A-Za-z_0-9]*|[0-9]+|==|[{}(),;:=|+*/<\-]')
        while pos < len(self.source):
            if self.source[pos] in ' \t\r\n':
                pos += 1
                continue
            match = token.match(self.source, pos)
            if not match:
                self.syntax('NUL in source' if self.source[pos] == '\0' else 'invalid term syntax', pos)
            word = match.group()
            if len(word) > 64:
                limit('source token exceeds 64 bytes')
            self.tokens.append((word, pos))
            if len(self.tokens) > 20000:
                limit('too many tokens')
            pos = match.end()
        self.tokens.append(('<eof>', len(self.source)))
        self.pos = 0

    def syntax(self, message, at=None):
        if at is None:
            at = self.tokens[self.pos][1]
        before = self.source[:at]
        error('{} at {}:{}'.format(message, before.count('\n') + 1, at - before.rfind('\n')))

    def peek(self):
        return self.tokens[self.pos][0]

    def take(self):
        word = self.peek()
        self.pos += 1
        return word

    def expect(self, word):
        if self.peek() != word:
            self.syntax("expected '{}'".format(word))
        self.take()

    def ident(self):
        if not re.fullmatch('[A-Za-z_][A-Za-z_0-9]*', self.peek()):
            self.syntax('expected identifier')
        return self.take()

    def symbol(self, word):
        self.symbols.add(word)
        if len(self.symbols) > 256:
            limit('runtime symbol limit')
        return word

    def parse(self, sort, depth=1):
        if depth > 128:
            limit('parse depth exceeded')
        if len(self.terms) >= 2048:
            limit('program syntax node limit')
        index = len(self.terms)
        self.terms.append(None)
        name = self.ident()
        if name not in self.names:
            self.syntax('unknown constructor ' + name)
        ci = self.names[name]
        ctor = self.ctors[ci]
        if ctor[1] != sort:
            self.syntax('constructor ' + name + ' has wrong sort')
        self.expect('(')
        fields = []
        for i, ty in enumerate(ctor[2]):
            if i:
                self.expect(',')
            if ty == -1:
                sign = self.take() if self.peek() == '-' else ''
                if not re.fullmatch('[0-9]+', self.peek()):
                    self.syntax('integer field expected')
                text = sign + self.take()
                if not MIN <= int(text) <= MAX:
                    self.syntax('integer field outside 32-bit range')
                fields.append((1, int(text)))
            elif ty == -2:
                fields.append((2, self.symbol(self.ident())))
            else:
                fields.append((4, self.parse(ty, depth + 1)))
        self.expect(')')
        self.terms[index] = (ci, fields)
        return index

    def tick(self, depth):
        self.steps += 1
        if self.steps > 100000:
            limit('evaluation step budget exceeded')
        if depth > 128:
            limit('evaluation nesting exceeded')

    def expr(self, pc, term, inputs, env, depth):
        self.tick(depth)
        op, x, y, n, text = self.code[pc]
        if op == 1:
            return (1, n)
        if op == 2:
            return (2, self.symbol(text))
        if op == 3:
            return self.terms[term][1][n]
        if op == 4:
            return inputs[n] if n < len(inputs) else None
        a = self.expr(x, term, inputs, env, depth + 1)
        if op == 5:
            return env.get(a[1]) if a and a[0] == 2 else None
        if op == 6:
            return self.cells[a[1]] if a and a[0] == 3 and 0 <= a[1] < len(self.cells) else None
        b = self.expr(y, term, inputs, env, depth + 1)
        if not a or not b or a[0] != 1 or b[0] != 1:
            return None
        a, b = a[1], b[1]
        if op == 7:
            v = a + b
        elif op == 8:
            v = a - b
        elif op == 9:
            v = a * b
        elif op == 10:
            if b == 0 or (a == MIN and b == -1):
                return None
            v = (abs(a) // abs(b)) * (-1 if (a < 0) != (b < 0) else 1)
        elif op == 11:
            v = int(a == b)
        else:
            v = int(a < b)
        return (1, v) if MIN <= v <= MAX else None

    @staticmethod
    def result(status=0, out=None, bindings=None):
        out, bindings = out or [], bindings or {}
        if len(out) > 64:
            limit('transient result capacity')
        if len(bindings) > 64:
            limit('binding result capacity')
        return status, out, bindings

    def action(self, pc, term, inputs, env, depth):
        self.tick(depth)
        op, x, y, n, _ = self.code[pc]
        if op == 13:
            return self.result()
        if op == 14:
            return self.result(1)
        if op in (15, 16, 17):
            v = self.expr(x, term, inputs, env, depth + 1)
            if v is None:
                return self.result(1)
            if op == 16:
                return self.result(0 if v[0] == 1 and v[1] else 1)
            return self.result(2 if op == 17 else 0, [v])
        if op == 19:
            if len(self.cells) >= 1024:
                limit('allocated cell limit')
            self.cells.append(None)
            return self.result(out=[(3, len(self.cells) - 1)])
        if op in (18, 20):
            a = self.expr(x, term, inputs, env, depth + 1)
            b = self.expr(y, term, inputs, env, depth + 1)
            if op == 18:
                return self.result(bindings={a[1]: b}) if a and a[0] == 2 and b else self.result(1)
            if not a or a[0] != 1 or not b or b[0] != 3 or not 0 <= b[1] < len(self.cells):
                return self.result(1)
            self.cells[b[1]] = a
            return self.result()
        if op == 21:
            return self.node(self.terms[term][1][n][1], inputs, env, depth + 1)
        left = self.action(x, term, inputs, env, depth + 1)
        status, out, bindings = left
        if op == 22:
            return self.result(0, out, dict(env, **bindings)) if status == 0 else left
        if op == 26:
            return self.action(y, term, inputs, env, depth + 1) if status == 1 else left
        if op == 27:
            return self.action(y, term, out, env, depth + 1) if status == 2 else left
        if status:
            return left
        right = self.action(y, term, out if op == 23 else inputs,
                            bindings if op == 25 else env, depth + 1)
        if right[0]:
            return right
        if op == 25:
            merged = right[2]
        else:
            if bindings.keys() & right[2].keys():
                return self.result(1)
            merged = dict(bindings, **right[2])
        return self.result(0, right[1] if op == 23 else out + right[1], merged)

    def node(self, term, inputs, env, depth):
        self.tick(depth)
        return self.action(self.ctors[self.terms[term][0]][3], term, inputs, env, depth + 1)

    def run(self):
        root = self.parse(self.start)
        if self.peek() != '<eof>':
            self.syntax('trailing term input')
        status, out, _ = self.node(root, [], {}, 1)
        text = ('completed', 'failed', 'escaped')[status]
        for ty, value in out:
            text += ' ' + ('token:' if ty == 2 else 'cell:' if ty == 3 else '') + str(value)
        return text + '\n', int(status != 0)


def main(args):
    term = path = output = None
    i = 0
    while i < len(args):
        flag, sep, value = args[i].partition('=')
        if flag in ('--help', '-h') and not sep:
            print('generated interpreter [--term PROGRAM | --term-file PATH] [-o FILE]\n'
                  'Exit: completed=0, failed/escaped=1, errors=2.')
            return 0
        if flag == '--version' and not sep:
            print('acsem @VERSION@')
            return 0
        if flag == '--run' and not sep:
            i += 1
            continue
        if flag not in ('--term', '--term-file', '-o', '--output'):
            error('invalid option ' + args[i])
        if not sep:
            i += 1
            if i == len(args):
                error('missing value for ' + flag)
            value = args[i]
        if flag == '--term':
            term = value
        elif flag == '--term-file':
            path = value
        else:
            output = value
        i += 1
    if term is not None and path is not None:
        error('use only one of --term and --term-file')
    if term is not None:
        source = term.encode('utf-8')
    elif path is not None:
        with open(path, 'rb') as stream:
            source = read_source(stream)
    else:
        source = read_source(sys.stdin.buffer)
    text, code = Runtime(source).run()
    if output is not None and output != '-':
        with open(output, 'w', encoding='ascii', newline='\n') as stream:
            stream.write(text)
    else:
        sys.stdout.write(text)
        sys.stdout.flush()
    return code


if __name__ == '__main__':
    try:
        sys.exit(main(sys.argv[1:]))
    except (AcsemError, OSError, UnicodeError) as exc:
        print(str(exc) if isinstance(exc, AcsemError) else 'ACSEM_ERROR: ' + str(exc), file=sys.stderr)
        sys.exit(2)
