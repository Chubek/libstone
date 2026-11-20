module MetaVect;

enum versionMajor = 0;
enum versionMinor = 1;
enum versionPatch = 0;

enum Status : int
{
    ok = 0,
    invalidArgument = 1,
    outOfMemory = 2,
    notImplemented = 3,
    syntaxError = 4,
    semanticError = 5,
    backendError = 6,
    limitExceeded = 7
}

enum Backend : int
{
    svg = 0,
    eps = 1
}

struct Output
{
    char* data;
    size_t size; /// Excludes the terminating NUL.
}

struct Diagnostic
{
    size_t offset; /// Zero-based byte offset.
    size_t line; /// One-based source line.
    size_t column; /// One-based byte column.
    const(char)* message; /// Static storage; do not free.
}

// The interface ships without a separate D object file.
pragma(inline, true)
string versionString() @safe pure nothrow { return "0.1.0"; }

extern(C) nothrow @nogc
{
    const(char)* mv_version_string();
    const(char)* mv_status_string(Status status);

    /**
     * Compile exactly sourceSize bytes without retaining source. Source must
     * be non-null but need not be NUL-terminated.
     * Output is required and is initialized even on failure. Release any old
     * result before reuse. On success its data is owned and NUL-terminated.
     * Diagnostic is optional and cleared on success. API argument errors
     * report zero offset, line, and column.
     */
    Status mv_compile(const(char)* source, size_t sourceSize, Backend backend,
                      Output* output, Diagnostic* diagnostic);

    /// Release and clear an initialized output. Null and repeated calls are safe.
    void mv_output_destroy(Output* output);
}
