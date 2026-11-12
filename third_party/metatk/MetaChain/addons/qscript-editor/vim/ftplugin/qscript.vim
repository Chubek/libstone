" Vim filetype plugin
" Language:     QScript
" Maintainer:   MetaChain
" Last Change:  2026 Oct 16
" Options:      g:qscript_executable     the qscript driver (default "qscript")
"               g:qscript_library_paths  list of directories passed as -L
"               g:qscript_no_mappings    do not define the <LocalLeader> mappings

if exists("b:did_ftplugin")
  finish
endif
let b:did_ftplugin = 1

let s:cpo_save = &cpo
set cpo&vim

setlocal comments=s1:/*,mb:*,ex:*/,://,:#
setlocal commentstring=//\ %s
setlocal formatoptions-=t formatoptions+=croql
setlocal iskeyword=@,48-57,_
setlocal suffixesadd=.qs
" `use a, b::c;` -> a.qs, b/c.qs for gf and [i
setlocal include=^\\s*use\\s\\+
setlocal includeexpr=QScriptIncludeExpr(v:fname)
setlocal define=^\\s*\\%(fn\\\|sub\\\|const\\\|let\\\|my\\)\\s\\+
setlocal keywordprg=:QScriptDoc

let b:undo_ftplugin = "setlocal comments< commentstring< formatoptions< iskeyword< suffixesadd< include< includeexpr< define< keywordprg< makeprg< errorformat<"
      \ . " | silent! delcommand -buffer QScriptRun | silent! delcommand -buffer QScriptCheck"
      \ . " | silent! delcommand -buffer QScriptAst | silent! delcommand -buffer QScriptTree"
      \ . " | silent! delcommand -buffer QScriptIR | silent! delcommand -buffer QScriptDoc"
      \ . " | silent! delcommand -buffer QScriptLib | silent! delcommand -buffer QScriptBuild"
      \ . " | silent! delcommand -buffer QScriptCall"

" matchit: jump between if/elsif/else and do/while|until.
if exists("loaded_matchit") || has("nvim")
  let b:match_ignorecase = 0
  let b:match_words = '\<if\>:\<elsif\>:\<else\>,\<do\>:\<\%(while\|until\)\>'
  let b:undo_ftplugin .= " | unlet! b:match_words b:match_ignorecase"
endif

" :make runs `qscript check` and reads file:line:column: message.
function! s:Driver() abort
  let cmd = get(g:, 'qscript_executable', 'qscript')
  for dir in get(g:, 'qscript_library_paths', [])
    let cmd .= ' -L ' . shellescape(dir)
  endfor
  return cmd
endfunction
let &l:makeprg = s:Driver() . ' check %:S'
setlocal errorformat=%f:%l:%c:\ %m,%f:%l:%c\ %m,%-G%.%#

if !exists("*QScriptIncludeExpr")
  function! QScriptIncludeExpr(fname) abort
    let name = substitute(a:fname, '[;,].*$', '', '')
    return substitute(name, '::', '/', 'g') . '.qs'
  endfunction
endif

function! s:Run(subcommand, args, ...) abort
  let file = expand('%:p')
  if &modified
    silent write
  endif
  let cmd = s:Driver() . ' ' . a:subcommand . ' ' . shellescape(file) . (a:args ==# '' ? '' : ' ' . a:args)
  let output = systemlist(cmd)
  let title = a:0 ? a:1 : '[qscript ' . a:subcommand . ']'
  call s:Show(title, output, v:shell_error)
endfunction

function! s:Show(title, lines, status) abort
  let name = '__QScript__'
  let win = bufwinnr(name)
  if win > 0
    execute win . 'wincmd w'
  else
    execute 'botright ' . min([max([len(a:lines), 3]), 15]) . 'new ' . name
    setlocal buftype=nofile bufhidden=wipe noswapfile nobuflisted
  endif
  setlocal modifiable
  silent %delete _
  call setline(1, empty(a:lines) ? ['(no output)'] : a:lines)
  if a:status != 0
    call append(line('$'), '[exit ' . a:status . ']')
  endif
  let &l:statusline = a:title
  setlocal nomodifiable
  wincmd p
endfunction

" :QScriptDoc NAME  documents a keyword, a library (math) or a function
" (gcd, math::gcd) using the driver, so it always matches the installed stdlib.
function! s:Doc(word) abort
  let word = a:word ==# '' ? expand('<cword>') : a:word
  let lib = ''
  let name = word
  if word =~ '::'
    let lib = matchstr(word, '^.*\ze::')
    let name = matchstr(word, '::\zs\h\w*$')
  endif
  let libs = systemlist(s:Driver() . ' libs')
  if lib ==# '' && index(libs, name) >= 0
    call s:Show('[qscript lib ' . name . ']', systemlist(s:Driver() . ' lib ' . shellescape(name)), v:shell_error)
    return
  endif
  let candidates = lib ==# '' ? s:UsedLibraries() + libs : [lib]
  for l in candidates
    let source = systemlist(s:Driver() . ' lib ' . shellescape(l))
    let idx = match(source, '^\s*\%(fn\|sub\)\s\+' . name . '\s*(')
    if idx < 0
      let idx = match(source, '^\s*const\s\+' . name . '\>')
    endif
    if idx >= 0
      let first = idx
      while first > 0 && source[first - 1] =~ '^\s*//'
        let first -= 1
      endwhile
      call s:Show('[' . l . '::' . name . ']', [l . '::' . name] + source[first : idx], 0)
      return
    endif
  endfor
  echohl WarningMsg | echo 'qscript: no documentation for ' . word | echohl None
endfunction

function! s:UsedLibraries() abort
  let libs = []
  for line in getline(1, '$')
    let m = matchlist(line, '^\s*use\s\+\([^;]*\);')
    if !empty(m)
      let libs += map(split(m[1], ','), 'trim(v:val)')
    endif
  endfor
  return libs
endfunction

function! s:LibComplete(lead, line, pos) abort
  return filter(systemlist(s:Driver() . ' libs'), 'v:val =~ "^" . a:lead')
endfunction

command! -buffer -nargs=* QScriptRun   call s:Run('run', <q-args>)
command! -buffer          QScriptCheck call s:Run('check', '')
command! -buffer          QScriptAst   call s:Run('ast', '')
command! -buffer          QScriptTree  call s:Run('tree', '')
command! -buffer          QScriptIR    call s:Run('ir', '')
command! -buffer -nargs=+ QScriptCall  call s:Run('call', '-- ' . <q-args>)
command! -buffer -nargs=1 QScriptBuild call s:Run('build', '-o ' . shellescape(<q-args>))
command! -buffer -nargs=? QScriptDoc   call s:Doc(<q-args>)
command! -buffer -nargs=1 -complete=customlist,s:LibComplete QScriptLib
      \ call s:Show('[qscript lib <args>]', systemlist(s:Driver() . ' lib ' . shellescape(<q-args>)), v:shell_error)

if !get(g:, 'qscript_no_mappings', 0)
  nnoremap <buffer> <silent> <LocalLeader>r :QScriptRun<CR>
  nnoremap <buffer> <silent> <LocalLeader>c :QScriptCheck<CR>
  nnoremap <buffer> <silent> <LocalLeader>a :QScriptAst<CR>
  nnoremap <buffer> <silent> <LocalLeader>i :QScriptIR<CR>
  nnoremap <buffer> <silent> <LocalLeader>d :QScriptDoc<CR>
  let b:undo_ftplugin .= " | silent! nunmap <buffer> <LocalLeader>r | silent! nunmap <buffer> <LocalLeader>c"
        \ . " | silent! nunmap <buffer> <LocalLeader>a | silent! nunmap <buffer> <LocalLeader>i"
        \ . " | silent! nunmap <buffer> <LocalLeader>d"
endif

let &cpo = s:cpo_save
unlet s:cpo_save
" vim: ts=8 sw=2 et
