" Vim indent file
" Language:     QScript
" Maintainer:   MetaChain
" Last Change:  2026 Oct 16
" Options:      g:qscript_indent_continuation  extra indent for a line continuing
"               an unfinished expression (default: shiftwidth())

if exists("b:did_indent")
  finish
endif
let b:did_indent = 1

setlocal indentexpr=GetQScriptIndent(v:lnum)
setlocal indentkeys=0{,0},0),0],!^F,o,O,e,=elsif,=else
setlocal nolisp nosmartindent nocindent autoindent

let b:undo_indent = "setlocal indentexpr< indentkeys< lisp< smartindent< cindent< autoindent<"

if exists("*GetQScriptIndent")
  finish
endif

let s:cpo_save = &cpo
set cpo&vim

" A brace-less control header: its body is the next line only.
let s:header = '^\s*\%(}\s*\)\=\%(\%(if\|elsif\|unless\|while\|until\|for\)\s*(.*)\|else\|do\)\s*\%(//.*\|#.*\)\=$'

function! s:InComment(lnum, col) abort
  return synIDattr(synID(a:lnum, a:col, 1), "name") =~? 'comment'
endfunction

" Line text without trailing comments and whitespace.
function! s:Code(lnum) abort
  let line = getline(a:lnum)
  let line = substitute(line, '\s*\%(//\|#\).*$', '', '')
  let line = substitute(line, '/\*.\{-}\*/', '', 'g')
  return substitute(line, '\s\+$', '', '')
endfunction

" Previous line that holds code (skips blanks and comment-only lines).
function! s:PrevCode(lnum) abort
  let lnum = prevnonblank(a:lnum - 1)
  while lnum > 0
    let text = getline(lnum)
    if text =~ '^\s*\%(//\|#\)' || (text =~ '^\s*\%(\*\|/\*\)' && s:InComment(lnum, match(text, '\S') + 1))
      let lnum = prevnonblank(lnum - 1)
      continue
    endif
    return lnum
  endwhile
  return 0
endfunction

" Net count of unclosed ( [ { on a line, ignoring comments and char literals.
function! s:Balance(text, open, close) abort
  let text = substitute(a:text, "'\\%([^'\\\\]\\|\\\\.\\)\\{-}'", '', 'g')
  return len(substitute(text, '[^' . a:open . ']', '', 'g')) - len(substitute(text, '[^' . a:close . ']', '', 'g'))
endfunction

let s:continued = '\%([-+*/%&|^<>=!?:,]\|\<and\|\<or\|\<not\)$'

" First line of the statement that ends on lnum (follows continuation lines).
function! s:StatementStart(lnum) abort
  let lnum = a:lnum
  let prev = s:PrevCode(lnum)
  while prev > 0 && s:Code(prev) =~ s:continued && s:Code(prev) !~ s:header
    let lnum = prev
    let prev = s:PrevCode(lnum)
  endwhile
  return lnum
endfunction

function! GetQScriptIndent(lnum) abort
  let line = getline(a:lnum)
  let sw = shiftwidth()

  " Inside a block comment: align continuation stars under the opener.
  if s:InComment(a:lnum, 1) && line =~ '^\s*\*'
    let start = search('/\*', 'bnW')
    if start > 0
      return indent(start) + 1
    endif
  endif

  let plnum = s:PrevCode(a:lnum)
  if plnum == 0
    return 0
  endif
  let prev = s:Code(plnum)
  let ind = indent(plnum)

  " A header that closed a brace-less body: back to the header's level.
  let pplnum = s:PrevCode(plnum)
  if pplnum > 0 && s:Code(pplnum) =~ s:header && prev !~ '^\s*{' && prev !~ '{$'
    let ind = indent(pplnum)
    " Chains of brace-less headers (if (a) while (b) x;) unwind fully.
    let p3 = s:PrevCode(pplnum)
    while p3 > 0 && s:Code(p3) =~ s:header && s:Code(p3) !~ '{$'
      let ind = indent(p3)
      let p3 = s:PrevCode(p3)
    endwhile
  endif

  " Openers on the previous line.
  let opened = s:Balance(prev, '[({', '])}')
  if opened > 0 || prev =~ '{$'
    let ind = indent(plnum) + sw
  elseif prev =~ s:header
    let ind = indent(plnum) + sw
  elseif prev =~ ';$' && s:StatementStart(plnum) != plnum && s:Balance(prev, '[({', '])}') == 0
    " End of a statement continued over several lines.
    let ind = indent(s:StatementStart(plnum))
  elseif opened < 0 && prev !~ '^\s*[})\]]'
    " A line that closes more than it opens (e.g. `foo(a,\n b));`): go back
    " to the line holding the matching opener.
    call cursor(plnum, 1)
    let match = searchpair('[({[]', '', '[)}\]]', 'bW', 's:InComment(line("."), col("."))')
    if match > 0
      let ind = indent(match)
    endif
  elseif prev !~ '[;{}]$' && prev !~ s:header && prev !~ '^\s*\%(use\|fn\|sub\|const\)\>' && prev =~ s:continued
    " Unfinished expression: continuation indent, once.
    let pp = pplnum > 0 ? s:Code(pplnum) : ''
    if pp =~ '[;{}]$' || pplnum == 0 || pp =~ s:header
      let ind = indent(plnum) + get(g:, 'qscript_indent_continuation', sw)
    endif
  endif

  " Closers at the start of this line.
  if line =~ '^\s*[})\]]'
    call cursor(a:lnum, 1)
    let col = match(line, '[})\]]') + 1
    call cursor(a:lnum, col)
    let match = searchpair('[({[]', '', '[)}\]]', 'bW', 's:InComment(line("."), col("."))')
    if match > 0
      return indent(match)
    endif
    return max([ind - sw, 0])
  endif
  " `else`/`elsif` line up with their `if` when written on a new line.
  if line =~ '^\s*\%(else\|elsif\)\>'
    let lnum = plnum
    while lnum > 0
      let text = s:Code(lnum)
      if indent(lnum) <= ind && text =~ '^\s*\%(}\s*\)\=\%(if\|elsif\|unless\)\>'
        return indent(lnum)
      endif
      if indent(lnum) < ind - sw
        break
      endif
      let lnum = s:PrevCode(lnum)
    endwhile
    return ind
  endif
  return ind
endfunction

let &cpo = s:cpo_save
unlet s:cpo_save
" vim: ts=8 sw=2 et
