module gateml.Module;
public import gateml.AST : Span, GateError;

struct LexRule { string name, pattern; bool skip; Span span; }
struct Production { string head; string[] body; string node; Span span; }
struct Frontend {
    string name, start;
    LexRule[] lexer;
    Production[] productions;
    string[] nodes;
    string[string] lowering, options;
}

enum tokenAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
char[] tokenCodes(const Frontend frontend) {
    char[] result;
    size_t count;
    foreach (rule; frontend.lexer) {
        if (!rule.skip && count == tokenAlphabet.length)
            throw new GateError("at most 62 named terminals are supported", rule.span);
        result ~= rule.skip ? '\0' : tokenAlphabet[count++];
    }
    return result;
}
