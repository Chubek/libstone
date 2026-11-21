#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
command -v vim >/dev/null || { echo 'SKIP: vim not installed'; exit 0; }
vim -Nu NONE -n -es \
  --cmd "set runtimepath^=$ROOT/neovim" \
  --cmd 'filetype plugin indent on' \
  --cmd 'syntax on' \
  "$ROOT/examples/tiny.llsynth" \
  -c 'if &filetype !=# "llsynth" | cquit 11 | endif' \
  -c 'if !exists("b:current_syntax") || b:current_syntax !=# "llsynth" | cquit 12 | endif' \
  -c 'if &indentexpr !~# "GetLLSynthIndent" | cquit 13 | endif' \
  -c 'if !exists("*GetLLSynthIndent") | cquit 14 | endif' \
  -c 'if GetLLSynthIndent(9) != 2 | cquit 15 | endif' \
  -c 'if synIDattr(synID(5, 7, 1), "name") !=# "llsynthRegex" | cquit 16 | endif' \
  -c 'if synIDattr(synID(11, 22, 1), "name") !=# "llsynthSigil" | cquit 17 | endif' \
  -c 'qa!'
echo 'OK: Vim detected and loaded LLSynth syntax/indent/ftplugin'
