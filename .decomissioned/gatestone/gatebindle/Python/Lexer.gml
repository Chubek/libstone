lexer {
    skip WS = "[ \t\r\n]+";
    skip HASH_COMMENT = "#[^\n]*";
    token INTEGER = "[0-9]+";
    token IDENT = "[A-Za-z_][A-Za-z0-9_]*";
    token PLUS = "\\+";
    token MINUS = "-";
    token STARSTAR = "\\*\\*";
    token STAR = "\\*";
    token DSLASH = "//";
    token SLASH = "/";
    token PERCENT = "%";
    token SHL = "<<";
    token SHR = ">>";
    token AMP = "&";
    token PIPE = "\\|";
    token CARET = "\\^";
    token TILDE = "~";
    token LPAREN = "\\(";
    token RPAREN = "\\)";
    token ASSIGN = "=";
}
