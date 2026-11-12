" Vim syntax file
" Language:     QScript (MetaChain integer scripting language)
" Filenames:    *.qs
" Maintainer:   MetaChain
" Last Change:  2026 Oct 16
" Options:      g:qscript_highlight_stdlib     highlight standard-library calls (default 1)
"               g:qscript_highlight_operators  highlight operators (default 0)
"               g:qscript_no_fold              disable syntax folding of { } blocks

if exists("b:current_syntax")
  finish
endif

let s:cpo_save = &cpo
set cpo&vim

syn case match
syn iskeyword @,48-57,_

" Comments: // and # to end of line, /* */ blocks.
syn keyword qscriptTodo          contained TODO FIXME XXX NOTE
syn region  qscriptLineComment   start="//" end="$" keepend contains=qscriptTodo,@Spell
syn region  qscriptLineComment   start="#"  end="$" keepend contains=qscriptTodo,@Spell
syn region  qscriptBlockComment  start="/\*" end="\*/" contains=qscriptTodo,@Spell fold extend
syn match   qscriptShebang       "\%^#!.*$"

" Keywords.
syn keyword qscriptDeclare       let my const nextgroup=qscriptBinding skipwhite
syn keyword qscriptConditional   if elsif else unless
syn keyword qscriptRepeat        while until for loop do
syn keyword qscriptJump          break last continue next return exit
syn keyword qscriptStatement     print say assert
syn keyword qscriptWordOperator  and or not
syn keyword qscriptBoolean       true false
syn keyword qscriptDefine        fn sub nextgroup=qscriptFunctionName skipwhite
syn keyword qscriptInclude       use nextgroup=qscriptLibraryList skipwhite

syn match   qscriptBinding       contained "\<\h\w*\>"
syn match   qscriptFunctionName  contained "\<\h\w*\>"
syn match   qscriptLibraryList   contained "[^;]*" contains=qscriptLibraryName,qscriptLineComment,qscriptBlockComment
syn match   qscriptLibraryName   contained "\<\h\w*\%(::\h\w*\)*\>"

" Numbers: decimal, 0x, 0b, 0o, with _ separators. Anything else that starts
" with a digit is an error; valid forms are defined later and so win.
syn match   qscriptNumberError   "\<\d\w*\>"
syn match   qscriptNumber        "\<\d[0-9_]*\>"
syn match   qscriptNumber        "\<0[xX][0-9a-fA-F_]\+\>"
syn match   qscriptNumber        "\<0[bB][01_]\+\>"
syn match   qscriptNumber        "\<0[oO][0-7_]\+\>"

" Character literals: 'a', '\n', '\x41'.
syn match   qscriptSpecialChar   contained "\\\%([nrt0\\'\"]\|x\x\x\)"
syn match   qscriptCharacter     "'\%([^'\\]\|\\\%([nrt0\\'\"]\|x\x\x\)\)'" contains=qscriptSpecialChar
syn match   qscriptCharError     "'\%([^'\\]\|\\.\)\{2,}'"
syn match   qscriptString        "\"[^"]*\"" " no strings in QScript; flag them

" Constants by convention: ALL_CAPS names.
syn match   qscriptConstant      "\<\u[A-Z0-9_]*\>"

" Qualified names lib::name and calls.
syn match   qscriptQualifier     "\<\h\w*\%(::\h\w*\)*::" nextgroup=qscriptQualifiedCall,qscriptConstant
syn match   qscriptQualifiedCall contained "\h\w*\ze\s*("
syn match   qscriptCall          "\<\h\w*\ze\s*(" contains=qscriptStdlibFunction,qscriptBuiltinBad

" Keywords that are often mistaken for function names.
syn match   qscriptBuiltinBad    contained "\<\%(and\|or\|not\)\ze\s*("

" Operators.
if get(g:, "qscript_highlight_operators", 0)
  syn match qscriptOperator      "\*\*=\?\|<<=\?\|>>=\?\|&&\|||\|[-+*/%&|^]=\?\|[=!<>]=\|[<>=!~?:]\|++\|--"
endif

" Blocks and folding.
if !get(g:, "qscript_no_fold", 0)
  syn region qscriptBlock        start="{" end="}" transparent fold
endif
syn match   qscriptBraceError    "}"
syn cluster qscriptTop contains=TOP

" Standard library names (generated list).
if get(g:, "qscript_highlight_stdlib", 1)
  runtime! syntax/qscript_stdlib.vim
endif

syn sync fromstart

hi def link qscriptTodo          Todo
hi def link qscriptLineComment   Comment
hi def link qscriptBlockComment  Comment
hi def link qscriptShebang       PreProc
hi def link qscriptDeclare       StorageClass
hi def link qscriptConditional   Conditional
hi def link qscriptRepeat        Repeat
hi def link qscriptJump          Statement
hi def link qscriptStatement     Statement
hi def link qscriptWordOperator  Operator
hi def link qscriptOperator      Operator
hi def link qscriptBoolean       Boolean
hi def link qscriptDefine        Keyword
hi def link qscriptInclude       Include
hi def link qscriptLibraryName   Identifier
hi def link qscriptStdlibName    Identifier
hi def link qscriptFunctionName  Function
hi def link qscriptBinding       Identifier
hi def link qscriptNumber        Number
hi def link qscriptCharacter     Character
hi def link qscriptSpecialChar   SpecialChar
hi def link qscriptConstant      Constant
hi def link qscriptQualifier     Type
hi def link qscriptQualifiedCall Function
hi def link qscriptStdlibFunction Function
hi def link qscriptNumberError   Error
hi def link qscriptCharError     Error
hi def link qscriptString        Error
hi def link qscriptBuiltinBad    Error
hi def link qscriptBraceError    Error

let b:current_syntax = "qscript"

let &cpo = s:cpo_save
unlet s:cpo_save
" vim: ts=8 sw=2 et
