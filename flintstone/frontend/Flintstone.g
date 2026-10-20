// Flintstone textual IL, version 1. Reference EBNF, not generated parser output.
// The bounded recursive-descent implementation is parser.cpp. Tests exercise
// canonical parse/print round trips. Whitespace and // comments are skipped.
module = 'module' identifier 'level' ('high' | 'core' | 'machine') '{' declaration* '}' EOF;
declaration = 'target' string attributes
            | 'import' string ';'
            | 'types' '{' (identifier '=' type ';')* '}'
            | 'dialect' identifier attributes
            | 'attrs' attributes | function;
function = 'extern'? 'func' '@' identifier '(' parameters? ')' '->' type
           ('attrs' attributes)? (';' | '{' ('entry' block_id ';')? block* '}');
parameters = value_declaration (',' value_declaration)*;
value_declaration = value_id ':' type;
block = block_id string? ':' operation*;
operation = (value_declaration (',' value_declaration)* '=')?
            identifier '(' (operand (',' operand)*)? ')'
            ('to' labels)? ('from' labels)? ('attrs' attributes)? ';';
labels = '[' (block_id (',' block_id)*)? ']';
operand = value_id | integer ':' type | real ':' type | '@' identifier ':' type;
attributes = '{' (identifier '=' attribute ';')* '}';
attribute = integer | real | string | 'true' | 'false' | 'type' '<' type '>';
type = 'void' | 'index' | integer_type | float_type
     | 'ptr' '<' identifier '>' | 'param' '<' identifier '>'
     | 'vector' '<' unsigned_integer ',' type '>'
     | 'tuple' '<' (type (',' type)*)? '>'
     | 'opaque' '<' identifier (',' type)* '>';
value_id = '%' (identifier | unsigned_integer);
block_id = '^' (identifier | unsigned_integer);
// identifier: ASCII [A-Za-z_][A-Za-z_0-9.-]*
// integer: -?[0-9]+, checked int64; real: decimal fraction and/or exponent.
// strings: double-quoted UTF-8 bytes; escapes: \\, \", \n, \r, \t.
