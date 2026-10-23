"""Reader for Yellowstone .isa files (grammar in isa.peg)."""
from dataclasses import dataclass
from pathlib import Path
import re

@dataclass
class ISANode:
    kind: str
    name: str = ''
    value: object = None
    fields: list = None
    line: int = 0
    column: int = 0
    def __post_init__(self):
        if self.fields is None: self.fields=[]

class _Parser:
    tok=re.compile(r'''\s+|#[^\n]*(?:\n|$)|"(?:\\.|[^"\\])*"|0x[0-9A-Fa-f]+|-?[0-9]+|[A-Za-z_][A-Za-z0-9_.-]*|[][{}()=;,,:]''')
    def __init__(self,s,source=''): self.s=s; self.source=source; self.t=[]; self.i=0; self._lex()
    def _lex(self):
        p=0; line=1; col=1
        while p<len(self.s):
            m=self.tok.match(self.s,p)
            if not m: raise SyntaxError(f'{self.source}:{line}:{col}: unexpected {self.s[p]!r}')
            z=m.group(); p=m.end()
            if z.isspace() or z.startswith('#'):
                n=z.count('\n'); line+=n; col=(len(z.rsplit('\n',1)[-1])+1 if n else col+len(z)); continue
            self.t.append((z,line,col)); col+=len(z)
        self.t.append(('<eof>',line,col))
    def peek(self): return self.t[self.i][0]
    def pop(self,x=None):
        z=self.t[self.i]
        if x is not None and z[0]!=x: raise SyntaxError(f'{self.source}:{z[1]}:{z[2]}: expected {x}, got {z[0]}')
        self.i+=1; return z
    def value(self):
        z,l,c=self.t[self.i]
        if z=='[':
            self.pop(); a=[]
            while self.peek()!=']':
                a.append(self.value())
                if self.peek()==',': self.pop()
                elif self.peek()!=']': raise SyntaxError('expected comma')
            self.pop(']'); return a
        if z=='{':
            self.pop(); a=[]
            while self.peek()!='}': a.append(self.field())
            self.pop('}'); return a
        if z=='"': pass
        if z.startswith('"'):
            self.pop(); return bytes(z[1:-1],'utf8').decode('unicode_escape')
        if z=='true' or z=='false': self.pop(); return z=='true'
        if re.fullmatch(r'-?[0-9]+',z): self.pop(); return int(z)
        if z.startswith('0x'): self.pop(); return int(z,16)
        # bare/call list; retain as string or structured call
        self.pop();
        if self.peek()=='(':
            self.pop(); args=[]
            while self.peek()!=')':
                args.append(self.value());
                if self.peek()==',': self.pop()
            self.pop(')'); return ISANode('call',z,value=args)
        return z
    def field(self):
        z,l,c=self.pop();
        if self.peek()==':':
            parts=[]
            while self.peek()!=';': parts.append(self.pop()[0])
            self.pop(';'); return ISANode('field',z,' '.join(parts),line=l,column=c)
        if self.peek()=='{':
            self.pop(); fs=[]
            while self.peek()!='}': fs.append(self.field())
            self.pop('}'); return ISANode('section',z,fields=fs,line=l,column=c)
        self.pop('=');
        if self.t[self.i+1][0]==':':
            parts=[]
            while self.peek()!=';': parts.append(self.pop()[0])
            self.pop(';'); return ISANode('field',z,' '.join(parts),line=l,column=c)
        v=self.value();
        while self.peek()==',': self.pop(); v = [v, self.value()] if not isinstance(v,list) else v+[self.value()]
        self.pop(';'); return ISANode('field',z,v,line=l,column=c)
    def parse(self):
        out=[]
        while self.peek()!='<eof>':
            z,l,c=self.pop()
            if z in ('arch','profile','compiler','legacy'):
                name='';
                if self.peek()!='{': name=self.pop()[0]
                self.pop('{'); fs=[]
                while self.peek()!='}': fs.append(self.field() if self.peek() not in ('regclass','encoding','op') else self.decl())
                self.pop('}'); out.append(ISANode(z,name,fields=fs,line=l,column=c))
            else:
                self.i-=1; out.append(self.decl())
        return ISANode('document',fields=out)
    def decl(self):
        z,l,c=self.pop(); name=self.pop()[0]; self.pop('{'); fs=[]
        while self.peek()!='}':
            if z=='regclass' and self.peek() not in ('}',):
                n,ln,cn=self.pop(); self.pop('('); width=self.pop()[0]; self.pop(')'); self.pop('='); num=self.value(); self.pop(','); fs.append(ISANode('register',n,[int(width),num],line=ln,column=cn))
            else: fs.append(self.field())
        self.pop('}'); return ISANode(z,name,fields=fs,line=l,column=c)

class ISAVisitor:
    def visit(self,node):
        m=getattr(self,'visit_'+node.kind,None)
        if m: m(node)
        for x in node.fields or []:
            if isinstance(x,ISANode): self.visit(x)
    def visit_document(self,node): pass

class ISAEventStream:
    def __init__(self, path): self.path=Path(path)
    def __iter__(self):
        root=_Parser(self.path.read_text(),str(self.path)).parse()
        yield from self._events(root)
    def _events(self,n):
        yield ('enter',n)
        for f in n.fields or []:
            if isinstance(f,ISANode): yield from self._events(f)
        yield ('exit',n)

def read(path): return _Parser(Path(path).read_text(),str(path)).parse()
def read_all(directory): return {p.stem:read(p) for p in sorted(Path(directory).glob('*.isa'))}
