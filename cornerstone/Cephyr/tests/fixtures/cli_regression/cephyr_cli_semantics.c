static long cephyr_cli_values[6];
static unsigned char cephyr_cli_bytes[4];
static long cephyr_cli_seed = 11;
static signed char cephyr_cli_signed_8 = -101;
static unsigned char cephyr_cli_unsigned_8 = 250;
static signed short cephyr_cli_signed_16 = -30001;
static unsigned short cephyr_cli_unsigned_16 = 65000;
static signed int cephyr_cli_signed_32 = -2000000001;
static unsigned int cephyr_cli_unsigned_32 = 4000000001U;
static signed long cephyr_cli_signed_64 = -5000000001L;
static unsigned long cephyr_cli_unsigned_64 = 5000000001UL;

static int cephyr_cli_widths(void) {
    unsigned char wrapped_byte = 250;
    signed char negative_byte = -7;
    unsigned short wrapped_short = 65530;
    signed short signed_16 = -30000;
    signed int signed_32 = -2000000001;
    unsigned int unsigned_32 = 4000000001U;
    signed long signed_64 = -5000000001L;
    unsigned long unsigned_64 = 5000000001UL;

    wrapped_byte = (unsigned char)(wrapped_byte + 10);
    wrapped_short = (unsigned short)(wrapped_short + 9);
    negative_byte = (signed char)(negative_byte - 100);
    signed_16 += 2345;
    signed_32 += 2000000000;
    unsigned_32 -= 4000000000U;
    signed_64 += 5000000000L;
    unsigned_64 -= 5000000000UL;

    return sizeof(char) == 1 && sizeof(short) == 2 && sizeof(int) == 4 &&
           sizeof(long) == 8 && sizeof(void *) == 8 &&
           wrapped_byte == 4 && negative_byte == -107 &&
           wrapped_short == 3 && signed_16 == -27655 &&
           signed_32 == -1 && unsigned_32 == 1 &&
           signed_64 == -1 && unsigned_64 == 1 &&
           cephyr_cli_signed_8 == -101 &&
           cephyr_cli_unsigned_8 == 250 &&
           cephyr_cli_signed_16 == -30001 &&
           cephyr_cli_unsigned_16 == 65000 &&
           cephyr_cli_signed_32 == -2000000001 &&
           cephyr_cli_unsigned_32 == 4000000001U &&
           cephyr_cli_signed_64 == -5000000001L &&
           cephyr_cli_unsigned_64 == 5000000001UL;
}

int main(void) {
    long total = 0;
    long product = 1;
    long *cursor;
    int i;

    for (i = 0; i < 6; ++i)
        cephyr_cli_values[i] = (long)(i + 1) * 3;

    cursor = &cephyr_cli_values[0];
    i = 0;
    while (i < 6) {
        if (i == 2) {
            ++cursor;
            ++i;
            continue;
        }
        total += *cursor;
        ++cursor;
        ++i;
    }

    for (i = 1; i <= 6; ++i)
        product *= i;

    i = 0;
    do {
        cephyr_cli_bytes[i] = (unsigned char)(i * 7 + 1);
        ++i;
    } while (i < 4);

    if (!cephyr_cli_widths())
        return 10;
    if (total != 54 || product != 720)
        return 11;
    if (cephyr_cli_bytes[0] != 1 || cephyr_cli_bytes[3] != 22)
        return 12;
    if (*(cephyr_cli_values + 4) != 15)
        return 13;
    if ((cephyr_cli_seed > 0 ? cephyr_cli_seed : -cephyr_cli_seed) != 11)
        return 14;
    return 0;
}
