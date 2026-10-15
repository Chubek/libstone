lexer {
    skip WS = "[ \t\r\n]+";
    skip LINE_COMMENT = "//[^\n]*";
    skip BRACE_COMMENT = "\\{[^}]*\\}";
    skip PAREN_COMMENT = "\\(\\*([^*]|\\*+[^*)])*\\*+\\)";
    token INTEGER = "[0-9]+";
    token SHL = "(s|S)(h|H)(l|L)";
    token SHR = "(s|S)(h|H)(r|R)";
    token INTDIV = "(d|D)(i|I)(v|V)";
    token MODULO = "(m|M)(o|O)(d|D)";
    token KW_AND = "(a|A)(n|N)(d|D)";
    token KW_OR = "(o|O)(r|R)";
    token KW_XOR = "(x|X)(o|O)(r|R)";
    token KW_NOT = "(n|N)(o|O)(t|T)";
    token IDENT = "[A-Za-z_][A-Za-z0-9_]*";
    token PLUS = "\\+";
    token MINUS = "-";
    token STAR = "\\*";
    token SLASH = "/";
    token LPAREN = "\\(";
    token RPAREN = "\\)";
    token ASSIGN = ":=";
}
