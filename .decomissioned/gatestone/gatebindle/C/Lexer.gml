lexer {
    skip WS = "[ \t\r\n]+";
    skip LINE_COMMENT = "//[^\n]*";
    skip BLOCK_COMMENT = "/\\*([^*]|\\*+[^*/])*\\*+/";
    token INTEGER = "[0-9]+";
    token PLUS = "\\+";
    token MINUS = "-";
    token STAR = "\\*";
    token SLASH = "/";
    token PERCENT = "%";
    token LPAREN = "\\(";
    token RPAREN = "\\)";
}
