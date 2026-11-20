static long cephyr_cli_static_value = 40;

static long cephyr_cli_static_step(long value) {
    return value * 2;
}

long cephyr_cli_static_right(void) {
    return cephyr_cli_static_step(cephyr_cli_static_value);
}
