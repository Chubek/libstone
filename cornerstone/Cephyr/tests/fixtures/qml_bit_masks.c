extern volatile unsigned long qml_mask_sample;
extern unsigned long qml_mask_source(void);

unsigned long qml_mask_union(unsigned long y) {
    return (qml_mask_sample & 255UL) | (y | 255UL);
}

unsigned long qml_mask_disjoint(unsigned long y) {
    return (qml_mask_source() << 63) & (y & ~(1UL << 63));
}

unsigned long qml_mask_overlap(unsigned long x, unsigned long y) {
    return (x & 255UL) & (y & 128UL);
}

unsigned int qml_mask_promoted(unsigned char x, unsigned int y) {
    return x | (y | 255U);
}

double qml_mask_float(double a, double b) {
    return a + b;
}
