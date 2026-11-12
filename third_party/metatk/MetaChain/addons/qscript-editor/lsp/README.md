# QScript-LSP

A dependency-free [Language Server Protocol](https://microsoft.github.io/language-server-protocol/)
server for QScript, the integer scripting language of MetaChain. It needs only
Node.js 18 or newer; there is nothing to `npm install`.

```sh
node server/index.js            # speaks LSP on stdin/stdout
npm install -g .                # or install the `qscript-lsp` command
node test/test.js path/to/qscript   # end-to-end protocol test
```

## Features

| Feature | Details |
|---------|---------|
| Diagnostics | from the real compiler (`qscript check -`), so they match `qscript run` exactly; positioned on the offending token; errors inside a library appear on its `use` line; basic bracket/library checks when the driver is missing |
| Completion | keywords and snippets; local functions, constants and in-scope variables; functions and constants of used libraries; library names after `use`; members after `lib::` |
| Hover | signature and documentation for standard-library and local functions, constant values, library summaries, keywords, values of numeric and character literals |
| Signature help | for local and library functions, with the active parameter |
| Go to definition | local functions, constants, variables and parameters; user libraries on the library path; standard-library sources (fetched with `qscript lib`) |
| References, symbols, folding | per document |
| Formatting | re-indentation only (braces, brace-less control bodies, continuation lines) |

The standard-library index (`server/stdlib.json`) is generated from
`qscript/stdlib` by `qscript/distrib/gen_stdlib.py`.

## Settings

Pass as `initializationOptions`, or under the `qscript` key of
`workspace/didChangeConfiguration`:

| Setting | Default | Meaning |
|---------|---------|---------|
| `executable` | `$QSCRIPT_EXECUTABLE` or `qscript` | the qscript driver |
| `libraryPaths` | `[]` | extra `-L` directories (the document's own directory is always searched) |
| `checkOnChange` | `true` | check while typing; otherwise on open and save |
| `debounceMs` | `250` | delay before checking after an edit |

## Editors

**Neovim** (0.10+): `require('qscript').setup()` from the `vim/lua` module
starts the server automatically. Or directly:

```lua
vim.api.nvim_create_autocmd('FileType', { pattern = 'qscript', callback = function(a)
  vim.lsp.start({ name = 'qscript', cmd = { 'node', '/path/to/lsp/server/index.js' },
                  init_options = { executable = 'qscript' } }, { bufnr = a.buf })
end })
```

**Vim** with vim-lsp: see `:help qscript-lsp`.

**Sublime Text** with the LSP package: `sublime/LSP-qscript.sublime-settings`.

**VS Code**: any generic LSP client extension; command
`node /path/to/lsp/server/index.js`, language id `qscript`, files `*.qs`.

**Emacs** (eglot):

```elisp
(add-to-list 'auto-mode-alist '("\\.qs\\'" . prog-mode))
(add-to-list 'eglot-server-programs '(prog-mode "node" "/path/to/lsp/server/index.js"))
```
