" Lightweight, deterministic indenting for LLSynth specifications.
if exists('b:did_indent')
  finish
endif
let b:did_indent = 1

setlocal autoindent
setlocal indentexpr=GetLLSynthIndent(v:lnum)
setlocal indentkeys=0{,0},0),0],0.,0',!^F,o,O
let b:undo_indent = 'setlocal autoindent< indentexpr< indentkeys<' 

if exists('*GetLLSynthIndent')
  finish
endif

function! GetLLSynthIndent(lnum) abort
  let l:sw = shiftwidth()
  let l:cur = getline(a:lnum)
  let l:pnum = prevnonblank(a:lnum - 1)
  if l:pnum == 0
    return 0
  endif
  let l:prev = getline(l:pnum)
  let l:base = indent(l:pnum)

  " Section terminators always return to column zero.
  if l:cur =~# '^\s*\.\.\s*$' || l:cur =~# '^\s*\.end\s*$'
    return 0
  endif

  " Nested directives in .ir/.rewrite/.backend sections.
  if l:cur =~# '^\s*\.\%(op\|match\|replace\|when\|priority\|emit\|prologue\|epilogue\)\>'
    return l:sw
  endif

  " All other directives begin at top level (including new blocks).
  if l:cur =~# '^\s*\.[A-Za-z]'
    return 0
  endif

  " Rule facet headers and their optional 'end marker.
  if l:cur =~# "^\s*'\%(syntax\|semantics\|lower\|analyze\|emit\)\s*:" || l:cur =~# "^\s*'end\>"
    return l:sw
  endif

  " The first content line after any block or facet starts one level deeper.
  if l:prev =~# '^\s*\.\%(rule\|sumtype\|prodtype\|ir\|isel\|rewrite\|backend\|analysis\|pipeline\|init\|macro\|function\|async\)\>'
    return l:sw
  endif
  if l:prev =~# "^\s*'\%(syntax\|semantics\|lower\|analyze\|emit\)\s*:"
    return l:sw * 2
  endif
  if l:prev =~# '^\s*\.end\s*$' || l:prev =~# '^\s*\.\.\s*$'
    return 0
  endif

  " Common multiline action-language brackets (heuristic, not a parser).
  if l:cur =~# '^\s*[}\])]' || l:cur =~# '^\s*else\>'
    return max([0, l:base - l:sw])
  endif
  if l:prev =~# '[{\[(]\s*$'
    return l:base + l:sw
  endif

  return l:base
endfunction
