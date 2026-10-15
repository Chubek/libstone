lexer {
    skip WS = "[ \t\r\n]+";
    skip LINE_COMMENT = ";[^\n]*";
    token INTEGER = "[+-]?[0-9]+";
    token PLUS = "\\+";
    token MINUS = "-";
    token STAR = "\\*";
    token QUOTIENT = "quotient";
    token REMAINDER = "remainder";
    token IDENT = "[A-Za-z_!?][A-Za-z0-9_!?-]*";
    token LPAREN = "\\(";
    token RPAREN = "\\)";
}
