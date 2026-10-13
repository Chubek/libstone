#include "blackstone.h"
#include <string.h>

int blackstone_c_api_test(const char *kernel) {
    blackstone_runtime *runtime = blackstone_create();
    blackstone_kernel_info info;
    int changed = 42;
    if (!runtime) return 1;
    if (strcmp(blackstone_version(), "0.2.0") || blackstone_load_file(runtime, kernel) ||
        blackstone_kernel_count(runtime) != 1 || blackstone_kernel_get(runtime, 0, &info) ||
        strcmp(info.name, "strength_reduction") || blackstone_requirement_count(runtime, 0) != 3 ||
        blackstone_preservation_count(runtime, 0) != 1 ||
        strcmp(blackstone_preservation_get(runtime, 0, 0), "cfg") ||
        blackstone_requirement_get(runtime, 0, 3) != NULL ||
        blackstone_kernel_get(runtime, 1, &info) != -1 ||
        blackstone_run(runtime, NULL, "ssa", NULL, 0, NULL, NULL, &changed) != -1 || changed != 0) {
        blackstone_destroy(runtime); return 1;
    }
    blackstone_destroy(runtime);
    return 0;
}
