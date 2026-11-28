/* Deterministic checksum kernel used by the Mahak minimal example.
 * It prints one line: checksum=<16 hex digits>. */
#include <stdio.h>
#include <stdlib.h>

static unsigned long mix(unsigned long state, unsigned long value) {
    state ^= value + 0x9e3779b97f4a7c15UL + (state << 6) + (state >> 2);
    state *= 0xff51afd7ed558ccdUL;
    return state ^ (state >> 33);
}

static unsigned long checksum(unsigned long size) {
    unsigned long state = 0xcbf29ce484222325UL;
    for (unsigned long index = 0; index < size; ++index)
        state = mix(state, index * 2654435761UL);
    return state;
}

int main(int argc, char **argv) {
    unsigned long size = argc > 1 ? strtoul(argv[1], NULL, 10) : 64UL;
    if (size == 0)
        size = 64UL;
    printf("checksum=%016lx\n", checksum(size));
    return 0;
}
