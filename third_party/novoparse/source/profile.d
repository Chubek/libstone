module profile;

import ast : Diagnostic, Location;
import sexp : Sexp, readSexp;
import std.file : readText;

struct Profile
{
    string name;
    string extension;
    string backend;
    string templateText;
}

Profile readProfile(string source, string file = "<profile>")
{
    auto root = readSexp(source, file);
    if (!root.isList || root.children.length < 2)
        throw new Diagnostic(root.location, "expected (profile ...)");
    Sexp versionField;
    foreach (field; root.children[1 .. $])
    {
        if (!field.isList || field.children.length != 2) continue;
        auto head = field.children[0];
        if (!head.isList && !head.quoted && head.text == "version") versionField = field;
    }
    if (versionField is null) throw new Diagnostic(root.location, "missing profile field: version");
    if (versionField.children[1].atom() != "1") throw new Diagnostic(root.location, "unsupported profile version");
    root.form("profile", 6);
    string[string] fields;
    foreach (field; root.children[1 .. $])
    {
        if (!field.isList || field.children.length != 2) throw new Diagnostic(field.location, "expected (field value) in profile");
        auto name = field.children[0].atom();
        if (name in fields) throw new Diagnostic(field.location, "duplicate profile field: " ~ name);
        fields[name] = field.children[1].atom();
    }
    string get(string name)
    {
        auto found = name in fields;
        if (found is null) throw new Diagnostic(root.location, "missing profile field: " ~ name);
        return *found;
    }
    if (get("version") != "1") throw new Diagnostic(root.location, "unsupported profile version");
    auto result = Profile(get("name"), get("extension"), get("backend"), get("template"));
    if (!result.name.length || !result.extension.length)
        throw new Diagnostic(root.location, "profile name and extension must not be empty");
    if (!result.templateText.length)
        throw new Diagnostic(root.location, "profile template must not be empty");
    foreach (c; result.name ~ result.extension ~ result.templateText)
        if (c == '\0') throw new Diagnostic(root.location, "profile field contains a NUL byte");
    if (result.backend != "c" && result.backend != "d" && result.backend != "python")
        throw new Diagnostic(root.location, "unknown profile backend: " ~ result.backend);
    return result;
}

Profile loadProfile(string name)
{
    switch (name)
    {
        case "C": return readProfile(import("C.sexp"), "profiles/C.sexp");
        case "C++": return readProfile(import("C++.sexp"), "profiles/C++.sexp");
        case "D": return readProfile(import("D.sexp"), "profiles/D.sexp");
        case "Python": return readProfile(import("Python.sexp"), "profiles/Python.sexp");
        default:
        {
            string text;
            try { text = readText(name); }
            catch (Exception e) { throw new Diagnostic(Location(name, 1, 1), "cannot read profile: " ~ e.msg); }
            if (!text.length) throw new Diagnostic(Location(name, 1, 1), "profile is empty or unimplemented");
            return readProfile(text, name);
        }
    }
}
