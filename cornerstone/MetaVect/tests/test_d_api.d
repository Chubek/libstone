module test_d_api;

import MetaVect;
import std.algorithm.searching : canFind, startsWith;
import std.string : fromStringz;

void main()
{
    assert(versionMajor == 0 && versionMinor == 1 && versionPatch == 0);
    assert(versionString() == "0.1.0");
    assert(fromStringz(mv_version_string()) == versionString());
    assert(fromStringz(mv_status_string(Status.notImplemented)) == "not implemented");
    assert(fromStringz(mv_status_string(Status.syntaxError)) == "syntax error");

    enum drawing = `canvas(100,80) { let pos = 1; rect(pos,2,10,20,"#ff0000"); }`;
    Output output;
    Diagnostic diagnostic;
    scope(exit) mv_output_destroy(&output);

    assert(mv_compile(drawing.ptr, drawing.length, Backend.svg, &output, &diagnostic) == Status.ok);
    assert(output.data[0 .. output.size].canFind("<rect"));
    assert(output.data[output.size] == '\0');
    assert(diagnostic.message is null);
    mv_output_destroy(&output);
    assert(output.data is null && output.size == 0);
    mv_output_destroy(&output);

    assert(mv_compile(drawing.ptr, drawing.length, Backend.eps, &output, null) == Status.ok);
    assert(output.data[0 .. output.size].startsWith("%!PS-Adobe-"));
    assert(output.data[output.size] == '\0');
    mv_output_destroy(&output);

    enum malformed = "canvas(100,80) {\n  @\n}";
    assert(mv_compile(malformed.ptr, malformed.length, Backend.svg, &output, &diagnostic) ==
           Status.syntaxError);
    assert(output.data is null && output.size == 0);
    assert(diagnostic.offset == "canvas(100,80) {\n  ".length);
    assert(diagnostic.line == 2 && diagnostic.column == 3);
    assert(diagnostic.message !is null);

    assert(mv_compile(null, 0, Backend.svg, &output, &diagnostic) == Status.invalidArgument);
    assert(output.data is null && output.size == 0);
    assert(diagnostic.offset == 0 && diagnostic.line == 0 && diagnostic.column == 0);
    assert(diagnostic.message !is null);
    mv_output_destroy(null);
}
