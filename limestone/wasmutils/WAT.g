// Descriptive EBNF for the handwritten parser in WAT.cpp.
document   = module | instruction* EOF ;
module     = '(' 'module' '(' 'func' result* instruction* ')' ')' EOF ;
result     = '(' 'result' ('i32' | 'i64')* ')' ;
instruction = plain | folded ;
folded     = '(' plain instruction* ')' ;
plain      = no_immediate | ('i32.const' | 'i64.const') integer ;

// Mnemonics are single tokens and must be present in opcode_info (WASM.aml).
no_immediate = 'unreachable' | 'nop' | 'drop' | 'select'
             | ('i32.' | 'i64.') ('eqz' | 'clz' | 'ctz' | 'popcnt'
               | 'add' | 'sub' | 'mul' | 'and' | 'or' | 'xor'
               | 'shl' | 'shr_s' | 'shr_u' | 'rotl' | 'rotr') ;

// Each integer is a single token. Its numeric value must be in -64..63.
integer    = ('+' | '-')? (decimal | '0x' hexadecimal) ;
decimal    = digit ('_'? digit)* ;
hexadecimal = hexdigit ('_'? hexdigit)* ;
digit      = '0'..'9' ;
hexdigit   = digit | 'a'..'f' | 'A'..'F' ;

// Space, tab, CR and LF separate tokens. Comments act as separators:
//   ;; through the next CR, LF, or EOF
//   (; through the matching ;), allowing nested (; ... ;) comments
// Folded instruction depth and block comment depth are each limited to 128.
// A wrapper declares exactly one anonymous, zero-argument function. Only
// result annotations are supported, and only at the start of its body.
// Folded operands are emitted left-to-right before the enclosing opcode.
