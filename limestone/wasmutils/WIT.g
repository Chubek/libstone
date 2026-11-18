(* Reference EBNF for the handwritten lexer/parser in WIT.cpp.
   This is a standalone, scalar-only subset of WIT. *)

document      = [ package_decl ], interface_decl, { interface_decl }, EOF ;
package_decl  = "package", name, ":", name, [ "@", version ], ";" ;
interface_decl = "interface", name, "{", { function_decl }, "}" ;
function_decl = name, ":", "func", "(", [ parameters ], ")",
                [ "->", scalar ], ";" ;
parameters    = parameter, { ",", parameter }, [ "," ] ;
parameter     = name, ":", scalar ;
scalar        = "bool" | "u8" | "s8" | "u16" | "s16" | "u32" | "s32"
              | "u64" | "s64" | "f32" | "f64" | "char" ;

(* Lexical productions: no spaces within a name or version. WIT keywords
   cannot be used as bare names. Names in a scope are unique after case
   folding and removing hyphens, including parameters within a function. *)
name          = [ "%" ], first_part, { "-", name_part } ;
first_part    = lowercase, { lowercase | digit }
              | uppercase, { uppercase | digit } ;
name_part     = ( lowercase | digit ), { lowercase | digit }
              | ( uppercase | digit ), { uppercase | digit } ;
version       = decimal, ".", decimal, ".", decimal ;
decimal       = "0" | nonzero_digit, { digit } ;

(* Printable ASCII plus space, tab, LF and CR. The lexer skips whitespace,
   // through the next LF or EOF, and balanced nested block comments. *)
