" Buffer-local LLSynth editing settings and Lua command shims.
if exists('b:did_ftplugin')
  finish
endif
let b:did_ftplugin = 1

setlocal expandtab
setlocal shiftwidth=2
setlocal softtabstop=2
setlocal tabstop=2
setlocal commentstring=#\ %s
setlocal comments=b:#,b://
setlocal formatoptions-=t
setlocal suffixesadd=.llsynth

command! -buffer -nargs=0 LLSynthCheck lua require('llsynth').check()
command! -buffer -nargs=? LLSynthGenerate lua require('llsynth').generate({target = <q-args>})

let b:undo_ftplugin = 'setlocal expandtab< shiftwidth< softtabstop< tabstop< commentstring< comments< formatoptions< suffixesadd< | silent! delcommand -buffer LLSynthCheck | silent! delcommand -buffer LLSynthGenerate'
