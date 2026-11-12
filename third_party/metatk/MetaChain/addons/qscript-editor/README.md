# QScript editor support

Editor support for QScript, the integer scripting language of MetaChain.

| Path | What |
|------|------|
| `vim/syntax/qscript.vim` | syntax highlighting (+ generated `qscript_stdlib.vim`) |
| `vim/indent/qscript.vim` | indentation |
| `vim/ftdetect/qscript.vim` | `*.qs`, `*.qscript` and `#!...qscript` detection |
| `vim/ftplugin/qscript.vim` | comments, `gf` on `use`, `:make`, `K`, `:QScriptRun` and friends |
| `vim/lua/qscript.lua` | Neovim module: filetype, language server, commands |
| `vim/doc/qscripts.txt` | Vim help (`:help qscript`) |
| `lsp/` | QScript-LSP, a dependency-free Node.js language server |
| `sublime/QScript.sublime-syntax` | Sublime Text syntax (+ comments, build system, LSP client config, syntax test) |

## Vim

```vim
set runtimepath+=/path/to/qscript-editor/vim
filetype plugin indent on
syntax on
```

Then `:helptags /path/to/qscript-editor/vim/doc` and `:help qscript`.

## Neovim

```lua
vim.opt.runtimepath:append('/path/to/qscript-editor/vim')
require('qscript').setup({ executable = 'qscript' })
```

This also starts QScript-LSP from `lsp/` (the two directories are found
relative to each other) unless `qscript-lsp` is on `$PATH`.

## Sublime Text

Copy the files in `sublime/` to `Packages/QScript/` (Preferences > Browse
Packages). Tools > Build runs the file; the build variants check it, run it on
the JIT, show MetaIL or build an executable. For language-server features,
install the LSP package and add `sublime/LSP-qscript.sublime-settings`.

## Requirements

The run/check commands and LSP diagnostics use the `qscript` driver built with
MetaChain (`cmake --build build --target qscript`). Set its path with
`g:qscript_executable` (Vim), `executable` (Lua module, LSP), or edit the
`cmd` in `QScript.sublime-build`.
