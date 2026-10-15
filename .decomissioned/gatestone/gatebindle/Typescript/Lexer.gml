lexer {
    skip WS = "[ \t\r\n]+";
    skip LINE_COMMENT = "//[^\n]*";
    skip BLOCK_COMMENT = "/\\*([^*]|\\*+[^*/])*\\*+/";
    token INTEGER = "[0-9]+";
    token IDENT = "[A-Za-z_$][A-Za-z0-9_$]*";
    token PLUS = "\\+";
    token MINUS = "-";
    token STAR = "\\*";
    token SLASH = "/";
    token PERCENT = "%";
    token SHL = "<<";
    token SHR = ">>";
    token USHR = ">>>";
    token AMP = "&";
    token PIPE = "\\|";
    token CARET = "\\^";
    token TILDE = "~";
    token LPAREN = "\\(";
    token RPAREN = "\\)";
    token ASSIGN = "=";
}
