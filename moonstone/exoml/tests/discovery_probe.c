#include "discovery.h"
#include <stdio.h>
int main(void) {
    printf("{\"long\":%zu,\"size_t\":%zu,\"intptr_t\":%zu,\"bool\":%zu,"
           "\"node_size\":%zu,\"node_align\":%zu,\"values_offset_bits\":%zu,"
           "\"packed_size\":%zu,\"aligned_size\":%zu,\"aligned_align\":%zu}\n",
           sizeof(long), sizeof(size_t), sizeof(intptr_t), sizeof(bool), sizeof(exoml_node),
           _Alignof(exoml_node), offsetof(exoml_node, values) * 8, sizeof(struct exoml_packed),
           sizeof(struct exoml_aligned), _Alignof(struct exoml_aligned));
    return 0;
}
