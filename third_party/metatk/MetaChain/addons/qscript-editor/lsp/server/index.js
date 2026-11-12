#!/usr/bin/env node
'use strict';

// QScript-LSP: a dependency-free language server for QScript.
//
// Diagnostics come from the real compiler (`qscript check -`), so they are
// exactly what `qscript run` would report. Completion, hover, signature help,
// go-to-definition, symbols, references, folding and formatting work from a
// light tokenizer plus the standard-library index in stdlib.json (generated
// from qscript/stdlib by gen_stdlib.py).
//
// Settings (initializationOptions, or workspace/didChangeConfiguration under
// the "qscript" key):
//   executable    path of the qscript driver (default: $QSCRIPT_EXECUTABLE or "qscript")
//   libraryPaths  extra directories passed as -L (the document's own directory is always added)
//   checkOnChange run diagnostics while typing (default true; otherwise on open/save)
//   debounceMs    delay before checking after a change (default 250)

const fs = require('fs');
const os = require('os');
const path = require('path');
const {spawn} = require('child_process');

const STDLIB = JSON.parse(fs.readFileSync(path.join(__dirname, 'stdlib.json'), 'utf8'));
const LIBS = STDLIB.libraries;
const KEYWORDS = STDLIB.keywords;
const VERSION = '1.0.0';

const settings = {
  executable: process.env.QSCRIPT_EXECUTABLE || 'qscript',
  libraryPaths: [],
  checkOnChange: true,
  debounceMs: 250,
};
let compilerAvailable = null;   // unknown until first run
let warnedMissing = false;
const documents = new Map();    // uri -> {text, version}
const timers = new Map();
let shuttingDown = false;

// ---------------------------------------------------------------- transport
let pending = Buffer.alloc(0);
function send(message) {
  const body = Buffer.from(JSON.stringify(message), 'utf8');
  process.stdout.write(`Content-Length: ${body.length}\r\n\r\n`);
  process.stdout.write(body);
}
const respond = (id, result) => send({jsonrpc: '2.0', id, result});
const fail = (id, code, message) => send({jsonrpc: '2.0', id, error: {code, message}});
const notify = (method, params) => send({jsonrpc: '2.0', method, params});

process.stdin.on('data', chunk => {
  pending = Buffer.concat([pending, chunk]);
  for (;;) {
    const header = pending.indexOf('\r\n\r\n');
    if (header < 0) return;
    const match = /Content-Length:\s*(\d+)/i.exec(pending.slice(0, header).toString('ascii'));
    if (!match) { pending = pending.slice(header + 4); continue; }
    const length = Number(match[1]);
    if (pending.length < header + 4 + length) return;
    const body = pending.slice(header + 4, header + 4 + length).toString('utf8');
    pending = pending.slice(header + 4 + length);
    let message;
    try { message = JSON.parse(body); } catch { continue; }
    try { handle(message); }
    catch (error) { if (message.id !== undefined) fail(message.id, -32603, String(error && error.stack || error)); }
  }
});
process.stdin.on('end', () => process.exit(shuttingDown ? 0 : 1));

// ---------------------------------------------------------------- text model
function uriToPath(uri) {
  if (!uri.startsWith('file://')) return null;
  let p = decodeURIComponent(uri.slice(7));
  if (/^\/[A-Za-z]:/.test(p)) p = p.slice(1);
  return p;
}
function pathToUri(p) {
  return 'file://' + (p.startsWith('/') ? '' : '/') + p.split(path.sep).map(encodeURIComponent).join('/').replace(/%3A/g, ':');
}
function lineStarts(text) {
  const starts = [0];
  for (let i = 0; i < text.length; ++i) if (text[i] === '\n') starts.push(i + 1);
  return starts;
}
function offsetAt(text, position) {
  const starts = lineStarts(text);
  const line = Math.min(position.line, starts.length - 1);
  return Math.min(starts[line] + position.character, line + 1 < starts.length ? starts[line + 1] - 1 : text.length);
}
function positionAt(starts, offset) {
  let lo = 0, hi = starts.length - 1;
  while (lo < hi) { const mid = (lo + hi + 1) >> 1; if (starts[mid] <= offset) lo = mid; else hi = mid - 1; }
  return {line: lo, character: offset - starts[lo]};
}

// Tokens: ident (with ::), number, char, punct; comments are skipped but
// recorded for folding and doc comments.
function tokenize(text) {
  const tokens = [], comments = [];
  let i = 0;
  while (i < text.length) {
    const c = text[i];
    if (c === ' ' || c === '\t' || c === '\r' || c === '\n') { ++i; continue; }
    if ((c === '/' && text[i + 1] === '/') || c === '#') {
      const start = i; while (i < text.length && text[i] !== '\n') ++i;
      comments.push({start, end: i, text: text.slice(start, i), line: true}); continue;
    }
    if (c === '/' && text[i + 1] === '*') {
      const start = i; const end = text.indexOf('*/', i + 2); i = end < 0 ? text.length : end + 2;
      comments.push({start, end: i, text: text.slice(start, i), line: false}); continue;
    }
    if (/[A-Za-z_]/.test(c)) {
      const start = i;
      while (i < text.length && (/\w/.test(text[i]) || (text[i] === ':' && text[i + 1] === ':' && /[A-Za-z_]/.test(text[i + 2] || '')))) i += text[i] === ':' ? 2 : 1;
      tokens.push({type: 'ident', value: text.slice(start, i), start, end: i}); continue;
    }
    if (/\d/.test(c)) {
      const start = i; while (i < text.length && /\w/.test(text[i])) ++i;
      tokens.push({type: 'number', value: text.slice(start, i), start, end: i}); continue;
    }
    if (c === "'") {
      const start = i++; while (i < text.length && text[i] !== "'" && text[i] !== '\n') i += text[i] === '\\' ? 2 : 1;
      if (text[i] === "'") ++i;
      tokens.push({type: 'char', value: text.slice(start, i), start, end: i}); continue;
    }
    const three = text.slice(i, i + 3), two = text.slice(i, i + 2);
    const op = ['**=', '<<=', '>>='].includes(three) ? three :
      ['**', '<<', '>>', '<=', '>=', '==', '!=', '&&', '||', '++', '--', '+=', '-=', '*=', '/=', '%=', '&=', '|=', '^='].includes(two) ? two : c;
    tokens.push({type: 'punct', value: op, start: i, end: i + op.length});
    i += op.length;
  }
  return {tokens, comments};
}

const KEYWORD_SET = new Set(Object.keys(KEYWORDS));

// Declarations in a document: fn, const, let/my (with block scopes), use.
function analyze(text) {
  const {tokens, comments} = tokenize(text);
  const starts = lineStarts(text);
  const result = {tokens, comments, starts, functions: [], constants: [], variables: [], uses: [], blocks: []};
  const docBefore = offset => {
    // Line comments directly above offset form its documentation.
    const lines = [];
    let line = positionAt(starts, offset).line - 1;
    for (let k = comments.length - 1; k >= 0 && line >= 0; --k) {
      const cm = comments[k];
      if (cm.end > offset) continue;
      const cl = positionAt(starts, cm.start).line;
      if (cl !== line || !cm.line) { if (cl < line) break; continue; }
      lines.unshift(cm.text.replace(/^(\/\/|#)\s?/, ''));
      --line;
    }
    return lines.join('\n');
  };
  const stack = [];
  let depth = 0;
  for (let k = 0; k < tokens.length; ++k) {
    const t = tokens[k];
    if (t.type === 'punct' && t.value === '{') { stack.push(t.start); ++depth; continue; }
    if (t.type === 'punct' && t.value === '}') {
      const open = stack.pop(); depth = Math.max(0, depth - 1);
      if (open !== undefined) result.blocks.push({start: open, end: t.end});
      for (const v of result.variables) if (v.scopeEnd === undefined && v.depth > depth) v.scopeEnd = t.end;
      continue;
    }
    if (t.type !== 'ident') continue;
    const next = tokens[k + 1];
    if ((t.value === 'fn' || t.value === 'sub') && next && next.type === 'ident') {
      const params = [];
      let j = k + 2;
      if (tokens[j] && tokens[j].value === '(') {
        for (++j; j < tokens.length && tokens[j].value !== ')'; ++j) if (tokens[j].type === 'ident') params.push(tokens[j].value);
      }
      let body = j + 1, end = tokens[j] ? tokens[j].end : next.end;
      if (tokens[body] && tokens[body].value === '{') {
        let d = 0;
        for (let b = body; b < tokens.length; ++b) {
          if (tokens[b].value === '{') ++d;
          if (tokens[b].value === '}' && --d === 0) { end = tokens[b].end; break; }
          end = tokens[b].end;
        }
      }
      result.functions.push({name: next.value, params, start: t.start, nameStart: next.start, nameEnd: next.end, end, doc: docBefore(t.start)});
      for (const p of params) result.variables.push({name: p, start: next.end, nameStart: next.end, nameEnd: next.end, depth: depth + 1, param: true, fn: next.value, scopeEnd: end});
    } else if (t.value === 'const' && next && next.type === 'ident') {
      let j = k + 2, value = '';
      if (tokens[j] && tokens[j].value === '=') {
        const from = tokens[j].end; while (j < tokens.length && tokens[j].value !== ';') ++j;
        value = text.slice(from, tokens[j] ? tokens[j].start : text.length).trim();
      }
      result.constants.push({name: next.value, value, start: t.start, nameStart: next.start, nameEnd: next.end, doc: docBefore(t.start)});
    } else if ((t.value === 'let' || t.value === 'my') && next && next.type === 'ident') {
      const v = {name: next.value, start: t.start, nameStart: next.start, nameEnd: next.end, depth};
      // `for (let i = ...; ...) BODY`: i is scoped to the loop.
      const prev = tokens[k - 1], prev2 = tokens[k - 2];
      if (prev && prev.value === '(' && prev2 && prev2.value === 'for') {
        let j = k, d = 1;
        while (++j < tokens.length && d > 0) { if (tokens[j].value === '(') ++d; if (tokens[j].value === ')') --d; }
        let end = tokens[j - 1] ? tokens[j - 1].end : t.end;
        if (tokens[j] && tokens[j].value === '{') {
          let b = j, bd = 0;
          for (; b < tokens.length; ++b) { if (tokens[b].value === '{') ++bd; if (tokens[b].value === '}' && --bd === 0) break; }
          end = tokens[Math.min(b, tokens.length - 1)].end;
        } else {
          while (j < tokens.length && tokens[j].value !== ';') ++j;
          end = tokens[Math.min(j, tokens.length - 1)].end;
        }
        v.scopeEnd = end; v.loop = true;
      }
      result.variables.push(v);
    } else if (t.value === 'use') {
      for (let j = k + 1; j < tokens.length && tokens[j].value !== ';'; ++j)
        if (tokens[j].type === 'ident') result.uses.push({name: tokens[j].value, start: tokens[j].start, end: tokens[j].end});
    }
  }
  return result;
}

// ---------------------------------------------------------------- libraries
function libraryPathsFor(uri) {
  const dirs = [...settings.libraryPaths];
  const file = uriToPath(uri);
  if (file) dirs.unshift(path.dirname(file));
  return dirs;
}
// A user library found on the library path: {file, functions, constants}.
const userLibCache = new Map();
function findUserLibrary(name, uri) {
  for (const dir of libraryPathsFor(uri)) {
    const file = path.join(dir, name.split('::').join(path.sep) + '.qs');
    let stat;
    try { stat = fs.statSync(file); } catch { continue; }
    const cached = userLibCache.get(file);
    if (cached && cached.mtime === stat.mtimeMs) return cached;
    let text;
    try { text = fs.readFileSync(file, 'utf8'); } catch { continue; }
    const a = analyze(text);
    const lib = {file, mtime: stat.mtimeMs, summary: (a.comments[0] && a.comments[0].start === 0) ? a.comments[0].text.replace(/^(\/\/|#)\s?/, '') : '',
      functions: {}, constants: {}};
    for (const f of a.functions) if (!f.name.startsWith('_'))
      lib.functions[f.name] = {params: f.params, doc: f.doc, summary: '', line: positionAt(a.starts, f.start).line + 1};
    for (const c of a.constants) if (!c.name.startsWith('_'))
      lib.constants[c.name] = {value: c.value, doc: c.doc, line: positionAt(a.starts, c.start).line + 1};
    userLibCache.set(file, lib);
    return lib;
  }
  return null;
}
function library(name, uri) { return findUserLibrary(name, uri) || LIBS[name] || null; }
function listUserLibraries(uri) {
  const names = new Set();
  const walk = (dir, prefix, depth) => {
    let entries = [];
    try { entries = fs.readdirSync(dir, {withFileTypes: true}); } catch { return; }
    for (const e of entries) {
      if (e.isFile() && e.name.endsWith('.qs')) names.add(prefix + e.name.slice(0, -3));
      else if (e.isDirectory() && depth < 2 && /^[A-Za-z_]\w*$/.test(e.name)) walk(path.join(dir, e.name), prefix + e.name + '::', depth + 1);
    }
  };
  for (const dir of settings.libraryPaths) walk(dir, '', 0);
  return [...names];
}
function signature(name, params) { return `${name}(${params.join(', ')})`; }
function functionDoc(libName, name, f) {
  const parts = ['```qscript\n' + (libName ? `${libName}::` : '') + `fn ${signature(name, f.params)}\n` + '```'];
  if (f.summary) parts.push(f.summary);
  if (f.doc && f.doc !== f.summary) parts.push(f.doc);
  if (libName) parts.push(`*library \`${libName}\`*`);
  return parts.join('\n\n');
}

// Resolve a (possibly qualified) name used in a document.
function resolve(uri, a, name) {
  if (name.includes('::')) {
    const lib = name.slice(0, name.lastIndexOf('::')), short = name.slice(name.lastIndexOf('::') + 2);
    const L = library(lib, uri);
    if (L && L.functions[short]) return {kind: 'function', lib, name: short, info: L.functions[short], L};
    if (L && L.constants[short]) return {kind: 'constant', lib, name: short, info: L.constants[short], L};
    if (library(name, uri)) return {kind: 'library', name, L: library(name, uri)};
    return null;
  }
  const f = a.functions.find(x => x.name === name);
  if (f) return {kind: 'local-function', name, info: f};
  const c = a.constants.find(x => x.name === name);
  if (c) return {kind: 'local-constant', name, info: c};
  for (const u of a.uses) {
    const L = library(u.name, uri);
    if (L && L.functions[name]) return {kind: 'function', lib: u.name, name, info: L.functions[name], L};
  }
  for (const u of a.uses) {
    const L = library(u.name, uri);
    if (L && L.constants[name]) return {kind: 'constant', lib: u.name, name, info: L.constants[name], L};
  }
  if (library(name, uri)) return {kind: 'library', name, L: library(name, uri)};
  return null;
}

// ---------------------------------------------------------------- diagnostics
function fallbackDiagnostics(uri, text) {
  const a = analyze(text);
  const out = [];
  const range = (s, e) => ({start: positionAt(a.starts, s), end: positionAt(a.starts, e)});
  const stack = [];
  for (const t of a.tokens) {
    if (t.type !== 'punct') continue;
    if ('({['.includes(t.value)) stack.push(t);
    else if (')}]'.includes(t.value)) {
      const open = stack.pop();
      if (!open || '({['.indexOf(open.value) !== ')}]'.indexOf(t.value))
        out.push({range: range(t.start, t.end), severity: 1, source: 'qscript-lsp', message: `unmatched '${t.value}'`});
    }
  }
  for (const t of stack) out.push({range: range(t.start, t.end), severity: 1, source: 'qscript-lsp', message: `unclosed '${t.value}'`});
  for (const u of a.uses) if (!library(u.name, uri))
    out.push({range: range(u.start, u.end), severity: 1, source: 'qscript-lsp', message: `unknown library: ${u.name}`});
  return out;
}
function runCompiler(args, input, timeoutMs = 5000) {
  return new Promise(resolvePromise => {
    let child;
    try { child = spawn(settings.executable, args, {stdio: ['pipe', 'pipe', 'pipe']}); }
    catch (error) { resolvePromise({error}); return; }
    let stdout = '', stderr = '', done = false;
    const finish = r => { if (!done) { done = true; clearTimeout(timer); resolvePromise(r); } };
    const timer = setTimeout(() => { child.kill('SIGKILL'); finish({timeout: true, stdout, stderr}); }, timeoutMs);
    child.on('error', error => finish({error}));
    child.stdout.on('data', d => { stdout += d; });
    child.stderr.on('data', d => { stderr += d; });
    child.on('close', code => finish({code, stdout, stderr}));
    child.stdin.on('error', () => {});
    child.stdin.end(input);
  });
}
function parseCompilerOutput(text, uri, documentText) {
  const a = analyze(documentText);
  const lines = documentText.split('\n');
  const out = [];
  for (const raw of text.split(/\r?\n/)) {
    const line = raw.trim();
    if (!line) continue;
    const m = /^(.*?):(\d+):(\d+):\s*(.*)$/.exec(line);
    let row = 0, col = 0, message = line, file = '<stdin>';
    if (m) { file = m[1]; row = Number(m[2]) - 1; col = Math.max(0, Number(m[3]) - 1); message = m[4]; }
    else {
      const m2 = /^(.*?):\s*(.*)$/.exec(line);
      if (m2 && !/\s/.test(m2[1])) { file = m2[1]; message = m2[2]; }
    }
    let range;
    if (file === '<stdin>' && m) {
      row = Math.min(row, lines.length - 1);
      const text = lines[row] || '';
      const word = /^[A-Za-z_]\w*(?:::[A-Za-z_]\w*)*|^\S/.exec(text.slice(col));
      range = {start: {line: row, character: col}, end: {line: row, character: col + (word ? word[0].length : 1)}};
    } else {
      // An error inside a library: show it on the `use` that loaded it.
      const name = path.basename(file, '.qs');
      const u = a.uses.find(x => x.name === name || x.name.endsWith('::' + name)) || a.uses[0];
      range = u ? {start: positionAt(a.starts, u.start), end: positionAt(a.starts, u.end)} : {start: {line: 0, character: 0}, end: {line: 0, character: 1}};
      message = `${file}${m ? `:${m[2]}:${m[3]}` : ''}: ${message}`;
    }
    out.push({range, severity: 1, source: 'qscript', message});
  }
  return out;
}
async function diagnose(uri) {
  const doc = documents.get(uri);
  if (!doc) return;
  const version = doc.version, text = doc.text;
  let diagnostics;
  if (compilerAvailable === false) diagnostics = fallbackDiagnostics(uri, text);
  else {
    const args = [];
    for (const dir of libraryPathsFor(uri)) args.push('-L', dir);
    args.push('check', '-');
    const r = await runCompiler(args, text);
    if (r.error) {
      compilerAvailable = false;
      if (!warnedMissing) {
        warnedMissing = true;
        notify('window/showMessage', {type: 2, message: `QScript-LSP: cannot run "${settings.executable}" (${r.error.code || r.error.message}); using basic checks only. Set qscript.executable.`});
      }
      diagnostics = fallbackDiagnostics(uri, text);
    } else if (r.timeout) {
      diagnostics = [{range: {start: {line: 0, character: 0}, end: {line: 0, character: 0}}, severity: 2, source: 'qscript', message: 'qscript check timed out'}];
    } else {
      compilerAvailable = true;
      diagnostics = r.code === 0 ? [] : parseCompilerOutput(r.stderr || r.stdout, uri, text);
    }
  }
  const current = documents.get(uri);
  if (!current || current.version !== version) return;   // stale
  notify('textDocument/publishDiagnostics', {uri, version, diagnostics});
}
function schedule(uri, immediate) {
  clearTimeout(timers.get(uri));
  if (immediate) { diagnose(uri); return; }
  if (!settings.checkOnChange) return;
  timers.set(uri, setTimeout(() => diagnose(uri), settings.debounceMs));
}

// ---------------------------------------------------------------- features
function wordAt(text, offset) {
  let s = offset, e = offset;
  const isWord = ch => /[\w:]/.test(ch || '');
  while (s > 0 && isWord(text[s - 1])) --s;
  while (e < text.length && isWord(text[e])) ++e;
  let word = text.slice(s, e).replace(/^:+|:+$/g, '');
  return {word, start: s, end: e};
}
const SNIPPETS = [
  ['fn', 'fn ${1:name}(${2:params}) {\n\t$0\n}', 'function definition'],
  ['if', 'if (${1:cond}) {\n\t$0\n}', 'if statement'],
  ['ifelse', 'if (${1:cond}) {\n\t$2\n} else {\n\t$0\n}', 'if / else'],
  ['for', 'for (my ${1:i} = 0; ${1:i} < ${2:n}; ${1:i}++) {\n\t$0\n}', 'counting loop'],
  ['while', 'while (${1:cond}) {\n\t$0\n}', 'while loop'],
  ['loop', 'loop {\n\t$0\n\tlast if ${1:cond};\n}', 'infinite loop with exit'],
  ['do', 'do {\n\t$0\n} while (${1:cond});', 'do-while loop'],
  ['const', 'const ${1:NAME} = ${0:value};', 'constant'],
  ['use', 'use ${0:math};', 'import a library'],
];
function completion(params) {
  const doc = documents.get(params.textDocument.uri);
  if (!doc) return [];
  const uri = params.textDocument.uri;
  const text = doc.text, offset = offsetAt(text, params.position);
  const lineStart = text.lastIndexOf('\n', offset - 1) + 1;
  const before = text.slice(lineStart, offset);
  const items = [];
  const libItem = (name, L) => ({label: name, kind: 9, detail: 'library', documentation: {kind: 'markdown', value: (L && (L.doc || L.summary)) || ''}});
  // `use ...` -> library names
  if (/^\s*use\s+[\w:,\s]*$/.test(before)) {
    for (const [name, L] of Object.entries(LIBS)) items.push(libItem(name, L));
    for (const name of listUserLibraries(uri)) if (!LIBS[name]) items.push({label: name, kind: 9, detail: 'library (library path)'});
    return items;
  }
  // `lib::` -> its members
  const qual = /([A-Za-z_]\w*(?:::[A-Za-z_]\w*)*)::(\w*)$/.exec(before);
  if (qual) {
    const L = library(qual[1], uri);
    if (L) {
      for (const [name, f] of Object.entries(L.functions))
        items.push({label: name, kind: 3, detail: signature(name, f.params), documentation: {kind: 'markdown', value: functionDoc(qual[1], name, f)},
                    insertText: f.params.length ? `${name}($1)` : `${name}()`, insertTextFormat: 2});
      for (const [name, c] of Object.entries(L.constants))
        items.push({label: name, kind: 21, detail: `const ${name} = ${c.value}`, documentation: c.doc || ''});
    }
    return items;
  }
  const a = analyze(text);
  for (const [k, d] of Object.entries(KEYWORDS)) items.push({label: k, kind: 14, detail: 'keyword', documentation: d});
  for (const [label, body, detail] of SNIPPETS) items.push({label, kind: 15, detail, insertText: body, insertTextFormat: 2, sortText: 'zz' + label});
  for (const f of a.functions)
    items.push({label: f.name, kind: 3, detail: signature(f.name, f.params), documentation: f.doc || undefined,
                insertText: f.params.length ? `${f.name}($1)` : `${f.name}()`, insertTextFormat: 2, sortText: '0' + f.name});
  for (const c of a.constants) items.push({label: c.name, kind: 21, detail: `const ${c.name} = ${c.value}`, sortText: '0' + c.name});
  const seen = new Set();
  for (const v of a.variables) {
    if (v.start > offset || (v.scopeEnd !== undefined && v.scopeEnd < offset) || seen.has(v.name)) continue;
    seen.add(v.name);
    items.push({label: v.name, kind: 6, detail: v.param ? `parameter of ${v.fn}` : 'variable', sortText: '0' + v.name});
  }
  for (const u of a.uses) {
    const L = library(u.name, uri);
    if (!L) continue;
    for (const [name, f] of Object.entries(L.functions))
      items.push({label: name, kind: 3, detail: `${u.name}::${signature(name, f.params)}`, documentation: {kind: 'markdown', value: functionDoc(u.name, name, f)},
                  insertText: f.params.length ? `${name}($1)` : `${name}()`, insertTextFormat: 2, sortText: '1' + name});
    for (const [name, c] of Object.entries(L.constants))
      items.push({label: name, kind: 21, detail: `${u.name}::${name} = ${c.value}`, sortText: '1' + name});
  }
  for (const [name, L] of Object.entries(LIBS)) items.push({...libItem(name, L), sortText: '2' + name});
  return items;
}
function hover(params) {
  const doc = documents.get(params.textDocument.uri);
  if (!doc) return null;
  const uri = params.textDocument.uri;
  const offset = offsetAt(doc.text, params.position);
  const {word, start, end} = wordAt(doc.text, offset);
  if (!word) return null;
  const a = analyze(doc.text);
  const range = {start: positionAt(a.starts, start), end: positionAt(a.starts, end)};
  const md = value => ({contents: {kind: 'markdown', value}, range});
  if (KEYWORDS[word]) return md('**' + word + '** - ' + KEYWORDS[word]);
  const r = resolve(uri, a, word);
  if (r) {
    if (r.kind === 'function') return md(functionDoc(r.lib, r.name, r.info));
    if (r.kind === 'constant') return md('```qscript\nconst ' + `${r.lib}::${r.name} = ${r.info.value}` + '\n```' + (r.info.doc ? '\n\n' + r.info.doc : ''));
    if (r.kind === 'library') return md(`**library \`${r.name}\`**\n\n` + (r.L.doc || r.L.summary || ''));
    if (r.kind === 'local-function') return md('```qscript\nfn ' + signature(r.name, r.info.params) + '\n```' + (r.info.doc ? '\n\n' + r.info.doc : ''));
    if (r.kind === 'local-constant') return md('```qscript\nconst ' + `${r.name} = ${r.info.value}` + '\n```' + (r.info.doc ? '\n\n' + r.info.doc : ''));
  }
  const v = a.variables.filter(x => x.name === word && x.start <= offset && (x.scopeEnd === undefined || x.scopeEnd >= offset)).pop();
  if (v) return md('```qscript\n' + (v.param ? `${word}  // parameter of ${v.fn}` : `let ${word}`) + '\n```');
  const tok = a.tokens.find(t => t.start <= offset && offset < t.end);
  if (tok && tok.type === 'number') {
    const clean = tok.value.replace(/_/g, '');
    try { const n = BigInt(/^0[oO]/.test(clean) ? '0o' + clean.slice(2) : clean); return md(`\`${tok.value}\` = ${n} (0x${(BigInt.asUintN(64, n)).toString(16)})`); } catch { return null; }
  }
  if (tok && tok.type === 'char') {
    const body = tok.value.slice(1, -1);
    const map = {n: 10, r: 13, t: 9, '0': 0, '\\': 92, "'": 39, '"': 34};
    const code = body[0] === '\\' ? (body[1] === 'x' ? parseInt(body.slice(2), 16) : map[body[1]]) : body.charCodeAt(0);
    if (code !== undefined && !Number.isNaN(code)) return md(`\`${tok.value}\` = ${code}`);
  }
  return null;
}
function signatureHelp(params) {
  const doc = documents.get(params.textDocument.uri);
  if (!doc) return null;
  const uri = params.textDocument.uri;
  const text = doc.text, offset = offsetAt(text, params.position);
  let depth = 0, commas = 0, i = offset - 1;
  for (; i >= 0; --i) {
    const c = text[i];
    if (c === ')') ++depth;
    else if (c === '(') { if (depth === 0) break; --depth; }
    else if (c === ',' && depth === 0) ++commas;
    else if ((c === ';' || c === '{' || c === '}') && depth === 0) return null;
  }
  if (i < 0) return null;
  const m = /([A-Za-z_]\w*(?:::[A-Za-z_]\w*)*)\s*$/.exec(text.slice(0, i));
  if (!m || KEYWORD_SET.has(m[1])) return null;
  const r = resolve(uri, analyze(text), m[1]);
  let name, ps, docText = '';
  if (r && r.kind === 'function') { name = `${r.lib}::${r.name}`; ps = r.info.params; docText = r.info.summary || r.info.doc; }
  else if (r && r.kind === 'local-function') { name = r.name; ps = r.info.params; docText = r.info.doc; }
  else return null;
  return {signatures: [{label: signature(name, ps), documentation: docText || undefined, parameters: ps.map(p => ({label: p}))}],
          activeSignature: 0, activeParameter: Math.min(commas, Math.max(ps.length - 1, 0))};
}
async function materialize(lib, uri) {
  const user = findUserLibrary(lib, uri);
  if (user) return user.file;
  if (!LIBS[lib]) return null;
  const dir = path.join(os.tmpdir(), 'qscript-lsp-stdlib');
  const file = path.join(dir, lib + '.qs');
  if (fs.existsSync(file)) return file;
  const r = await runCompiler(['lib', lib], '');
  if (r.error || r.code !== 0 || !r.stdout) return null;
  fs.mkdirSync(dir, {recursive: true});
  fs.writeFileSync(file, r.stdout);
  return file;
}
async function definition(params) {
  const doc = documents.get(params.textDocument.uri);
  if (!doc) return null;
  const uri = params.textDocument.uri;
  const offset = offsetAt(doc.text, params.position);
  const {word} = wordAt(doc.text, offset);
  if (!word) return null;
  const a = analyze(doc.text);
  const here = (s, e) => ({uri, range: {start: positionAt(a.starts, s), end: positionAt(a.starts, e)}});
  const v = a.variables.filter(x => x.name === word && x.start <= offset && (x.scopeEnd === undefined || x.scopeEnd >= offset)).pop();
  if (v && !word.includes('::')) return here(v.nameStart, v.nameEnd);
  const r = resolve(uri, a, word);
  if (!r) return null;
  if (r.kind === 'local-function' || r.kind === 'local-constant') return here(r.info.nameStart, r.info.nameEnd);
  const lib = r.kind === 'library' ? r.name : r.lib;
  const file = await materialize(lib, uri);
  if (!file) return null;
  const line = r.kind === 'library' ? 0 : Math.max(0, (r.info.line || 1) - 1);
  return {uri: pathToUri(file), range: {start: {line, character: 0}, end: {line, character: 0}}};
}
function documentSymbols(params) {
  const doc = documents.get(params.textDocument.uri);
  if (!doc) return [];
  const a = analyze(doc.text);
  const R = (s, e) => ({start: positionAt(a.starts, s), end: positionAt(a.starts, e)});
  const symbols = [];
  for (const u of a.uses) symbols.push({name: u.name, kind: 2, detail: 'use', range: R(u.start, u.end), selectionRange: R(u.start, u.end)});
  for (const c of a.constants) symbols.push({name: c.name, kind: 14, detail: c.value, range: R(c.start, c.nameEnd), selectionRange: R(c.nameStart, c.nameEnd)});
  for (const f of a.functions) symbols.push({name: f.name, kind: 12, detail: signature(f.name, f.params), range: R(f.start, f.end), selectionRange: R(f.nameStart, f.nameEnd)});
  for (const v of a.variables) if (!v.param && !v.loop && v.depth === 0)
    symbols.push({name: v.name, kind: 13, range: R(v.start, v.nameEnd), selectionRange: R(v.nameStart, v.nameEnd)});
  return symbols.sort((x, y) => x.range.start.line - y.range.start.line || x.range.start.character - y.range.start.character);
}
function references(params) {
  const doc = documents.get(params.textDocument.uri);
  if (!doc) return [];
  const offset = offsetAt(doc.text, params.position);
  const {word} = wordAt(doc.text, offset);
  if (!word || KEYWORD_SET.has(word)) return [];
  const a = analyze(doc.text);
  const short = word.includes('::') ? word.slice(word.lastIndexOf('::') + 2) : word;
  return a.tokens.filter(t => t.type === 'ident' && (t.value === word || t.value === short || t.value.endsWith('::' + short)))
    .filter(t => params.context && params.context.includeDeclaration !== false || !a.functions.some(f => f.nameStart === t.start))
    .map(t => ({uri: params.textDocument.uri, range: {start: positionAt(a.starts, t.start), end: positionAt(a.starts, t.end)}}));
}
function foldingRanges(params) {
  const doc = documents.get(params.textDocument.uri);
  if (!doc) return [];
  const a = analyze(doc.text);
  const ranges = [];
  for (const b of a.blocks) {
    const s = positionAt(a.starts, b.start).line, e = positionAt(a.starts, b.end).line;
    if (e > s) ranges.push({startLine: s, endLine: e - 1});
  }
  for (const c of a.comments) {
    const s = positionAt(a.starts, c.start).line, e = positionAt(a.starts, c.end).line;
    if (!c.line && e > s) ranges.push({startLine: s, endLine: e, kind: 'comment'});
  }
  return ranges;
}
// Re-indent: braces and parentheses nest, a brace-less control header indents
// its single following statement, a line continuing an expression gets one
// extra level. Only leading whitespace changes.
function format(params) {
  const doc = documents.get(params.textDocument.uri);
  if (!doc) return [];
  const opts = params.options || {tabSize: 4, insertSpaces: true};
  const unit = opts.insertSpaces ? ' '.repeat(opts.tabSize || 4) : '\t';
  const lines = doc.text.split('\n');
  const header = /^(\}\s*)?(?:(?:if|elsif|unless|while|until|for)\s*\(.*\)|else|do)$/;
  const edits = [];
  let depth = 0, pendingHeader = 0, continued = false, inComment = false;
  lines.forEach((line, row) => {
    const trimmed = line.trim();
    let code = trimmed;
    if (inComment) {
      const end = code.indexOf('*/');
      const want = trimmed === '' ? '' : unit.repeat(depth) + ' ' + trimmed;
      if (trimmed !== '' && line !== want) edits.push({range: {start: {line: row, character: 0}, end: {line: row, character: line.length}}, newText: want});
      if (end < 0) return;
      inComment = false; code = code.slice(end + 2).trim();
      if (!code) return;
    }
    code = code.replace(/'(?:[^'\\]|\\.)*'/g, "'c'").replace(/\/\*.*?\*\//g, '').replace(/\s*(?:\/\/|#).*$/, '');
    if (code.includes('/*')) { inComment = true; code = code.slice(0, code.indexOf('/*')).trim(); }
    let level = depth + pendingHeader + (continued ? 1 : 0);
    const leadingClose = /^[)}\]]+/.exec(code);
    if (leadingClose) level = Math.max(0, level - leadingClose[0].length - (continued ? 1 : 0));
    if (/^(else|elsif)\b/.test(code) && pendingHeader) level = Math.max(0, level - 1);
    const want = trimmed === '' ? '' : unit.repeat(level) + trimmed;
    if (line !== want) edits.push({range: {start: {line: row, character: 0}, end: {line: row, character: line.length}}, newText: want});
    if (!code) return;
    for (const ch of code) { if ('{(['.includes(ch)) ++depth; else if ('})]'.includes(ch)) depth = Math.max(0, depth - 1); }
    if (header.test(code)) { pendingHeader += 1; continued = false; return; }
    if (/[;}{]$/.test(code) || /^(use|fn|sub|const)\b/.test(code)) { pendingHeader = 0; continued = false; return; }
    continued = /(?:[-+*\/%&|^<>=!?:,]|\band|\bor|\bnot)$/.test(code) && !/[({[]$/.test(code);
  });
  return edits;
}

// ---------------------------------------------------------------- dispatch
function applySettings(s) {
  if (!s) return;
  const q = s.qscript || s;
  if (typeof q.executable === 'string' && q.executable) { settings.executable = q.executable; compilerAvailable = null; warnedMissing = false; }
  if (Array.isArray(q.libraryPaths)) settings.libraryPaths = q.libraryPaths.filter(x => typeof x === 'string');
  if (typeof q.checkOnChange === 'boolean') settings.checkOnChange = q.checkOnChange;
  if (Number.isFinite(q.debounceMs)) settings.debounceMs = Math.max(0, q.debounceMs);
}
function handle(message) {
  const {id, method, params} = message;
  switch (method) {
  case 'initialize':
    applySettings(params && params.initializationOptions);
    respond(id, {
      capabilities: {
        textDocumentSync: {openClose: true, change: 1, save: {includeText: false}},
        completionProvider: {triggerCharacters: [':', ' '], resolveProvider: false},
        hoverProvider: true,
        signatureHelpProvider: {triggerCharacters: ['(', ','], retriggerCharacters: [',']},
        definitionProvider: true,
        referencesProvider: true,
        documentSymbolProvider: true,
        foldingRangeProvider: true,
        documentFormattingProvider: true,
      },
      serverInfo: {name: 'qscript-lsp', version: VERSION},
    });
    return;
  case 'initialized': return;
  case 'shutdown': shuttingDown = true; respond(id, null); return;
  case 'exit': process.exit(shuttingDown ? 0 : 1); return;
  case 'workspace/didChangeConfiguration':
    applySettings(params && params.settings);
    for (const uri of documents.keys()) schedule(uri, true);
    return;
  case 'textDocument/didOpen':
    documents.set(params.textDocument.uri, {text: params.textDocument.text, version: params.textDocument.version});
    schedule(params.textDocument.uri, true);
    return;
  case 'textDocument/didChange': {
    const doc = documents.get(params.textDocument.uri);
    if (!doc) return;
    for (const change of params.contentChanges) {
      if (!change.range) doc.text = change.text;
      else {
        const s = offsetAt(doc.text, change.range.start), e = offsetAt(doc.text, change.range.end);
        doc.text = doc.text.slice(0, s) + change.text + doc.text.slice(e);
      }
    }
    doc.version = params.textDocument.version;
    schedule(params.textDocument.uri, false);
    return;
  }
  case 'textDocument/didSave': schedule(params.textDocument.uri, true); return;
  case 'textDocument/didClose':
    documents.delete(params.textDocument.uri);
    clearTimeout(timers.get(params.textDocument.uri));
    notify('textDocument/publishDiagnostics', {uri: params.textDocument.uri, diagnostics: []});
    return;
  case 'textDocument/completion': respond(id, {isIncomplete: false, items: completion(params)}); return;
  case 'textDocument/hover': respond(id, hover(params)); return;
  case 'textDocument/signatureHelp': respond(id, signatureHelp(params)); return;
  case 'textDocument/definition': definition(params).then(r => respond(id, r), e => fail(id, -32603, String(e))); return;
  case 'textDocument/references': respond(id, references(params)); return;
  case 'textDocument/documentSymbol': respond(id, documentSymbols(params)); return;
  case 'textDocument/foldingRange': respond(id, foldingRanges(params)); return;
  case 'textDocument/formatting': respond(id, format(params)); return;
  default:
    if (id !== undefined && method) fail(id, -32601, `method not found: ${method}`);
  }
}

if (require.main !== module) module.exports = {analyze, tokenize, format, parseCompilerOutput};
