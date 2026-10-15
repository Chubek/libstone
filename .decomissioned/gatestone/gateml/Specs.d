module gateml.Specs;
public import gateml.Module;
public import gateml.AbsynSpec;
public import gateml.VisitorSpec;
public import gateml.TargetSpec;

enum formatName = "gateml.frontend";
enum descriptorVersion = 1;

bool identifier(string name) {
    import std.string : startsWith;
    if (!name.length || name.startsWith("__np_")) return false;
    bool letter(char c) { return c == '_' || c >= 'a' && c <= 'z' || c >= 'A' && c <= 'Z'; }
    if (!letter(name[0])) return false;
    foreach (char c; name) if (!letter(c) && !(c >= '0' && c <= '9')) return false;
    foreach (keyword; ["frontend", "lexer", "token", "skip", "parser", "start", "rule", "ast", "node", "adapter", "lower", "profile", "option"])
        if (name == keyword) return false;
    return true;
}
