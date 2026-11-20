#include <cephyr/Cephyr.h>
#include <cephyr/CephyrSDK.h>

#include <stddef.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    const char *const args[] = {"--target=x86_64-linux-gnu", "-x", "c", "-", "-S",
                                "-o", "-", "--no-cache"};
    const char source[] = "#include <limits.h>\n"
                          "#ifndef CEPHYR_TEST_INSTALLED_HEADER\n"
                          "#error not using installed Cephyr resources\n"
                          "#endif\n"
                          "_Static_assert(CHAR_BIT == 8, \"expected 8-bit bytes\");\n"
                          "int cephyr_installed_consumer(void) { return 37; }\n";
    cephyr_request request = {
        .struct_size = sizeof(request), .argc = sizeof(args) / sizeof(args[0]),
        .argv = args, .stdin_data = source, .stdin_size = sizeof(source) - 1,
    };
    if (cephyr_abi_version() != 1 || !cephyr_version() || !*cephyr_version())
        return 1;
    for (unsigned i = 0; i < 2; ++i) {
        cephyr_result *result = NULL;
        const int status = cephyr_compile(&request, &result);
        if (!result)
            return 2;
        size_t size = 0;
        const char *output = cephyr_result_output(result, &size);
        const int ok = status == CEPHYR_OK && size &&
                       strstr(output, "cephyr_installed_consumer:") != NULL;
        if (!ok) {
            const char *diagnostics = cephyr_result_diagnostics(result, &size);
            if (size && diagnostics)
                fwrite(diagnostics, 1, size, stderr);
        }
        cephyr_result_destroy(result);
        if (!ok)
            return 3;
    }
    return 0;
}
