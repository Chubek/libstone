" Vim/Neovim syntax rules for LLSynth language specifications.
if exists('b:current_syntax')
  finish
endif
syn case match

" Top-level and action-language comments. #symbol is not a comment.
syn match llsynthComment /^\s*#\%([[:space:]]\|$\).*$/ contains=@Spell
syn match llsynthComment /\/\/.*$/ contains=@Spell

" .token/.skip regexes are recognized only in these declaration lines.
syn region llsynthRegex start=+\/+ skip=+\\/+ end=+\/+ contained contains=llsynthRegexClass,llsynthRegexEscape
syn match llsynthRegexEscape /\\./ contained
syn region llsynthRegexClass start=/\[/ skip=/\\]/ end=/\]/ contained contains=llsynthRegexEscape

" Directive declarations and special block terminators.
syn match llsynthDirective /^\s*\.\%(language\|version\|summary\|start\|module\|library\|include\|import\|export\|option\|precedence\|skip\|token\|literal\|type\|sumtype\|prodtype\|alias\|state\|symtbl\|exception\|rule\|function\|async\|extern\|sandstone\|init\|ir\|op\|rewrite\|analysis\|backend\|pipeline\|template\|macro\|match\|replace\|when\|priority\|emit\|prologue\|epilogue\|isel\)\>/
syn match llsynthBlockEnd /^\s*\.end\s*$/
syn match llsynthDirective /^\s*\.isel\>/
syn match llsynthRuleEnd /^\s*\.\.\s*$/
syn match llsynthFacet /^\s*'\%(syntax\|semantics\|lower\|analyze\|emit\)\s*:/
syn match llsynthFacetEnd /^\s*'end\s*$/

" Literals and interpolations. Single quotes match only closed same-line literals,
" so that facet markers such as 'semantics: never start a string region.
syn region llsynthString start=/"/ skip=/\\\\\|\\"/ end=/"/ contains=llsynthEscape,llsynthInterpolation
syn match llsynthSingleString /'\%(\\.\|[^'\\]\)*'/ contains=llsynthEscape
syn match llsynthEscape /\\./ contained
syn match llsynthInterpolation /${[A-Za-z_][A-Za-z0-9_]*}/ contained

" Captures, state, symbol tables, exceptions, aliases and function references.
syn match llsynthSigil /[@$#%!&][A-Za-z_][A-Za-z0-9_]*/
syn match llsynthType /\<\%(Int\|Int64\|Float64\|Bool\|String\|Void\|Unit\|Any\|Vec\|Task\|Fn\|Callable\|Op\)\>/
syn match llsynthBoolean /\<\%(true\|false\|nil\|None\)\>/
syn match llsynthKeyword /\<\%(let\|return\|if\|else\|while\|for\|in\|fn\|async\|await\|throw\|grab\|require\|on\|discard\|call\|spawn\|join\)\>/
syn match llsynthBuiltin /\<\%(len\|push\|pop\|top\|emit\|print\|reverse\|min\|max\|abs\|is_finite\|final_stack_height\|sleep\|Throw\)\>\ze\s*(/
syn match llsynthNumber /\<\d\+\%(\.\d*\)\?\%([eE][+-]\?\d\+\)\?\>/
syn match llsynthNumber /\<\.\d\+\%([eE][+-]\?\d\+\)\?\>/
syn match llsynthOperator /\%(->\|=>\|==\|!=\|<=\|>=\|&&\|||\)/
syn match llsynthGrammarOperator /[|*+?%]/
syn match llsynthIdent /\<[A-Z][A-Z0-9_]*\>/

" Special declaration region must come *after* generic matches: later starts win.
" Its matchgroup colors the .token/.skip prefix while allowing contained regexes.
syn region llsynthPatternLine matchgroup=llsynthDirective start=/^\s*\.\%(token\|skip\)\>/ end=/$/ keepend contains=llsynthRegex,llsynthType,llsynthNumber,llsynthIdent,llsynthOperator,llsynthComment

hi def link llsynthComment Comment
hi def link llsynthDirective Statement
hi def link llsynthBlockEnd Statement
hi def link llsynthRuleEnd Statement
hi def link llsynthFacet PreProc
hi def link llsynthFacetEnd PreProc
hi def link llsynthRegex String
hi def link llsynthRegexClass Character
hi def link llsynthRegexEscape SpecialChar
hi def link llsynthString String
hi def link llsynthSingleString String
hi def link llsynthEscape SpecialChar
hi def link llsynthInterpolation Special
hi def link llsynthSigil Identifier
hi def link llsynthType Type
hi def link llsynthBoolean Boolean
hi def link llsynthKeyword Keyword
hi def link llsynthBuiltin Function
hi def link llsynthNumber Number
hi def link llsynthOperator Operator
hi def link llsynthGrammarOperator Operator
hi def link llsynthIdent Constant
let b:current_syntax = 'llsynth'
