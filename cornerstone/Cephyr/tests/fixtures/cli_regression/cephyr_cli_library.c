unsigned long cephyr_cli_accumulate(const unsigned *values, unsigned count) {
    unsigned long result = 0;
    unsigned index;

    for (index = 0; index < count; ++index)
        result += values[index];
    return result;
}
