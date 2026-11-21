// Standalone acsem @VERSION@ interpreter, Rust 2021, standard library only. MIT-0.
use std::collections::{BTreeMap, BTreeSet};
use std::io::{self, Read, Write};

const MODEL: &str = @MODEL@;
type Checked<T> = Result<T, String>;
type Env = BTreeMap<String, Value>;
fn error(s: &str) -> String { format!("ACSEM_ERROR: {}", s) }
fn limit(s: &str) -> String { format!("ACSEM_LIMIT: {}", s) }

#[derive(Clone)]
enum Value { Integer(i64), Token(String), Cell(usize), Syntax(usize) }
struct Constructor { name: String, sort: i32, fields: Vec<i32>, rule: usize }
#[derive(Clone)]
struct Instruction { op: i32, x: usize, y: usize, n: i64, text: String }
struct Term { ctor: usize, fields: Vec<Value> }
struct Token { text: String, kind: u8, at: usize }
struct Outcome { status: u8, out: Vec<Value>, bindings: Env }
impl Outcome {
    fn new(status: u8, out: Vec<Value>, bindings: Env) -> Checked<Self> {
        if out.len() > 64 { return Err(limit("transient result capacity")); }
        if bindings.len() > 64 { return Err(limit("binding result capacity")); }
        Ok(Self { status, out, bindings })
    }
    fn empty(status: u8) -> Checked<Self> { Self::new(status, vec![], Env::new()) }
}
struct Runtime {
    start: i32, ctors: Vec<Constructor>, code: Vec<Instruction>, terms: Vec<Term>,
    cells: Vec<Option<Value>>, symbols: BTreeSet<String>, source: Vec<u8>,
    tokens: Vec<Token>, pos: usize, steps: usize,
}
impl Runtime {
    fn new(source: Vec<u8>) -> Checked<Self> {
        if source.len() > 65536 { return Err(limit("source exceeds 64 KiB")); }
        let mut rows = MODEL.lines();
        let head: Vec<usize> = rows.next().unwrap().split_whitespace().map(|s| s.parse().unwrap()).collect();
        let mut ctors = Vec::new();
        for _ in 0..head[1] {
            let row: Vec<&str> = rows.next().unwrap().split_whitespace().collect();
            ctors.push(Constructor { name: row[0].into(), sort: row[1].parse().unwrap(),
                fields: row[4..].iter().map(|s| s.parse().unwrap()).collect(), rule: row[3].parse().unwrap() });
        }
        let mut code = Vec::new();
        for _ in 0..head[2] {
            let row: Vec<&str> = rows.next().unwrap().split_whitespace().collect();
            code.push(Instruction { op: row[0].parse().unwrap(),
                x: row[1].parse::<i32>().unwrap() as usize, y: row[2].parse::<i32>().unwrap() as usize,
                n: row[3].parse().unwrap(), text: row[4].into() });
        }
        let mut rt = Self { start: head[0] as i32, ctors, code, terms: vec![], cells: vec![],
            symbols: BTreeSet::new(), source, tokens: vec![], pos: 0, steps: 0 };
        rt.lex()?;
        Ok(rt)
    }
    fn syntax_at(&self, message: &str, at: usize) -> String {
        let before = &self.source[..at];
        let line = before.iter().filter(|&&c| c == b'\n').count() + 1;
        let col = at - before.iter().rposition(|&c| c == b'\n').map(|p| p + 1).unwrap_or(0) + 1;
        error(&format!("{} at {}:{}", message, line, col))
    }
    fn syntax(&self, message: &str) -> String { self.syntax_at(message, self.tokens[self.pos].at) }
    fn lex(&mut self) -> Checked<()> {
        let mut at = 0;
        while at < self.source.len() {
            let c = self.source[at];
            if b" \t\r\n".contains(&c) { at += 1; continue; }
            let begin = at;
            let kind;
            if c.is_ascii_alphabetic() || c == b'_' {
                kind = 1; at += 1;
                while at < self.source.len() && (self.source[at].is_ascii_alphanumeric() || self.source[at] == b'_') { at += 1; }
            } else if c.is_ascii_digit() {
                kind = 2; at += 1;
                while at < self.source.len() && self.source[at].is_ascii_digit() { at += 1; }
            } else if b"{}(),;:=|+*/-<".contains(&c) {
                kind = 3; at += 1;
                if c == b'=' && at < self.source.len() && self.source[at] == b'=' { at += 1; }
            } else { return Err(self.syntax_at(if c == 0 { "NUL in source" } else { "invalid term syntax" }, at)); }
            if at - begin > 64 { return Err(limit("source token exceeds 64 bytes")); }
            if self.tokens.len() == 20000 { return Err(limit("too many tokens")); }
            self.tokens.push(Token { text: String::from_utf8(self.source[begin..at].to_vec()).unwrap(), kind, at: begin });
        }
        self.tokens.push(Token { text: "<eof>".into(), kind: 0, at: self.source.len() });
        Ok(())
    }
    fn eat(&mut self, text: &str) -> bool {
        if self.tokens[self.pos].text != text { return false; }
        self.pos += 1; true
    }
    fn expect(&mut self, text: &str) -> Checked<()> {
        if self.eat(text) { Ok(()) } else { Err(self.syntax(&format!("expected '{}'", text))) }
    }
    fn ident(&mut self) -> Checked<String> {
        if self.tokens[self.pos].kind != 1 { return Err(self.syntax("expected identifier")); }
        let text = self.tokens[self.pos].text.clone(); self.pos += 1; Ok(text)
    }
    fn symbol(&mut self, text: &str) -> Checked<Value> {
        self.symbols.insert(text.into());
        if self.symbols.len() > 256 { return Err(limit("runtime symbol limit")); }
        Ok(Value::Token(text.into()))
    }
    fn parse(&mut self, sort: i32, depth: usize) -> Checked<usize> {
        if depth > 128 { return Err(limit("parse depth exceeded")); }
        if self.terms.len() == 2048 { return Err(limit("program syntax node limit")); }
        let index = self.terms.len();
        self.terms.push(Term { ctor: 0, fields: vec![] });
        let name = self.ident()?;
        let ci = self.ctors.iter().position(|c| c.name == name).ok_or_else(|| self.syntax("unknown constructor"))?;
        if self.ctors[ci].sort != sort { return Err(self.syntax("constructor has wrong sort")); }
        self.expect("(")?;
        let mut fields = Vec::new();
        for (i, ty) in self.ctors[ci].fields.clone().into_iter().enumerate() {
            if i != 0 { self.expect(",")?; }
            let v = if ty == -1 {
                let neg = self.eat("-");
                if self.tokens[self.pos].kind != 2 { return Err(self.syntax("integer field expected")); }
                let text = format!("{}{}", if neg { "-" } else { "" }, self.tokens[self.pos].text);
                self.pos += 1;
                let n: i64 = text.parse().map_err(|_| self.syntax("integer field outside 32-bit range"))?;
                if n < i32::MIN as i64 || n > i32::MAX as i64 { return Err(self.syntax("integer field outside 32-bit range")); }
                Value::Integer(n)
            } else if ty == -2 {
                let name = self.ident()?; self.symbol(&name)?
            } else { Value::Syntax(self.parse(ty, depth + 1)?) };
            fields.push(v);
        }
        self.expect(")")?;
        self.terms[index] = Term { ctor: ci, fields }; Ok(index)
    }
    fn tick(&mut self, depth: usize) -> Checked<()> {
        self.steps += 1;
        if self.steps > 100000 { return Err(limit("evaluation step budget exceeded")); }
        if depth > 128 { return Err(limit("evaluation nesting exceeded")); }
        Ok(())
    }
    fn expr(&mut self, pc: usize, term: usize, inputs: &[Value], env: &Env, depth: usize) -> Checked<Option<Value>> {
        self.tick(depth)?;
        let e = self.code[pc].clone();
        match e.op {
            1 => return Ok(Some(Value::Integer(e.n))),
            2 => return Ok(Some(self.symbol(&e.text)?)),
            3 => return Ok(Some(self.terms[term].fields[e.n as usize].clone())),
            4 => return Ok(inputs.get(e.n as usize).cloned()),
            _ => ()
        }
        let a = self.expr(e.x, term, inputs, env, depth + 1)?;
        if e.op == 5 { return Ok(if let Some(Value::Token(key)) = a { env.get(&key).cloned() } else { None }); }
        if e.op == 6 { return Ok(if let Some(Value::Cell(cell)) = a { self.cells.get(cell).cloned().flatten() } else { None }); }
        let b = self.expr(e.y, term, inputs, env, depth + 1)?;
        if let (Some(Value::Integer(a)), Some(Value::Integer(b))) = (a, b) {
            let n = match e.op {
                7 => a + b, 8 => a - b, 9 => a * b,
                10 => { if b == 0 || (a == i32::MIN as i64 && b == -1) { return Ok(None); } a / b },
                11 => i64::from(a == b), 12 => i64::from(a < b),
                _ => return Err(error("invalid expression instruction"))
            };
            return Ok(if n < i32::MIN as i64 || n > i32::MAX as i64 { None } else { Some(Value::Integer(n)) });
        }
        Ok(None)
    }
    fn action(&mut self, pc: usize, term: usize, inputs: &[Value], env: &Env, depth: usize) -> Checked<Outcome> {
        self.tick(depth)?;
        let a = self.code[pc].clone();
        if a.op == 13 { return Outcome::empty(0); }
        if a.op == 14 { return Outcome::empty(1); }
        if (15..=17).contains(&a.op) {
            return match self.expr(a.x, term, inputs, env, depth + 1)? {
                None => Outcome::empty(1),
                Some(v) => {
                    if a.op == 16 { Outcome::empty(if matches!(v, Value::Integer(n) if n != 0) { 0 } else { 1 }) }
                    else { Outcome::new(if a.op == 17 { 2 } else { 0 }, vec![v], Env::new()) }
                }
            };
        }
        if a.op == 19 {
            if self.cells.len() == 1024 { return Err(limit("allocated cell limit")); }
            let cell = self.cells.len(); self.cells.push(None);
            return Outcome::new(0, vec![Value::Cell(cell)], Env::new());
        }
        if a.op == 18 || a.op == 20 {
            let v = self.expr(a.x, term, inputs, env, depth + 1)?;
            let w = self.expr(a.y, term, inputs, env, depth + 1)?;
            if a.op == 18 {
                return if let (Some(Value::Token(key)), Some(w)) = (v, w) {
                    let mut bindings = Env::new(); bindings.insert(key, w); Outcome::new(0, vec![], bindings)
                } else { Outcome::empty(1) };
            }
            if let (Some(Value::Integer(n)), Some(Value::Cell(cell))) = (v, w) {
                if cell < self.cells.len() { self.cells[cell] = Some(Value::Integer(n)); return Outcome::empty(0); }
            }
            return Outcome::empty(1);
        }
        if a.op == 21 {
            if let Value::Syntax(child) = self.terms[term].fields[a.n as usize] { return self.node(child, inputs, env, depth + 1); }
            return Err(error("invalid syntax field"));
        }
        let left = self.action(a.x, term, inputs, env, depth + 1)?;
        if a.op == 22 {
            if left.status != 0 { return Ok(left); }
            let mut bindings = env.clone(); bindings.extend(left.bindings);
            return Outcome::new(0, left.out, bindings);
        }
        if a.op == 26 { return if left.status == 1 { self.action(a.y, term, inputs, env, depth + 1) } else { Ok(left) }; }
        if a.op == 27 { return if left.status == 2 { self.action(a.y, term, &left.out, env, depth + 1) } else { Ok(left) }; }
        if left.status != 0 { return Ok(left); }
        let right = self.action(a.y, term, if a.op == 23 { &left.out } else { inputs },
            if a.op == 25 { &left.bindings } else { env }, depth + 1)?;
        if right.status != 0 { return Ok(right); }
        let bindings = if a.op == 25 { right.bindings } else {
            if right.bindings.keys().any(|k| left.bindings.contains_key(k)) { return Outcome::empty(1); }
            let mut bindings = left.bindings; bindings.extend(right.bindings); bindings
        };
        let out = if a.op == 23 { right.out } else { let mut out = left.out; out.extend(right.out); out };
        Outcome::new(0, out, bindings)
    }
    fn node(&mut self, term: usize, inputs: &[Value], env: &Env, depth: usize) -> Checked<Outcome> {
        self.tick(depth)?;
        self.action(self.ctors[self.terms[term].ctor].rule, term, inputs, env, depth + 1)
    }
    fn run(&mut self) -> Checked<(String, i32)> {
        let root = self.parse(self.start, 1)?;
        if self.tokens[self.pos].kind != 0 { return Err(self.syntax("trailing term input")); }
        let r = self.node(root, &[], &Env::new(), 1)?;
        let mut text = ["completed", "failed", "escaped"][r.status as usize].to_string();
        for v in r.out {
            text.push(' ');
            text.push_str(&match v {
                Value::Integer(n) => n.to_string(), Value::Token(s) => format!("token:{}", s),
                Value::Cell(n) => format!("cell:{}", n), Value::Syntax(_) => return Err(error("invalid output value"))
            });
        }
        text.push('\n'); Ok((text, i32::from(r.status != 0)))
    }
}
fn read_source(stream: impl Read) -> Checked<Vec<u8>> {
    let mut source = Vec::new();
    stream.take(65537).read_to_end(&mut source).map_err(|e| error(&e.to_string()))?;
    if source.len() > 65536 { return Err(limit("input exceeds 64 KiB")); }
    Ok(source)
}
fn main_checked() -> Checked<i32> {
    let args: Vec<String> = std::env::args_os().skip(1).map(|s| s.into_string().map_err(|_| error("invalid UTF-8 argument"))).collect::<Checked<_>>()?;
    let (mut term, mut path, mut output) = (None, None, None);
    let mut i = 0;
    while i < args.len() {
        let (flag, inline) = args[i].split_once('=').map(|(a,b)| (a,Some(b))).unwrap_or((&args[i],None));
        if inline.is_none() {
            if flag == "--help" || flag == "-h" {
                println!("generated interpreter [--term PROGRAM | --term-file PATH] [-o FILE]\nExit: completed=0, failed/escaped=1, errors=2."); return Ok(0);
            }
            if flag == "--version" { println!("acsem @VERSION@"); return Ok(0); }
            if flag == "--run" { i += 1; continue; }
        }
        if !["--term", "--term-file", "-o", "--output"].contains(&flag) { return Err(error("invalid option")); }
        let arg = if let Some(s) = inline { s.to_string() } else {
            i += 1; args.get(i).ok_or_else(|| error("missing option value"))?.clone()
        };
        match flag { "--term" => term = Some(arg), "--term-file" => path = Some(arg), _ => output = Some(arg) }
        i += 1;
    }
    if term.is_some() && path.is_some() { return Err(error("use only one of --term and --term-file")); }
    let source = if let Some(text) = term { text.into_bytes() }
        else if let Some(path) = path { read_source(std::fs::File::open(path).map_err(|e| error(&e.to_string()))?)? }
        else { read_source(io::stdin().lock())? };
    let (text, status) = Runtime::new(source)?.run()?;
    if let Some(path) = output.filter(|s| s != "-") {
        std::fs::write(path, text).map_err(|e| error(&e.to_string()))?;
    } else {
        let mut stdout = io::stdout().lock();
        stdout.write_all(text.as_bytes()).and_then(|()| stdout.flush()).map_err(|e| error(&e.to_string()))?;
    }
    Ok(status)
}
fn main() {
    match main_checked() { Ok(status) => std::process::exit(status), Err(message) => { eprintln!("{}", message); std::process::exit(2); } }
}
