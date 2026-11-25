(* Reference EBNF for the handwritten WIT 0.2 parser in WIT.cpp. *)

document        = [ annotations, package_decl ], { annotations, declaration }, EOF ;
package_decl    = "package", name, ":", name, [ "@", version ], ";" ;
declaration     = interface_decl | world_decl | standalone_use ;

standalone_use  = "use", path, [ "as", name ], ";" ;
path            = name, [ ":", name, "/", name, [ "@", version ] ] ;
scoped_use      = "use", path, ".", "{", [ use_names ], "}", ";" ;
use_names       = use_name, { ",", use_name }, [ "," ] ;
use_name        = name, [ "as", name ] ;

interface_decl  = "interface", name, interface_body ;
interface_body  = "{", { annotations, interface_item }, "}" ;
interface_item  = scoped_use | type_def | function ;
function        = name, ":", "func", signature, ";" ;
signature       = "(", [ parameters ], ")", [ "->", type ] ;
parameters      = parameter, { ",", parameter }, [ "," ] ;
parameter       = annotations, name, ":", type ;

world_decl      = "world", name, "{", { annotations, world_item }, "}" ;
world_item      = scoped_use | type_def | include | extern ;
extern          = ( "import" | "export" ),
                  ( path, ";" | function | name, ":", "interface", interface_body, [ ";" ] ) ;
include         = "include", path,
                  ( ";" | "with", "{", [ renames ], "}", [ ";" ] ) ;
renames         = rename, { ",", rename }, [ "," ] ;
rename          = name, "as", name ;

type_def        = "type", name, "=", type, ";"
                | "record", name, "{", [ parameters ], "}"
                | "variant", name, "{", variant_cases, "}"
                | "enum", name, "{", named_cases, "}"
                | "flags", name, "{", [ named_cases ], "}"
                | "resource", name, ( ";" | "{", { annotations, resource_item }, "}" ) ;
variant_cases   = variant_case, { ",", variant_case }, [ "," ] ;
variant_case    = annotations, name, [ "(", type, ")" ] ;
named_cases     = named_case, { ",", named_case }, [ "," ] ;
named_case      = annotations, name ;
resource_item   = "constructor", "(", [ parameters ], ")", ";"
                | name, ":", [ "static" ], "func", signature, ";" ;

type            = scalar | "string" | name
                | ( "list" | "option" ), "<", type, ">"
                | "tuple", "<", type, { ",", type }, [ "," ], ">"
                | "result", [ "<", ( type, [ ",", type ] | "_", ",", type ), ">" ]
                | ( "own" | "borrow" ), "<", name, ">" ;
scalar          = "bool" | "u8" | "s8" | "u16" | "s16" | "u32" | "s32"
                | "u64" | "s64" | "f32" | "f64" | "char" ;

annotations     = { doc_comment | attribute } ;
attribute       = "@", attribute_name, [ "(", [ attribute_args ], ")" ] ;
attribute_args  = attribute_arg, { ",", attribute_arg }, [ "," ] ;
attribute_arg   = attribute_name, "=", attribute_value ;
attribute_name  = name ; (* Keywords are allowed without '%' here. *)

(* Lexical productions. Names use ASCII kebab-case; keywords must be
   escaped with '%' except as attribute names. Attribute values are kept
   as source spellings: identifier, number/SemVer token or quoted string.
   Version tokens use SemVer 2.0.0, without spaces. Ordinary // comments
   and nested /* comments */ are skipped; /// and /** docs */ are retained.
   UTF-8 is allowed in comments and attribute strings. *)
name            = [ "%" ], first_part, { "-", name_part } ;
first_part      = lowercase, { lowercase | digit }
                | uppercase, { uppercase | digit } ;
name_part       = ( lowercase | digit ), { lowercase | digit }
                | ( uppercase | digit ), { uppercase | digit } ;
version         = decimal, ".", decimal, ".", decimal, [ "-", prerelease ], [ "+", build ] ;
decimal         = "0" | nonzero_digit, { digit } ;

(* Semantic validation additionally checks scopes/duplicates, resource
   handles, recursive type definitions, local references and world include
   composition. Foreign package paths are preserved but not resolved.
   Attributes are recorded without applying feature or version policy.
   Asynchronous functions, future/stream types, and post-0.2 constructs
   are not part of this grammar. *)
