#include "MetaVect.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

static void usage(FILE *stream) {
    fputs("Usage: metavectc [--backend svg|eps] [-o OUTPUT] INPUT\n"
          "Use - for stdin or stdout. Defaults: backend svg, output stdout.\n",
          stream);
}

static int cli_error(const char *message) {
    fprintf(stderr, "metavectc: %s\n", message);
    usage(stderr);
    return EXIT_FAILURE;
}

static int stream_error(const char *name, const char *message) {
    fprintf(stderr, "%s: error: %s\n", name, message);
    return 0;
}

static int binary_mode(FILE *stream) {
#ifdef _WIN32
    return _setmode(_fileno(stream), _O_BINARY) != -1;
#else
    (void)stream;
    return 1;
#endif
}

/* Read the named stream once, without seeking (stdin may be a pipe). */
static int read_source(const char *path, const char *name, char **data, size_t *size) {
    const int from_stdin = strcmp(path, "-") == 0;
    FILE *stream = from_stdin ? stdin : fopen(path, "rb");
    char chunk[8192];
    size_t capacity = 0;
    int ok = 1;

    if (!stream)
        return stream_error(name, "cannot open input");
    if (from_stdin && !binary_mode(stream))
        return stream_error(name, "cannot set binary input mode");

    for (;;) {
        const size_t count = fread(chunk, 1, sizeof chunk, stream);
        if (ferror(stream)) {
            ok = stream_error(name, "cannot read input");
            break;
        }
        if (count > SIZE_MAX - *size) {
            ok = stream_error(name, "input is too large");
            break;
        }
        if (*size + count > capacity) {
            size_t next = capacity == 0 ? sizeof chunk :
                          capacity <= SIZE_MAX / 2 ? 2 * capacity : SIZE_MAX;
            char *buffer;
            if (next < *size + count)
                next = *size + count;
            buffer = realloc(*data, next);
            if (!buffer) {
                ok = stream_error(name, "out of memory while reading input");
                break;
            }
            *data = buffer;
            capacity = next;
        }
        if (count != 0) {
            memcpy(*data + *size, chunk, count);
            *size += count;
        }
        if (count != sizeof chunk)
            break;
    }

    if (!from_stdin && fclose(stream) != 0 && ok)
        ok = stream_error(name, "cannot close input");
    return ok;
}

static int write_output(const char *path, const mv_output *result) {
    const int to_stdout = strcmp(path, "-") == 0;
    const char *name = to_stdout ? "<stdout>" : path;
    FILE *stream = to_stdout ? stdout : fopen(path, "wb");
    int written, flushed;

    if (!stream)
        return stream_error(name, "cannot open output");
    if (to_stdout && !binary_mode(stream))
        return stream_error(name, "cannot set binary output mode");

    written = fwrite(result->data, 1, result->size, stream) == result->size;
    flushed = (to_stdout ? fflush(stream) : fclose(stream)) == 0;
    if (!written || !flushed)
        return stream_error(name, "cannot write output");
    return 1;
}

int main(int argc, char **argv) {
    mv_backend backend = MV_BACKEND_SVG;
    const char *input = NULL;
    const char *output = "-";
    const char *input_name;
    int options = 1;
    char *source = NULL;
    size_t source_size = 0;
    mv_output result;
    mv_diagnostic diagnostic;
    mv_status status;
    int ok;

    for (int i = 1; i < argc; ++i) {
        const char *arg = argv[i];
        if (options && strcmp(arg, "--") == 0) {
            options = 0;
        } else if (options && strcmp(arg, "--help") == 0) {
            usage(stdout);
            return EXIT_SUCCESS;
        } else if (options && strcmp(arg, "--backend") == 0) {
            if (++i == argc)
                return cli_error("missing argument for --backend");
            if (strcmp(argv[i], "svg") == 0)
                backend = MV_BACKEND_SVG;
            else if (strcmp(argv[i], "eps") == 0)
                backend = MV_BACKEND_EPS;
            else
                return cli_error("backend must be svg or eps");
        } else if (options && strcmp(arg, "-o") == 0) {
            if (++i == argc)
                return cli_error("missing argument for -o");
            output = argv[i];
        } else if (options && arg[0] == '-' && arg[1] != '\0') {
            return cli_error("unknown option");
        } else if (input) {
            return cli_error("exactly one input is required");
        } else {
            input = arg;
        }
    }
    if (!input)
        return cli_error("input is required");

    input_name = strcmp(input, "-") == 0 ? "<stdin>" : input;
    if (!read_source(input, input_name, &source, &source_size)) {
        free(source);
        return EXIT_FAILURE;
    }
    status = mv_compile(source ? source : "", source_size, backend, &result, &diagnostic);
    free(source);
    if (status != MV_STATUS_OK) {
        const char *message = diagnostic.message ? diagnostic.message : mv_status_string(status);
        if (diagnostic.line != 0)
            fprintf(stderr, "%s:%zu:%zu: error: %s\n", input_name,
                    diagnostic.line, diagnostic.column, message);
        else
            stream_error(input_name, message);
        return EXIT_FAILURE;
    }

    ok = write_output(output, &result);
    mv_output_destroy(&result);
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
