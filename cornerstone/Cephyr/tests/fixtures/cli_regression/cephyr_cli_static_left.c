static long cephyr_cli_static_value = 19;

static long cephyr_cli_static_step(long value) {
    return value + 4;
}

long cephyr_cli_static_left(void) {
    return cephyr_cli_static_step(cephyr_cli_static_value);
}
