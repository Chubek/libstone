module gateml.Bundle;
import gateml.AST;
import gateml.Module;
import gateml.Compiler;
import std.file : isDir, readText;
import std.path : buildPath, baseName, extension;
import std.string : splitLines, strip;

Frontend load(string path) {
    import std.file : FileException;
    if (!path.length) throw new GateError("empty definition path");
    foreach (char c; path) if (!c) throw new GateError("NUL in definition path");
    string readDefinition(string filename) {
        try { return readText(filename); }
        catch (FileException error) { throw new GateError("cannot read " ~ filename ~ ": " ~ error.msg, Span.init, filename); }
    }
    bool directory;
    try { directory = isDir(path); }
    catch (FileException error) { throw new GateError("cannot inspect " ~ path ~ ": " ~ error.msg, Span.init, path); }
    if (!directory) {
        try { return compile(readDefinition(path)); }
        catch (GateError error) { if (!error.source.length) error.source = path; throw error; }
    }
    struct Part { string path, text; size_t begin; }
    Part[] parts; string source; bool[string] seen;
    string manifest;
    try { manifest = readText(buildPath(path, "MANIFEST")); }
    catch (FileException error) { throw new GateError("cannot read bundle MANIFEST: " ~ error.msg, Span.init, buildPath(path, "MANIFEST")); }
    auto manifestPath = buildPath(path, "MANIFEST");
    size_t manifestOffset;
    foreach (line; manifest.splitLines()) {
        auto entrySpan = location(manifest, manifestOffset, manifestOffset + line.length);
        manifestOffset += line.length;
        // splitLines strips CR/LF delimiters, including CRLF pairs.
        if (manifestOffset < manifest.length && manifest[manifestOffset] == '\r') ++manifestOffset;
        if (manifestOffset < manifest.length && manifest[manifestOffset] == '\n') ++manifestOffset;
        auto entry = line.strip;
        if (!entry.length || entry[0] == '#') continue;
        foreach (char c; entry) if (!c) throw new GateError("NUL in MANIFEST entry", entrySpan, manifestPath);
        if (baseName(entry) != entry || extension(entry) != ".gml" || entry == ".gml" || entry[0] == '/' || entry in seen)
            throw new GateError("MANIFEST entries must be unique local .gml filenames: " ~ entry, entrySpan, manifestPath);
        seen[entry] = true;
        auto filename = buildPath(path, entry); auto text = readDefinition(filename);
        parts ~= Part(filename, text, source.length); source ~= text ~ "\n";
    }
    try { return compile(source); }
    catch (GateError error) {
        foreach_reverse (part; parts) if (error.span.begin >= part.begin) {
            error.span = location(part.text, error.span.begin - part.begin, error.span.end - part.begin);
            error.source = part.path; break;
        }
        throw error;
    }
}
