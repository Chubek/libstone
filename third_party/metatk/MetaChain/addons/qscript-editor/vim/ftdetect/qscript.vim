" QScript filetype detection
" *.qs sources, and scripts whose first line runs qscript.
autocmd BufNewFile,BufRead *.qs,*.qscript setlocal filetype=qscript
autocmd BufNewFile,BufRead * if getline(1) =~# '^#!.*\<qscript\>' | setlocal filetype=qscript | endif
