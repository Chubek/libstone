long cephyr_cli_static_left(void);
long cephyr_cli_static_right(void);

int main(void) {
    if (cephyr_cli_static_left() != 23)
        return 30;
    if (cephyr_cli_static_right() != 80)
        return 31;
    return 0;
}
