// NovoParse grammar over a one-byte token tape produced by libchomsky3.
// A..M are reserved words; i/s are identifier/string; > represents =>.
%grammar GateML;
%start document;
document: "A" "i" ";" section* => Frontend;
section: "B" "{" lexdecl* "}" => Lexer
       | "E" "{" parserdecl* "}" => Parser
       | "H" "{" nodedecl* "}" => Ast
       | "J" "{" lowerdecl* "}" => Adapter
       | "L" "{" optiondecl* "}" => Profile;
lexdecl: "C" "i" "=" "s" ";" => Token
       | "D" "i" "=" "s" ";" => Skip;
parserdecl: "F" "i" ";" => Start
          | "G" "i" "=" alternative ("|" alternative)* ";" => Rule;
alternative: "i"* ">" "i" => Alternative;
nodedecl: "I" "i" ";" => Node;
lowerdecl: "K" "i" "=" "i" ";" => Lower;
optiondecl: "M" "i" "=" "s" ";" => Option;
