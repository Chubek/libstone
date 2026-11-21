#include "bitfields.h"

struct MixedBits external_bits = {
    .prefix = 0xfa, .small = 5, .delta = -313, .flag = 1, .trailer = 9,
    .wide = 0x71020408u, .sign = -1, .last = 8,
};

void external_update(struct MixedBits *record) {
    record->small = 3;
    record->delta = -321;
    record->flag = 0;
    record->wide = 0x10002000u;
}

int main(void) {
    return cephyr_bits();
}
