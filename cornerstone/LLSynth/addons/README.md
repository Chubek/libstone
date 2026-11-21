# LLSynth editor addons

Syntax highlighting, indentation and convenient generator commands for
[LLSynth](https://github.com/Chubek/libstone/tree/master/cornerstone/LLSynth).
This archive contains **editor support only**; it does not install LLSynth
itself or change language semantics. No Tree-sitter dependency is needed.

## Contents

| Path | Purpose |
| --- | --- |
| `neovim/ftdetect/llsynth.vim` | Detect `.llsynth` files |
| `neovim/syntax/llsynth.vim` | Syntax highlighting for declarations, facets, actions and token regexes |
| `neovim/indent/llsynth.vim` | Heuristic indentation of block and facet bodies |
| `neovim/ftplugin/llsynth.vim` | Editing settings and buffer-local commands |
| `neovim/lua/llsynth/init.lua` | Asynchronous generator/checker integration, quickfix reporting |
| `bat/LLSynth.sublime-syntax` | Sublime syntax for `bat`/`batcat` |
| `examples/tiny.llsynth` | Minimal syntax-highlighting/check example |

## Neovim installation

Copy the **contents** of `neovim/` into your user Neovim configuration:

```sh
mkdir -p "${XDG_CONFIG_HOME:-$HOME/.config}/nvim"
cp -R neovim/. "${XDG_CONFIG_HOME:-$HOME/.config}/nvim/"
```

Or install the `neovim/` folder itself as a runtime-path plugin with your
preferred plugin manager. For a local installation without copying files:

```lua
vim.opt.runtimepath:prepend('/absolute/path/to/llsynth-adons/neovim')
```

Neovim normally enables filetype support automatically. After restarting,
open a `.llsynth` file and check `:set filetype?` (should be `llsynth`).
Use `:set syntax?` to check highlighting.

### Configure the LLSynth executable

By default, this addon runs `llsynth.pl` from `PATH`. To use the repository
checkout, configure it in `init.lua` **before** invoking the commands:

```lua
require('llsynth').setup({
  command = {'perl', '/absolute/path/to/libstone/cornerstone/LLSynth/scripts/llsynth.pl'},
  mode = 'interpret', -- change to 'both', 'compile', or 'rewrite' as appropriate
  target = 'python3', -- also: c99, c11, cpp17, cpp20
  include_paths = {}, -- optional directories used for .include
  module_paths = {},  -- optional directories used for .import
  open_quickfix_on_error = true,
})
```

Inside a saved `.llsynth` buffer:

- `:LLSynthCheck` executes `--check --mode=interpret` (unless configured
  otherwise) and opens quickfix on errors.
- `:LLSynthGenerate` emits a neighboring `.py` file by default.
- `:LLSynthGenerate c11` emits a neighboring `.c` file; other targets are
  `python3`, `c99`, `cpp17`, `cpp20`.

These commands do **not** run the generated program. They refuse to work with
unsaved changes so the diagnostic input is always the visible on-disk version.
The generator runs asynchronously using Neovim's `jobstart()` with argv lists,
not through an interpolated shell command. On failures, messages are inserted
into quickfix using the generator's text diagnostics.

### Notes on indentation

The indentation engine is intentionally conservative and recognizes `.rule`,
`.ir`, `.function`, `.macro`, `.init`, `.rewrite`, `.backend`, `.analysis`,
`.pipeline`, `.sumtype`, `.prodtype`, `'syntax:`, `'semantics:`, `'lower:`,
`'analyze:`, `'emit:` and `.end` boundaries (including legacy `..` rule
terminators). It also handles simple `{...}` action blocks. It is a
heuristic formatter, not an LR parser: unusual nested or multiline expressions
may require manual adjustment.

## Bat installation

`bat` and `batcat` use the Sublime Text syntax format. Install the supplied
syntax, then rebuild Bat's syntax cache:

```sh
mkdir -p "$(bat --config-dir)/syntaxes"
cp bat/LLSynth.sublime-syntax "$(bat --config-dir)/syntaxes/"
bat cache --build
bat --list-languages | grep -i llsynth
bat examples/tiny.llsynth
```

On systems where the executable is `batcat` (for example, some Debian/Ubuntu
installations), replace `bat` with `batcat` in those commands.

## LLSynth language coverage

The highlighters recognize the actual LLSynth spec notation: directives
(`.token`, `.literal`, `.skip`, `.rule`, `.function`, `.ir`, `.rewrite`,
`.backend`, `.module`, `.library`, `.import`, `.alias`, `.prodtype`, `.sumtype`, etc.),
facets (`'syntax:`, `'semantics:`, `'lower:`, `'analyze:`, `'emit:`), block
terminators (`.end`, `..`, `'end`), action sigils (`$capture`, `@state`,
`#table`, `!exception`, `%type`, `&function`), string literals and numeric
values. Regex delimiters are highlighted only on `.token`/`.skip` lines to
avoid confusing division in action code with regular expressions.

## Smoke testing

- If `vim` is installed, `bash tests/smoke-vim.sh` exercises detection, syntax
  and indentation without modifying your configuration.
- If PyYAML is installed, `python3 tests/check-sublime.py` validates the YAML
  structure and checks key contexts; this **does not** replace a real Bat
  syntax-cache build.
- For a full generator check, run:

  ```sh
  perl /path/to/libstone/cornerstone/LLSynth/scripts/llsynth.pl \
    --check --mode=interpret examples/tiny.llsynth
  ```

## Compatibility

- Neovim with runtime Vimscript + Lua support; the Lua helper uses APIs
  present in modern Neovim (0.8+). Vim can use the Vimscript highlighter and
  indenter, but the `LLSynthCheck`/`LLSynthGenerate` commands require Neovim.
- Bat releases supporting custom `.sublime-syntax` files.

This is a community-style starter addon, not an official part of LLSynth.
