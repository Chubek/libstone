#!/usr/bin/env python3
"""Reference Cephyr QML v1.1 expression-MIR pass runner (Python 3 stdlib only).

This tests the QML semantics independently of the LLSynth-generated target.
It does NOT transform native Cephyr/Archon MachineIR.
"""
import argparse
import json
from pathlib import Path
import re
import sys

TOKEN = re.compile(r"\s+|\#[^\n]*|=>|[A-Za-z_][A-Za-z_0-9]*|-?[0-9]+|[?:;,()]|.", re.S)

class SyntaxErrorQML(Exception):
    pass

class Reader:
    def __init__(self, source):
        self.tokens = [(m.group(), m.start()) for m in TOKEN.finditer(source)
                       if not m.group().isspace() and not m.group().startswith('#')]
        self.index = 0
    def peek(self):
        return self.tokens[self.index][0] if self.index < len(self.tokens) else '<EOF>'
    def take(self):
        if self.index >= len(self.tokens):
            raise SyntaxErrorQML('unexpected end of input')
        result = self.tokens[self.index][0]
        self.index += 1
        return result
    def expect(self, value):
        found = self.take()
        if found != value:
            raise SyntaxErrorQML(f'expected {value!r}, found {found!r}')
    def ident(self):
        value = self.take()
        if not re.fullmatch(r'[A-Za-z_][A-Za-z_0-9]*', value):
            raise SyntaxErrorQML(f'expected identifier, got {value!r}')
        return value
    def tree(self):
        t = self.take()
        if t == '?':
            return ('meta', self.ident())
        if t == '_':
            return ('wild',)
        if t == 'true':
            return ('bool', True)
        if t == 'false':
            return ('bool', False)
        if re.fullmatch(r'-?[0-9]+', t):
            value = int(t)
            if not -(2**63) <= value <= 2**63 - 1:
                raise SyntaxErrorQML('integer outside Int64 range')
            return ('int', value)
        if not re.fullmatch(r'[A-Za-z_][A-Za-z_0-9]*', t):
            raise SyntaxErrorQML(f'unexpected term {t!r}')
        if self.peek() == '(':
            self.take()
            args = []
            if self.peek() != ')':
                args.append(self.tree())
                while self.peek() == ',':
                    self.take()
                    args.append(self.tree())
            self.expect(')')
            return ('node', t, tuple(args))
        return ('sym',t)
    def program(self):
        ds = []
        while self.peek() != 'run':
            k = self.take()
            if k == 'rewrite':
                phase,name=self.ident(),self.ident()
                self.expect(':')
                lhs=self.tree()
                self.expect('=>')
                rhs=self.tree()
                self.expect(';')
                ds.append(('rewrite',phase,name,lhs,rhs))
            elif k == 'reject':
                phase,name=self.ident(),self.ident()
                self.expect(':')
                pattern=self.tree()
                self.expect(';')
                ds.append(('reject',phase,name,pattern))
            elif k == 'pass':
                phase=self.ident()
                self.expect(';')
                ds.append(('pass',phase))
            else:
                raise SyntaxErrorQML(f'unsupported QML declaration {k!r}')
        self.expect('run')
        tree = self.tree()
        self.expect(';')
        if self.peek() != '<EOF>':
            raise SyntaxErrorQML(f'trailing source after run: {self.peek()!r}')
        return ds,tree

def parse(source):
    return Reader(source).program()

def n(op,*args):return ('node',op,args)
def integer(x):
    if not -(1 << 63) <= x <= ((1 << 63) - 1):
        raise RuntimeError('Int64 overflow')
    return ('int',x)
def boolean(x):return ('bool',x)
def meta(x):return ('meta',x)
def sym(x):return ('sym',x)
def rule(p,nm,a,b):return ('rewrite',p,nm,a,b)
x=meta('x');y=meta('y')
BUILTINS=[
 rule('desugar','sub',n('sub',x,y),n('add',x,n('neg',y))),
 rule('desugar','and',n('and',x,y),n('if',x,y,boolean(False))),
 rule('desugar','or',n('or',x,y),n('if',x,boolean(True),y)),
 rule('desugar','not',n('not',x),n('if',x,boolean(False),boolean(True))),
 rule('canonicalize','addzero_r',n('add',x,integer(0)),x),
 rule('canonicalize','addzero_l',n('add',integer(0),x),x),
 rule('canonicalize','mulone_r',n('mul',x,integer(1)),x),
 rule('canonicalize','mulone_l',n('mul',integer(1),x),x),
 rule('canonicalize','negneg',n('neg',n('neg',x)),x),
 rule('canonicalize','iftrue',n('if',boolean(True),x,y),x),
 rule('canonicalize','iffalse',n('if',boolean(False),x,y),y),
]

def match(p,v,bindings):
    if p[0]=='wild':return True
    if p[0]=='meta':
        if p[1] in bindings: return bindings[p[1]]==v
        bindings[p[1]]=v;return True
    if p[0]!=v[0]:return False
    if p[0]=='node':
        if p[1]!=v[1] or len(p[2])!=len(v[2]):return False
        return all(match(a,b,bindings) for a,b in zip(p[2],v[2]))
    return p==v

def instantiate(expr,b):
    if expr[0]=='meta':return b[expr[1]]
    if expr[0]=='node':return n(expr[1],*(instantiate(ch,b) for ch in expr[2]))
    if expr[0]=='wild':raise RuntimeError('wildcard in replacement')
    return expr

def compile_decision(pattern, path, yes, no):
    kind = pattern[0]
    if kind == 'wild': return yes
    if kind == 'meta': return ('bind', path, pattern[1], yes, no)
    if kind == 'node':
        children = pattern[2]
        body = yes
        for i in range(len(children)-1, -1, -1):
            body = compile_decision(children[i],path+(i,),body,no)
        return ('test',path,('node', pattern[1],len(children)),body,no)
    return ('test',path,pattern,yes,no)

def execute_decision(decision,root,env):
    kind = decision[0]
    if kind == 'fail': return None
    if kind == 'leaf': return instantiate(decision[1],env)
    target = root
    try:
        for i in decision[1]: target=target[2][i]
    except (IndexError,TypeError): return None
    if kind == 'test':
        pattern=decision[2]
        if pattern[0]=='node':
            success=(target[0]=='node' and target[1]==pattern[1] and len(target[2])==pattern[2])
        else: success=(target==pattern)
        return execute_decision(decision[3] if success else decision[4],root,env)
    if kind == 'bind':
        label=decision[2]
        if label in env:
            return execute_decision(decision[3] if env[label]==target else decision[4],root,env)
        new_env={**env,label:target}
        return execute_decision(decision[3],root,new_env)
    raise RuntimeError('unknown decision instruction: '+kind)

def compile_rule(r):
    return (r[1],r[2],compile_decision(r[3],(),('leaf',r[4]),('fail',)))

def try_rule(r,term):
    return execute_decision(r[2],term,{})

def validate_rule(r):
    names=set()
    def collect(t):
        if t[0]=='meta': names.add(t[1])
        elif t[0]=='node':
            for c in t[2]:collect(c)
    def verify(t):
        if t[0]=='wild':raise RuntimeError('wildcard in replacement')
        if t[0]=='meta' and t[1] not in names:raise RuntimeError('unbound replacement metavariable '+t[1])
        if t[0]=='node':
            for c in t[2]:verify(c)
    collect(r[3]);verify(r[4])

def fold(t,p):
    if p!='optimize' or t[0]!='node':return t
    op,args=t[1],t[2]
    if op=='neg' and len(args)==1 and args[0][0]=='int':return integer(-args[0][1])
    if len(args)==2:
        a,b=args
        if a[0]=='int' and b[0]=='int':
            u,v=a[1],b[1]
            if op=='add':return integer(u+v)
            if op=='sub':return integer(u-v)
            if op=='mul':return integer(u*v)
            if op=='div' and v==0:raise RuntimeError('DivisionByZero')
            if op=='eq':return boolean(u==v)
            if op=='lt':return boolean(u<v)
            if op=='gt':return boolean(u>v)
        if a[0]=='bool' and b[0]=='bool' and op=='eq':return boolean(a==b)
    return t

def rewrite_visit(t,rules,p):
    if t[0]=='node':t=n(t[1],*(rewrite_visit(child,rules,p) for child in t[2]))
    for r in rules:
        if r[0]==p:
            result=try_rule(r,t)
            if result is not None:return result
    return fold(t,p)

def rewrite_fixpoint(t,rules,p):
    for i in range(128):
        nxt=rewrite_visit(t,rules,p)
        if nxt==t:return t
        t=nxt
    raise RuntimeError('RewriteFuel: '+p)

def check_rejections(t,checks,p):
    for r in checks:
        if r[0]==p and execute_decision(r[2],t,{}) is not None:
            raise RuntimeError('Rejected: '+r[1])
    if t[0]=='node':
        for ch in t[2]:check_rejections(ch,checks,p)

def infer(t,env=None):
    env=env or {}
    if t[0]=='int':return 'Int'
    if t[0]=='bool':return 'Bool'
    if t[0]!='node':raise RuntimeError('BadIR: '+t[0])
    op,args=t[1:]
    def arity(k):
        if len(args)!=k:raise RuntimeError('BadArity '+op)
    def expect(got,want):
        if got!=want:raise RuntimeError('TypeMismatch expected '+want+' got '+got)
    if op=='var':
        arity(1)
        if args[0][0]!='sym':raise RuntimeError('BadIR: variable name')
        if args[0][1] not in env:raise RuntimeError('Unbound '+args[0][1])
        return env[args[0][1]]
    if op=='let':
        arity(3)
        if args[0][0]!='sym':raise RuntimeError('BadIR: binding name')
        t=infer(args[1],env)
        return infer(args[2],{**env,args[0][1]:t})
    if op=='seq':
        arity(2);infer(args[0],env);return infer(args[1],env)
    if op=='if':
        arity(3);expect(infer(args[0],env),'Bool');yt=infer(args[1],env);expect(infer(args[2],env),yt);return yt
    if op in ('neg','not'):
        arity(1);want='Int' if op=='neg' else 'Bool';expect(infer(args[0],env),want);return want
    if op in ('add','sub','mul','div','lt','gt'):
        arity(2)
        for a in args:expect(infer(a,env),'Int')
        if op=='div' and args[1]==integer(0):raise RuntimeError('DivisionByZero')
        return 'Bool' if op in ('lt','gt') else 'Int'
    if op=='eq':
        arity(2);a=infer(args[0],env);expect(infer(args[1],env),a);return 'Bool'
    if op in ('and','or'):
        arity(2)
        for a in args:expect(infer(a,env),'Bool')
        return 'Bool'
    raise RuntimeError('BadIR op: '+op)

def execute(directives,tree):
    raw_rules=[d for d in directives if d[0]=='rewrite']+BUILTINS
    for r in raw_rules: validate_rule(r)
    rules=[compile_rule(r) for r in raw_rules]
    checks=[(d[1],d[2],compile_decision(d[3],(),('leaf', ('bool',True)),('fail',)))
            for d in directives if d[0]=='reject']
    passes=[d[1] for d in directives if d[0]=='pass'] or ['desugar','sema','canonicalize','optimize']
    for p in passes:
        if p=='sema':check_rejections(tree,checks,p);infer(tree)
        else:tree=rewrite_fixpoint(tree,rules,p);check_rejections(tree,checks,p)
    check_rejections(tree,checks,'sema');type_name=infer(tree)
    return tree,type_name

def render(t):
    if t[0]=='int':return str(t[1])
    if t[0]=='bool':return str(t[1]).lower()
    if t[0]=='sym':return t[1]
    if t[0]=='meta':return '?'+t[1]
    if t[0]=='wild':return '_'
    return t[1]+'('+', '.join(render(c) for c in t[2])+')'



def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('input', type=Path, help='complete bundled QML program')
    ap.add_argument('--json', action='store_true', help='print normalized tagged IR as JSON')
    ap.add_argument('--check', action='store_true', help='only parse and validate rewrite templates')
    args = ap.parse_args()
    try:
        directives,term=parse(args.input.read_text(encoding='utf-8'))
        if args.check:
            for d in directives:
                if d[0]=='rewrite':validate_rule(d)
            print(f'OK: {len(directives)} QML declarations')
            return 0
        result,typ=execute(directives,term)
        if args.json:
            print(json.dumps({'type':typ,'ir':result},indent=2))
        else:
            print(f'{render(result)} : {typ}')
        return 0
    except (RuntimeError,SyntaxErrorQML,ValueError) as exc:
        print(f'QML ERROR: {exc}',file=sys.stderr)
        return 1

if __name__ == '__main__':
    raise SystemExit(main())
