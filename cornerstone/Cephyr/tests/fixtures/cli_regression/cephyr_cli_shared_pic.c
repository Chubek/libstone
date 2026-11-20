long cephyr_cli_shared_counter = 37;
long *cephyr_cli_shared_pointer = &cephyr_cli_shared_counter;

long cephyr_cli_shared_read(void) {
    return *cephyr_cli_shared_pointer;
}

void cephyr_cli_shared_write(long value) {
    *cephyr_cli_shared_pointer = value;
}
