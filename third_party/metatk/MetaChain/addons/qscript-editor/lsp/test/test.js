#!/usr/bin/env node
'use strict';
// End-to-end test: speaks LSP to server/index.js over stdio.
// Usage: node test/test.js [path/to/qscript]   (or set QSCRIPT_EXECUTABLE)
const {spawn} = require('child_process');
const assert = require('assert');
const fs = require('fs');
const os = require('os');
const path = require('path');

const qscript = process.argv[2] || process.env.QSCRIPT_EXECUTABLE || 'qscript';
const server = spawn(process.execPath, [path.join(__dirname, '..', 'server', 'index.js')], {stdio: ['pipe', 'pipe', 'inherit']});
let buffer = Buffer.alloc(0), nextId = 1;
const waiting = new Map(), notes = [];
let noteWaiter = null;
server.stdout.on('data', chunk => {
  buffer = Buffer.concat([buffer, chunk]);
  for (;;) {
    const h = buffer.indexOf('\r\n\r\n');
    if (h < 0) return;
    const len = Number(/Content-Length: (\d+)/.exec(buffer.slice(0, h).toString())[1]);
    if (buffer.length < h + 4 + len) return;
    const msg = JSON.parse(buffer.slice(h + 4, h + 4 + len).toString());
    buffer = buffer.slice(h + 4 + len);
    if (msg.id !== undefined && waiting.has(msg.id)) { waiting.get(msg.id)(msg); waiting.delete(msg.id); }
    else { notes.push(msg); if (noteWaiter) noteWaiter(); }
  }
});
function write(msg) { const b = Buffer.from(JSON.stringify(msg)); server.stdin.write(`Content-Length: ${b.length}\r\n\r\n`); server.stdin.write(b); }
function request(method, params) {
  const id = nextId++;
  return new Promise(resolve => { waiting.set(id, resolve); write({jsonrpc: '2.0', id, method, params}); });
}
function notifyServer(method, params) { write({jsonrpc: '2.0', method, params}); }
async function diagnosticsFor(uri, version) {
  for (;;) {
    const i = notes.findIndex(n => n.method === 'textDocument/publishDiagnostics' && n.params.uri === uri && (version === undefined || n.params.version === version));
    if (i >= 0) return notes.splice(i, 1)[0].params.diagnostics;
    await new Promise(r => { noteWaiter = r; setTimeout(r, 50); });
  }
}

(async () => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'qscript-lsp-test-'));
  fs.writeFileSync(path.join(dir, 'shapes.qs'), '// shapes: tiny test library\n// area of a rectangle\nfn area(w, h) { return w * h; }\nconst UNIT = 10;\n');
  const uri = 'file://' + path.join(dir, 'main.qs');
  const init = await request('initialize', {processId: process.pid, rootUri: null, capabilities: {},
                                            initializationOptions: {executable: qscript, debounceMs: 10}});
  assert.equal(init.result.serverInfo.name, 'qscript-lsp');
  assert.ok(init.result.capabilities.hoverProvider);
  notifyServer('initialized', {});

  const good = [
    'use math, shapes;',
    '// Square of x.',
    'fn sq(x) { return x * x; }',
    'const N = 0x10;',
    'let total = 0;',
    'for (my i = 0; i < N; i++) total += sq(gcd(i, 12));',
    'return area(total, UNIT);',
    ''].join('\n');
  notifyServer('textDocument/didOpen', {textDocument: {uri, languageId: 'qscript', version: 1, text: good}});
  assert.deepEqual(await diagnosticsFor(uri, 1), [], 'valid program has no diagnostics');

  // Diagnostics from the compiler, positioned on the offending token.
  const bad = good.replace('return area(total, UNIT);', 'return aera(total, UNIT);');
  notifyServer('textDocument/didChange', {textDocument: {uri, version: 2}, contentChanges: [{text: bad}]});
  let d = await diagnosticsFor(uri, 2);
  assert.equal(d.length, 1);
  assert.match(d[0].message, /undefined function: aera/);
  assert.deepEqual(d[0].range.start, {line: 6, character: 7});
  assert.deepEqual(d[0].range.end, {line: 6, character: 11});
  // Incremental change back to the good text, plus a syntax error.
  notifyServer('textDocument/didChange', {textDocument: {uri, version: 3}, contentChanges: [
    {range: {start: {line: 6, character: 7}, end: {line: 6, character: 11}}, text: 'area'},
    {range: {start: {line: 4, character: 13}, end: {line: 4, character: 14}}, text: ''}]});
  d = await diagnosticsFor(uri, 3);
  assert.equal(d.length, 1);
  assert.match(d[0].message, /expected ';'/);
  assert.equal(d[0].range.start.line, 5);
  notifyServer('textDocument/didChange', {textDocument: {uri, version: 4}, contentChanges: [{text: good}]});
  assert.deepEqual(await diagnosticsFor(uri, 4), []);

  const at = (line, character) => ({textDocument: {uri}, position: {line, character}});
  // Hover: stdlib function, local function with doc comment, user library, keyword, number.
  let h = await request('textDocument/hover', at(5, 40));
  assert.match(h.result.contents.value, /math::fn gcd\(a, b\)/);
  h = await request('textDocument/hover', at(5, 37));
  assert.match(h.result.contents.value, /fn sq\(x\)/); assert.match(h.result.contents.value, /Square of x/);
  h = await request('textDocument/hover', at(6, 9));
  assert.match(h.result.contents.value, /shapes::fn area\(w, h\)/); assert.match(h.result.contents.value, /area of a rectangle/);
  h = await request('textDocument/hover', at(4, 1));
  assert.match(h.result.contents.value, /\*\*let\*\*/);
  h = await request('textDocument/hover', at(3, 11));
  assert.match(h.result.contents.value, /= 16/);
  h = await request('textDocument/hover', at(6, 20));
  assert.match(h.result.contents.value, /UNIT = 10/);

  // Completion: after `use`, after `lib::`, and in code.
  notifyServer('textDocument/didChange', {textDocument: {uri, version: 5}, contentChanges: [{text: good + 'use pr\nmath::\nisq\n'}]});
  let c = await request('textDocument/completion', at(7, 6));
  assert.ok(c.result.items.some(i => i.label === 'prime'));
  c = await request('textDocument/completion', at(8, 6));
  const isqrt = c.result.items.find(i => i.label === 'isqrt');
  assert.equal(isqrt.detail, 'isqrt(n)');
  assert.ok(c.result.items.some(i => i.label === 'INT_MAX'));
  c = await request('textDocument/completion', at(9, 3));
  for (const label of ['isqrt', 'area', 'sq', 'N', 'total', 'while', 'fn'])
    assert.ok(c.result.items.some(i => i.label === label), 'completion offers ' + label);
  notifyServer('textDocument/didChange', {textDocument: {uri, version: 6}, contentChanges: [{text: good}]});
  await diagnosticsFor(uri, 6);

  // Signature help inside gcd(i, |12)
  const sig = await request('textDocument/signatureHelp', at(5, 47));
  assert.equal(sig.result.signatures[0].label, 'math::gcd(a, b)');
  assert.equal(sig.result.activeParameter, 1);

  // Definition: local function, variable, user library, stdlib (materialized via `qscript lib`).
  let def = await request('textDocument/definition', at(5, 37));
  assert.deepEqual(def.result.range.start, {line: 2, character: 3});
  def = await request('textDocument/definition', at(5, 29));
  assert.deepEqual(def.result.range.start, {line: 4, character: 4});
  def = await request('textDocument/definition', at(6, 9));
  assert.ok(def.result.uri.endsWith('/shapes.qs') && def.result.range.start.line === 2);
  def = await request('textDocument/definition', at(5, 40));
  assert.ok(def.result && def.result.uri.endsWith('/math.qs'), 'stdlib definition');
  const mathLines = fs.readFileSync(new URL(def.result.uri), 'utf8').split('\n');
  assert.match(mathLines[def.result.range.start.line], /^fn gcd\(/);

  // Symbols, references, folding.
  const sym = await request('textDocument/documentSymbol', {textDocument: {uri}});
  assert.deepEqual(sym.result.map(s => s.name), ['math', 'shapes', 'sq', 'N', 'total']);
  const refs = await request('textDocument/references', {...at(4, 5), context: {includeDeclaration: true}});
  assert.equal(refs.result.length, 3);
  notifyServer('textDocument/didChange', {textDocument: {uri, version: 7}, contentChanges: [{text: 'fn f() {\n  /* a\n  b */\n  return 1;\n}\n'}]});
  await diagnosticsFor(uri, 7);
  const folds = await request('textDocument/foldingRange', {textDocument: {uri}});
  assert.ok(folds.result.some(f => f.startLine === 0 && f.endLine === 3));
  assert.ok(folds.result.some(f => f.kind === 'comment' && f.startLine === 1 && f.endLine === 2));

  // Formatting re-indents.
  const messy = 'fn f(n) {\nif (n)\nreturn 1;\nlet x = 1 +\n2;\nreturn x;\n}\n';
  notifyServer('textDocument/didChange', {textDocument: {uri, version: 8}, contentChanges: [{text: messy}]});
  await diagnosticsFor(uri, 8);
  const fmt = await request('textDocument/formatting', {textDocument: {uri}, options: {tabSize: 4, insertSpaces: true}});
  const lines = messy.split('\n');
  for (const e of fmt.result) lines[e.range.start.line] = e.newText;
  assert.equal(lines.join('\n'), 'fn f(n) {\n    if (n)\n        return 1;\n    let x = 1 +\n        2;\n    return x;\n}\n');

  // Errors inside a library point at the `use`.
  fs.writeFileSync(path.join(dir, 'broken.qs'), 'fn oops() { return nope; }\n');
  notifyServer('textDocument/didChange', {textDocument: {uri, version: 9}, contentChanges: [{text: 'use broken;\nreturn 0;\n'}]});
  d = await diagnosticsFor(uri, 9);
  assert.equal(d.length, 1);
  assert.match(d[0].message, /broken\.qs:1:\d+: undefined variable: nope/);
  assert.deepEqual(d[0].range.start, {line: 0, character: 4});

  const shut = await request('shutdown', null);
  assert.equal(shut.result, null);
  notifyServer('exit', null);
  server.on('exit', code => { assert.equal(code, 0); console.log('QScript-LSP tests passed'); });
})().catch(error => { console.error(error); server.kill(); process.exit(1); });
