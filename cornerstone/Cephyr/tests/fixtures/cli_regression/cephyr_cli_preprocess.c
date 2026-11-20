#include "cephyr_cli_user.h"
#include <cephyr_cli_system.h>

#ifdef CEPHYR_CLI_REMOVE_ME
#error "-U must take effect after an earlier -D"
#endif

#if __STDC_VERSION__ != 201112L
#error "Cephyr must advertise C11 while preprocessing"
#endif

/* Cephyr CLI block comment sentinel. */
int cephyr_cli_preprocessed =
    CEPHYR_CLI_FORCED_VALUE +
    CEPHYR_CLI_USER_VALUE +
    CEPHYR_CLI_SYSTEM_VALUE +
    CEPHYR_CLI_COMMAND_VALUE; // Cephyr CLI line comment sentinel.
