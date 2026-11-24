#include <cephyr/Cephyr.h>
#include <cephyr/CephyrSDK.h>

#include <dirent.h>
#include <stdio.h>
#include <string.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "line %d: %s\n", __LINE__, #condition); \
    return 1; \
} } while (0)

static int test_parse_c11(void)
{
    const char *const args[] = {"-DVALUE=42", "-"};
    const char *const compile_args[] = {"-S", "-o", "-", "-x", "c11-ast-json", "-"};
    const char source[] = "int embedding_ast(void) { return VALUE; }\n";
    cephyr_request request = {
        .struct_size = offsetof(cephyr_request, lua), /* legacy request prefix */
        .argc = sizeof(args) / sizeof(*args), .argv = args,
        .stdin_data = source, .stdin_size = sizeof(source) - 1
    };
    cephyr_result *ast = NULL;
    CHECK(CEPHYR_C11_SYNTAX_SCHEMA_VERSION == 1);
    CHECK(cephyr_parse_c11(&request, CEPHYR_AST_JSON, &ast) == CEPHYR_OK && ast);
    size_t bytes = 0;
    const char *text = cephyr_result_output(ast, &bytes);
    CHECK(text && bytes && strstr(text, "\"schema_version\": 1") && strstr(text, "\"42\""));
    cephyr_result *compiled = ast;
    CHECK(cephyr_parse_c11(&request, (cephyr_ast_format)0, &compiled) ==
          CEPHYR_INVALID_ARGUMENT && !compiled);
    request.argc = sizeof(compile_args) / sizeof(*compile_args);
    request.argv = compile_args;
    request.stdin_data = text;
    request.stdin_size = bytes;
    CHECK(cephyr_compile(&request, &compiled) == CEPHYR_OK && compiled);
    CHECK(strstr(cephyr_result_output(compiled, NULL), "embedding_ast:"));
    cephyr_result_destroy(compiled);
    cephyr_result_destroy(ast);
    return 0;
}

static int test_context(void)
{
    char define[] = "-DCEPHYR_VALUE=19";
    const char *args[] = {"-E", "-P", define, "-"};
    struct {
        cephyr_context_options options;
        size_t future_field;
    } extended = {{sizeof(extended), sizeof(args) / sizeof(*args), args, NULL, NULL}, 42};
    cephyr_context *context = NULL;
    CHECK(cephyr_context_create(&extended.options, &context) == CEPHYR_OK && context);

    /* Neither the pointer array nor the strings may be borrowed by the context. */
    memset(define, 'X', sizeof(define) - 1);
    args[0] = args[2] = NULL;
    const char first_source[] = "#define TEMPORARY_MACRO 7\nint value = CEPHYR_VALUE;\n";
    const char second_source[] = "#ifdef TEMPORARY_MACRO\n#error leaked macro\n#endif\nint other;\n";
    cephyr_result *first = NULL;
    cephyr_result *result = NULL;
    size_t size;
    CHECK(cephyr_context_compile(context, first_source, sizeof(first_source) - 1, NULL, &first) ==
          CEPHYR_OK && first);
    const char *first_output = cephyr_result_output(first, &size);
    CHECK(size && strstr(first_output, "int value") && strstr(first_output, "19") &&
          !strstr(first_output, "CEPHYR_VALUE"));

    const char embedded_nul[] = {'i', 'n', 't', '\0', 'x', ';'};
    result = first;
    CHECK(cephyr_context_compile(context, embedded_nul, sizeof(embedded_nul), NULL, &result) ==
          CEPHYR_COMPILE_ERROR && result && result != first);
    CHECK(strstr(cephyr_result_diagnostics(result, NULL), "NUL"));
    cephyr_result_destroy(result);

    result = first;
    CHECK(cephyr_context_compile(context, NULL, 1, NULL, &result) == CEPHYR_INVALID_ARGUMENT && !result);
    CHECK(cephyr_context_compile(context, second_source, sizeof(second_source) - 1, NULL, &result) ==
          CEPHYR_OK && result);
    cephyr_context_destroy(context);
    CHECK(strstr(first_output, "19"));
    CHECK(strstr(cephyr_result_output(result, NULL), "int other;"));
    cephyr_result_destroy(result);
    cephyr_result_destroy(first);

    cephyr_context_options invalid = extended.options;
    CHECK(cephyr_context_create(&invalid, &context) == CEPHYR_INVALID_ARGUMENT && !context);
    invalid.struct_size = sizeof(invalid) - 1;
    CHECK(cephyr_context_create(&invalid, &context) == CEPHYR_INVALID_ARGUMENT && !context);
    CHECK(cephyr_context_create(NULL, &context) == CEPHYR_INVALID_ARGUMENT && !context);
    CHECK(cephyr_context_create(&extended.options, NULL) == CEPHYR_INVALID_ARGUMENT);
    CHECK(cephyr_context_compile(NULL, NULL, 0, NULL, &result) == CEPHYR_INVALID_ARGUMENT && !result);
    cephyr_context_destroy(NULL);

    /* Mutating either supplied root to empty would cause invalid argument if borrowed. */
    const char *syntax[] = {"-fsyntax-only", "-"};
    for (unsigned i = 0; i < 2; ++i) {
        char missing_root[] = "/proc/self/cephyr-nonexistent-root";
        const cephyr_context_options options = {
            sizeof(options), sizeof(syntax) / sizeof(*syntax), syntax,
            i ? NULL : missing_root, i ? missing_root : NULL
        };
        CHECK(cephyr_context_create(&options, &context) == CEPHYR_OK && context);
        missing_root[0] = '\0';
        CHECK(cephyr_context_compile(context, NULL, 0, NULL, &result) == CEPHYR_COMPILE_ERROR && result);
        CHECK(strstr(cephyr_result_diagnostics(result, NULL), "cephyr.target"));
        cephyr_result_destroy(result);
        cephyr_context_destroy(context);
    }
    return 0;
}

static int check_elf(const cephyr_result *result, unsigned type)
{
    size_t size;
    const unsigned char *bytes = (const unsigned char *)cephyr_result_output(result, &size);
    CHECK(bytes && size >= 64 && bytes[size] == '\0');
    /* ELF64, little-endian, including embedded NUL bytes. */
    CHECK(memcmp(bytes, "\x7f" "ELF\x02\x01\x01", 7) == 0 && memchr(bytes, '\0', size));
    CHECK(bytes[16] == type && bytes[17] == 0);
    return 0;
}

static unsigned fd_count(void)
{
    DIR *directory = opendir("/proc/self/fd");
    unsigned count = 0;
    if (directory) {
        while (readdir(directory))
            ++count;
        closedir(directory);
    }
    return count;
}

static int test_native_artifacts(void)
{
    const char *object_args[] = {"--target=x86_64-linux-gnu", "-c", "-x", "c", "-", "-o", "-"};
    const char *shared_args[] = {
        "--target=auto", "-shared", "--stdlib=none", "-nostdlib", "-x", "c", "-", "-o", "-",
        "-Wl,-init,cephyr_test_init,-fini,cephyr_test_fini,--defsym,cephyr_test_null=0"
    };
    const char first_source[] =
        "int cephyr_test_value = 12;\n"
        "static int cephyr_test_hidden(void) { return cephyr_test_value; }\n"
        "int cephyr_test_answer(void) { return cephyr_test_hidden(); }\n"
        "static int *observer;\n"
        "void cephyr_test_observe(int *p) { observer = p; }\n"
        "void cephyr_test_init(void) { cephyr_test_value = 31; }\n"
        "void cephyr_test_fini(void) { if (observer) *observer = 73; }\n";
    const char second_source[] = "int cephyr_test_answer(void) { return 52; }\n"
                                 "void cephyr_test_init(void) {}\n"
                                 "void cephyr_test_fini(void) {}\n";
    /* This symbol exists in the first open module but must not be globally visible. */
    const char unresolved[] =
        "extern void cephyr_test_observe(int *);\n"
        "void cephyr_test_init(void) { cephyr_test_observe(0); }\n"
        "void cephyr_test_fini(void) {}\n";
    const char broken[] = "int cephyr_test_answer(void) { return +; }\n";

    cephyr_request request = {
        .struct_size = sizeof(request), .argc = sizeof(object_args) / sizeof(*object_args),
        .argv = object_args, .stdin_data = second_source, .stdin_size = sizeof(second_source) - 1
    };
    cephyr_result *artifact = NULL;
    cephyr_result *details = NULL;
    cephyr_module *module = NULL;
    CHECK(cephyr_compile(&request, &artifact) == CEPHYR_OK && artifact);
    CHECK(check_elf(artifact, 1) == 0); /* ET_REL */
    details = artifact; /* Output slots are cleared, not destroyed. */
    CHECK(cephyr_module_load(artifact, &module, &details) == CEPHYR_INVALID_ARGUMENT && !module && !details);
    CHECK(check_elf(artifact, 1) == 0);
    cephyr_result_destroy(artifact);

    /* Non-shared executables must not be written to the binary result buffer. */
    const char *executable_args[] = {"--target=auto", "-x", "c", "-", "-o", "-"};
    request.argc = sizeof(executable_args) / sizeof(*executable_args);
    request.argv = executable_args;
    CHECK(cephyr_compile(&request, &artifact) == CEPHYR_COMPILE_ERROR && artifact);
    size_t size;
    CHECK(cephyr_result_output(artifact, &size) && size == 0);
    CHECK(strstr(cephyr_result_diagnostics(artifact, NULL), "requires -c or -shared"));
    cephyr_result_destroy(artifact);

    cephyr_context *context = NULL;
    const cephyr_context_options options = {
        sizeof(options), sizeof(shared_args) / sizeof(*shared_args), shared_args, NULL, NULL
    };
    CHECK(cephyr_context_create(&options, &context) == CEPHYR_OK && context);
    const int status = cephyr_context_compile(context, broken, sizeof(broken) - 1, NULL, &artifact);
    CHECK(status == CEPHYR_COMPILE_ERROR && artifact);
    if (strstr(cephyr_result_diagnostics(artifact, NULL), "no Cephyr native C lowering target for host")) {
        fprintf(stderr, "skipping host module execution: %s", cephyr_result_diagnostics(artifact, NULL));
        cephyr_result_destroy(artifact);
        cephyr_context_destroy(context);
        return 0;
    }
    CHECK(strstr(cephyr_result_diagnostics(artifact, NULL), "cephyr.parse"));
    CHECK(cephyr_result_output(artifact, &size) && size == 0);
    CHECK(cephyr_module_load(artifact, &module, NULL) == CEPHYR_INVALID_ARGUMENT && !module);
    cephyr_result_destroy(artifact);

    CHECK(cephyr_context_compile(context, first_source, sizeof(first_source) - 1, NULL, &artifact) ==
          CEPHYR_OK && artifact);
    CHECK(check_elf(artifact, 3) == 0); /* ET_DYN */
    CHECK(cephyr_module_load(artifact, NULL, &details) == CEPHYR_INVALID_ARGUMENT && !details);
    CHECK(cephyr_module_load(artifact, &module, &details) == CEPHYR_OK && module && !details);
    cephyr_result_destroy(artifact);

    void *address = NULL;
    CHECK(cephyr_module_symbol(module, "cephyr_test_answer", &address) == CEPHYR_OK && address);
    int (*answer)(void) = (int (*)(void))address; /* POSIX dlsym conversion */
    CHECK(answer() == 31); /* Constructor has run. */
    CHECK(cephyr_module_symbol(module, "cephyr_test_value", &address) == CEPHYR_OK && address);
    CHECK(*(int *)address == 31);
    *(int *)address = 44;
    CHECK(answer() == 44);
    CHECK(cephyr_module_symbol(module, "cephyr_test_hidden", &address) == CEPHYR_SYMBOL_NOT_FOUND && !address);
    CHECK(cephyr_module_symbol(module, "cephyr_test_answer", &address) == CEPHYR_OK && address);
    CHECK(cephyr_module_symbol(module, "cephyr_test_null", &address) == CEPHYR_OK && !address);
    CHECK(cephyr_module_symbol(module, "cephyr_test_answer", &address) == CEPHYR_OK && address);
    CHECK(cephyr_module_symbol(module, "", &address) == CEPHYR_INVALID_ARGUMENT && !address);
    CHECK(cephyr_module_symbol(module, "cephyr_test_answer", NULL) == CEPHYR_INVALID_ARGUMENT);
    int observed = 0;
    CHECK(cephyr_module_symbol(module, "cephyr_test_observe", &address) == CEPHYR_OK && address);
    ((void (*)(int *))address)(&observed);

    /* Keep two modules with identical export names alive at the same time. */
    CHECK(cephyr_context_compile(context, second_source, sizeof(second_source) - 1, NULL, &artifact) ==
          CEPHYR_OK && artifact);
    cephyr_module *other = module;
    CHECK(cephyr_module_load(artifact, &other, NULL) == CEPHYR_OK && other && other != module);
    cephyr_result_destroy(artifact);
    CHECK(cephyr_module_symbol(other, "cephyr_test_answer", &address) == CEPHYR_OK && address);
    CHECK(((int (*)(void))address)() == 52 && answer() == 44);
    cephyr_module_destroy(other);

    CHECK(cephyr_context_compile(context, unresolved, sizeof(unresolved) - 1, NULL, &artifact) ==
          CEPHYR_OK && artifact);
    cephyr_context_destroy(context);
    other = module;
    CHECK(cephyr_module_load(artifact, &other, &details) == CEPHYR_LOAD_ERROR && !other && details);
    cephyr_result_destroy(artifact);
    const char *diagnostics = cephyr_result_diagnostics(details, NULL);
    CHECK(strstr(diagnostics, "cephyr.module.load") && strstr(diagnostics, "cephyr_test_observe"));
    CHECK(cephyr_result_output(details, &size) && size == 0);
    cephyr_result_destroy(details);

    /* Binary capture is valid for cross targets; loading into this host is not. */
#if defined(__x86_64__)
    const char *foreign_target = "--target=aarch64-linux-gnu";
    const char *cross_config = CEPHYR_TEST_FIXTURES "/aarch64-cross-dso.toml";
#else
    const char *foreign_target = "--target=x86_64-linux-gnu";
    const char *cross_config = CEPHYR_TEST_FIXTURES "/x86_64-cross-dso.toml";
#endif
    const char *cross_args[] = {"--config", cross_config, foreign_target, "-shared", "--stdlib=none", "-nostdlib",
                                "-x", "assembler", "-", "-o", "-"};
    request.argc = sizeof(cross_args) / sizeof(*cross_args);
    request.argv = cross_args;
    request.stdin_data = ".text\n";
    request.stdin_size = sizeof(".text\n") - 1;
    CHECK(cephyr_compile(&request, &artifact) == CEPHYR_OK && artifact);
    CHECK(check_elf(artifact, 3) == 0);
    CHECK(cephyr_module_load(artifact, &other, NULL) == CEPHYR_LOAD_ERROR && !other);
    cephyr_result_destroy(artifact);

    CHECK(cephyr_module_load(NULL, &other, &details) == CEPHYR_INVALID_ARGUMENT && !other && !details);
    CHECK(answer() == 44 && observed == 0);
    cephyr_module_destroy(module);
    CHECK(observed == 73); /* Destructor can use host state while the module is loaded. */
    cephyr_module_destroy(NULL);
    return 0;
}

struct unit_snapshot_probe {
    cephyr_context *context;
    unsigned calls;
};

static uint32_t probe_snapshot(const cephyr_sdk_input *input, const cephyr_sdk_output *output, void *user)
{
    (void)output;
    struct unit_snapshot_probe *probe = user;
    const char *expected = probe->calls == 0 ? "consumer.c" : "<cephyr-tu-2>";
    if (probe->calls >= 2 || input->input_name.size != strlen(expected) ||
        memcmp(input->input_name.data, expected, input->input_name.size))
        return CEPHYR_SDK_REJECT;
    if (++probe->calls == 1 && cephyr_context_reset(probe->context) != CEPHYR_OK)
        return CEPHYR_SDK_REJECT;
    return CEPHYR_SDK_OK;
}

static int test_units(void)
{
    const char *const args[] = {"--target=auto", "-shared", "--stdlib=none", "-nostdlib",
                                "-o", "-", "-"};
    const cephyr_context_options options = {sizeof(options), sizeof(args) / sizeof(*args), args};
    cephyr_context *context = NULL;
    cephyr_result *artifact = NULL, *details = NULL;
    cephyr_module *one = NULL, *two = NULL;
    CHECK(cephyr_context_create(&options, &context) == CEPHYR_OK);
    CHECK(cephyr_context_link(context, NULL, &artifact) == CEPHYR_INVALID_ARGUMENT && !artifact);
    const char broken[] = "int fail(void) { return +; }";
    CHECK(cephyr_context_add_translation_unit(context, "bad-unit.c", broken, sizeof(broken) - 1) ==
          CEPHYR_OK);
    CHECK(cephyr_context_link(context, NULL, &artifact) == CEPHYR_COMPILE_ERROR && artifact);
    const char *diagnostics = cephyr_result_diagnostics(artifact, NULL);
    if (strstr(diagnostics, "no Cephyr native C lowering target for host")) {
        cephyr_result_destroy(artifact);
        cephyr_context_destroy(context);
        return 0;
    }
    CHECK(strstr(diagnostics, "bad-unit.c") && strstr(diagnostics, "cephyr.parse"));
    cephyr_result_destroy(artifact);
    CHECK(cephyr_context_reset(context) == CEPHYR_OK);

    char consumer[] = "#define CODE 7\n"
                      "extern int linked_data;\nextern int linked_function(int);\n"
                      "int cephyr_units(void) { return linked_function(CODE) + linked_data; }\n";
    const char provider[] = "#ifdef CODE\n#error macro escaped its TU\n#endif\n"
                            "int linked_data = 13;\nint linked_function(int x) { return x + 5; }\n";
    CHECK(cephyr_context_add_translation_unit(context, "consumer.c", consumer, sizeof(consumer) - 1) ==
          CEPHYR_OK);
    memset(consumer, 0, sizeof(consumer)); /* the context owns the bytes */
    const char standalone[] = "int standalone(void) { return 2; }";
    CHECK(cephyr_context_compile(context, standalone, sizeof(standalone) - 1, NULL, &artifact) ==
          CEPHYR_OK && artifact);
    CHECK(cephyr_module_load(artifact, &one, &details) == CEPHYR_OK && one && !details);
    cephyr_module_destroy(one);
    cephyr_result_destroy(artifact);
    CHECK(cephyr_context_link(context, NULL, &artifact) == CEPHYR_OK && artifact);
    CHECK(cephyr_module_load(artifact, &one, &details) == CEPHYR_LOAD_ERROR && !one && details);
    CHECK(strstr(cephyr_result_diagnostics(details, NULL), "linked_"));
    cephyr_result_destroy(details);
    cephyr_result_destroy(artifact);

    CHECK(cephyr_context_add_translation_unit(context, NULL, provider, sizeof(provider) - 1) == CEPHYR_OK);
    struct unit_snapshot_probe probe = {context, 0};
    const cephyr_sdk_hook hook = {CEPHYR_SDK_DESUGAR, {"snapshot", 8}, {0}, probe_snapshot, &probe};
    const cephyr_sdk sdk = {sizeof(sdk), CEPHYR_SDK_ABI_VERSION, &hook, 1};
    CHECK(cephyr_context_link(context, &sdk, &artifact) == CEPHYR_OK && artifact && probe.calls == 2);
    CHECK(cephyr_context_link(context, NULL, &details) == CEPHYR_INVALID_ARGUMENT && !details);
    CHECK(cephyr_context_add_translation_unit(context, "one.c", provider, sizeof(provider) - 1) ==
          CEPHYR_OK);
    CHECK(cephyr_context_add_translation_unit(context, "two.c", provider, sizeof(provider) - 1) ==
          CEPHYR_OK);
    CHECK(cephyr_context_link(context, NULL, &details) == CEPHYR_COMPILE_ERROR && details);
    CHECK(strstr(cephyr_result_diagnostics(details, NULL), "multiple definition") ||
          strstr(cephyr_result_diagnostics(details, NULL), "duplicate symbol"));
    cephyr_result_destroy(details);
    CHECK(cephyr_context_reset(context) == CEPHYR_OK);
    cephyr_context_destroy(context);
    CHECK(check_elf(artifact, 3) == 0);
    const unsigned descriptors_before_load = fd_count();
    CHECK(cephyr_module_load(artifact, &one, &details) == CEPHYR_OK && one && !details);
    void *address = NULL;
    CHECK(cephyr_module_symbol(one, "linked_data", &address) == CEPHYR_OK && address);
    *(int *)address += 10;
    for (unsigned i = 0; i < 8; ++i) {
        CHECK(cephyr_module_load(artifact, &two, &details) == CEPHYR_OK && two && !details);
        CHECK(cephyr_module_symbol(two, "cephyr_units", &address) == CEPHYR_OK && address);
        CHECK(((int (*)(void))address)() == 25);
        cephyr_module_destroy(two);
    }
    cephyr_result_destroy(artifact);
    CHECK(cephyr_module_symbol(one, "cephyr_units", &address) == CEPHYR_OK && address);
    CHECK(((int (*)(void))address)() == 35);
    cephyr_module_destroy(one);
    CHECK(fd_count() == descriptors_before_load);
    return 0;
}

#if defined(__x86_64__) && !defined(__ILP32__)
static int host_add(int value) { return value + 9; }
static int host_sub(int value) { return value - 9; }
static int host_values[2] = {10, 11};
static int alternate_values[2] = {20, 21};

static int test_registered_symbols(void)
{
    const char *const args[] = {"--target=auto", "-shared", "--stdlib=none", "-nostdlib", "-o", "-", "-",
                                "-Wl,-init,cephyr_host_init,-fini,cephyr_host_fini"};
    const cephyr_context_options options = {sizeof(options), sizeof(args) / sizeof(*args), args};
    const char source[] = "extern int cephyr_host_cb(int);\nextern int cephyr_host_data[2];\n"
                          "int *host_slot = &cephyr_host_data[1];\n"
                          "int (*host_call)(int) = cephyr_host_cb;\n"
                          "int use_host(void) { return cephyr_host_cb(++cephyr_host_data[0]); }\n"
                          "void cephyr_host_init(void) { cephyr_host_data[1] = host_call(30); }\n"
                          "void cephyr_host_fini(void) { cephyr_host_data[1] = host_call(40); }\n";
    cephyr_context *context = NULL;
    cephyr_module *one = NULL, *two = NULL;
    cephyr_result *first = NULL, *second = NULL, *details = NULL;
    void *address = NULL;
    CHECK(cephyr_context_create(&options, &context) == CEPHYR_OK);
    CHECK(cephyr_context_add_translation_unit(context, "imports.c", source, sizeof(source) - 1) ==
          CEPHYR_OK);
    CHECK(cephyr_context_register_host_symbol(context, "foo,bar", CEPHYR_HOST_DATA, host_values) ==
          CEPHYR_INVALID_ARGUMENT);
    CHECK(cephyr_context_register_host_symbol(context, "cephyr_host_cb", CEPHYR_HOST_FUNCTION,
                                               (const void *)host_add) == CEPHYR_OK);
    CHECK(cephyr_context_register_host_symbol(context, "cephyr_host_data", CEPHYR_HOST_DATA,
                                               host_values) == CEPHYR_OK);
    CHECK(cephyr_context_link(context, NULL, &first) == CEPHYR_OK && first);
    CHECK(cephyr_context_register_host_symbol(context, "cephyr_host_cb", CEPHYR_HOST_FUNCTION,
                                               (const void *)host_sub) == CEPHYR_OK);
    CHECK(cephyr_context_register_host_symbol(context, "cephyr_host_data", CEPHYR_HOST_DATA,
                                               alternate_values) == CEPHYR_OK);
    CHECK(cephyr_context_link(context, NULL, &second) == CEPHYR_OK && second);
    CHECK(cephyr_context_reset(context) == CEPHYR_OK);
    CHECK(cephyr_context_add_translation_unit(context, NULL, source, sizeof(source) - 1) == CEPHYR_OK);
    cephyr_result *unbound = NULL;
    CHECK(cephyr_context_link(context, NULL, &unbound) == CEPHYR_OK && unbound);
    cephyr_context_destroy(context);

    CHECK(cephyr_module_load(first, &one, &details) == CEPHYR_OK && one && !details);
    CHECK(cephyr_module_load(second, &two, &details) == CEPHYR_OK && two && !details);
    CHECK(host_values[1] == 39 && alternate_values[1] == 21);
    cephyr_result_destroy(first);
    cephyr_result_destroy(second);
    cephyr_module *missing = NULL;
    CHECK(cephyr_module_load(unbound, &missing, &details) == CEPHYR_LOAD_ERROR && !missing && details);
    CHECK(strstr(cephyr_result_diagnostics(details, NULL), "cephyr_host_"));
    cephyr_result_destroy(details);
    cephyr_result_destroy(unbound);

    CHECK(cephyr_module_symbol(one, "use_host", &address) == CEPHYR_OK && address);
    CHECK(((int (*)(void))address)() == 20 && host_values[0] == 11);
    CHECK(cephyr_module_symbol(one, "host_slot", &address) == CEPHYR_OK && address);
    CHECK(*(int **)address == &host_values[1]);
    CHECK(cephyr_module_symbol(one, "__wrap_cephyr_host_cb", &address) == CEPHYR_SYMBOL_NOT_FOUND && !address);
    CHECK(cephyr_module_symbol(two, "use_host", &address) == CEPHYR_OK && address);
    CHECK(((int (*)(void))address)() == 12 && alternate_values[0] == 21);
    cephyr_module_destroy(one);
    cephyr_module_destroy(two);
    CHECK(host_values[1] == 49 && alternate_values[1] == 31);
    return 0;
}
#endif

int main(void)
{
    CHECK(test_parse_c11() == 0);
    const char *const preprocess[] = {"-E", "-P", "-x", "c", "-", "-o", "-"};
    const char *const syntax[] = {"-fsyntax-only", "-x", "c", "-"};
    const char *const sarif[] = {"-fsyntax-only", "--diagnostics-format=sarif", "-"};
    const char *const mir[] = {"--emit-mir", "-x", "c", "-", "-o", "-"};
    const char *const qml_mir[] = {"--emit-mir", "--qml-pass=canonicalize", "-o", "-", "-"};
    const char *const broken_args[] = {NULL};
    const char source[] = "#include <stdint.h>\n#define TEST_VALUE 347\n"
                          "int embedding_c(void) { return TEST_VALUE; }\n";
    const char broken[] = "int embedding_broken(void) { return ; + }";
    const char identity[] = "unsigned long embedding_qml(unsigned long x) { return x + 0ul; }";
    /* The source is a length-delimited span, not a C string. */
    char bytes[sizeof(source) - 1];
    memcpy(bytes, source, sizeof(bytes));

    size_t size = 999;
    cephyr_result *first = NULL;
    cephyr_result *result = NULL;
    CHECK(cephyr_abi_version() == 1);
    CHECK(cephyr_version() && cephyr_version()[0]);
    CHECK(cephyr_python_version() == 0);
    CHECK(cephyr_result_output(NULL, &size) == NULL && size == 0);
    CHECK(cephyr_result_diagnostics(NULL, NULL) == NULL);
    cephyr_result_destroy(NULL);

    struct {
        cephyr_request request;
        size_t future_field;
    } extended = {
        {sizeof(extended), sizeof(preprocess) / sizeof(*preprocess),
         preprocess, bytes, sizeof(bytes), NULL, NULL, NULL},
        42
    };
    CHECK(cephyr_compile(&extended.request, &first) == CEPHYR_OK && first);
    const char *output = cephyr_result_output(first, &size);
    CHECK(output && size == strlen(output));
    CHECK(strstr(output, "embedding_c") && strstr(output, "347"));
    CHECK(!strstr(output, "TEST_VALUE"));
    CHECK(cephyr_result_diagnostics(first, &size) && size == 0);

    cephyr_request request = extended.request;
    request.struct_size = sizeof(request);
    request.argc = sizeof(syntax) / sizeof(*syntax);
    request.argv = syntax;
    result = first; /* cephyr_compile replaces this slot, not the prior result. */
    CHECK(cephyr_compile(&request, &result) == CEPHYR_OK && result);
    CHECK(result != first);
    CHECK(cephyr_result_output(result, &size) && size == 0);
    cephyr_result_destroy(result);

    request.argc = sizeof(sarif) / sizeof(*sarif);
    request.argv = sarif;
    request.stdin_data = broken;
    request.stdin_size = sizeof(broken) - 1;
    CHECK(cephyr_compile(&request, &result) == CEPHYR_COMPILE_ERROR && result);
    const char *diagnostics = cephyr_result_diagnostics(result, &size);
    CHECK(diagnostics && size == strlen(diagnostics));
    CHECK(strstr(diagnostics, "\"version\":\"2.1.0\"") && strstr(diagnostics, "cephyr.parse"));
    CHECK(cephyr_result_output(result, &size) && size == 0);
    cephyr_result_destroy(result);

    request.argc = sizeof(mir) / sizeof(*mir);
    request.argv = mir;
    request.stdin_data = bytes;
    request.stdin_size = sizeof(bytes);
    CHECK(cephyr_compile(&request, &result) == CEPHYR_OK && result);
    CHECK(strncmp(cephyr_result_output(result, NULL), "archon-mir 1\n", 13) == 0);
    CHECK(strstr(output, "347")); /* first result outlives later calls */
    cephyr_result_destroy(first);
    cephyr_result_destroy(result);

    request.resource_root = "/proc/self/cephyr-nonexistent-root";
    CHECK(cephyr_compile(&request, &result) == CEPHYR_COMPILE_ERROR && result);
    CHECK(strstr(cephyr_result_diagnostics(result, NULL), "cephyr.target"));
    cephyr_result_destroy(result);
    request.resource_root = NULL;

    request.archon_root = "/proc/self/cephyr-nonexistent-root";
    CHECK(cephyr_compile(&request, &result) == CEPHYR_COMPILE_ERROR && result);
    CHECK(strstr(cephyr_result_diagnostics(result, NULL), "cephyr.target"));
    cephyr_result_destroy(result);
    request.archon_root = NULL;

    bytes[3] = '\0';
    CHECK(cephyr_compile(&request, &result) == CEPHYR_COMPILE_ERROR && result);
    CHECK(strstr(cephyr_result_diagnostics(result, NULL), "NUL"));
    cephyr_result_destroy(result);

    CHECK(cephyr_compile(NULL, &result) == CEPHYR_INVALID_ARGUMENT && !result);
    CHECK(cephyr_compile(&request, NULL) == CEPHYR_INVALID_ARGUMENT);
    request.struct_size = sizeof(request) - 1;
    CHECK(cephyr_compile(&request, &result) == CEPHYR_INVALID_ARGUMENT && !result);
    request.struct_size = sizeof(request);
    request.stdin_data = NULL;
    CHECK(cephyr_compile(&request, &result) == CEPHYR_INVALID_ARGUMENT && !result);
    request.stdin_size = 0;
    request.argc = 1;
    request.argv = broken_args;
    CHECK(cephyr_compile(&request, &result) == CEPHYR_INVALID_ARGUMENT && !result);

    request.argv = syntax;
    request.argc = sizeof(syntax) / sizeof(*syntax);
    CHECK(cephyr_compile(&request, &result) == CEPHYR_OK && result);
    CHECK(cephyr_result_output(result, &size) && size == 0);
    cephyr_result_destroy(result);

    request.argv = qml_mir;
    request.argc = sizeof(qml_mir) / sizeof(*qml_mir);
    request.stdin_data = identity;
    request.stdin_size = sizeof(identity) - 1;
    CHECK(cephyr_compile(&request, &result) == CEPHYR_OK && result);
    const char *qml_output = cephyr_result_output(result, NULL);
    CHECK(strstr(qml_output, "function embedding_qml ") && !strstr(qml_output, "\nadd "));
    cephyr_result_destroy(result);
    CHECK(test_context() == 0);
    CHECK(test_native_artifacts() == 0);
    CHECK(test_units() == 0);
#if defined(__x86_64__) && !defined(__ILP32__)
    CHECK(test_registered_symbols() == 0);
#endif
    return 0;
}
