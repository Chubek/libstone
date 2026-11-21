" LLSynth filetype detection (.llsynth language specifications).
augroup llsynth_filetype
  autocmd!
  autocmd BufRead,BufNewFile *.llsynth setfiletype llsynth
augroup END
