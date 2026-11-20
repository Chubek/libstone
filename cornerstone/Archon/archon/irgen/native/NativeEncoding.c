#include "NativeInternal.h"

int an_encode(ANOpcode opcode, unsigned a, unsigned b, unsigned c, int imm, uint32_t *word)
{
    uint32_t operands[] = {a, b, c, (uint32_t)imm};
    uint32_t encoded;
    const ANEncoding *form;
    unsigned i;

    if ((unsigned)opcode >= AN_OPCODE_COUNT)
        return 0;
    form = &an_encodings[opcode];
    if (!form->mnemonic)
        return 0;
    for (i = 0; i < 3; ++i)
        if (operands[i] > ((form->registers & (1u << i)) ? AN_T6 : 0u))
            return 0;
    if (form->imm_width) {
        int64_t span = INT64_C(1) << (form->imm_width - form->imm_signed);
        if (imm < (form->imm_signed ? -span : 0) || imm >= span ||
            (operands[3] & form->imm_zero_mask))
            return 0;
    } else if (imm) {
        return 0;
    }
    encoded = form->bits;
    for (i = 0; i < form->field_count; ++i) {
        const ANEncodingField *slot = &form->fields[i];
        uint32_t mask = (UINT32_C(1) << slot->width) - 1u;
        encoded |= ((operands[slot->operand] >> slot->source_lsb) & mask) << slot->lsb;
    }
    if ((encoded & form->mask) != form->bits)
        return 0;
    if (word)
        *word = encoded;
    return 1;
}

int an_encode_pcrel(size_t from, size_t to, unsigned base, unsigned dst,
    ANOpcode lower, uint32_t words[2])
{
    uintmax_t distance_words = to >= from ? to - from : from - to;
    int64_t bytes, high;
    int low;
    uint32_t first, second;
    if (distance_words > (UINTMAX_C(1) << 30) ||
        (lower != AN_ADDI && lower != AN_JALR) || !base)
        return 0;
    bytes = (int64_t)distance_words * 4;
    if (from > to)
        bytes = -bytes;
    low = (int)((uint64_t)bytes & UINT64_C(0xfff));
    if (low >= 2048)
        low -= 4096;
    high = (bytes - low) / 4096;
    if (high < -524288 || high > 524287 ||
        !an_encode(AN_AUIPC, base, 0, 0, (int)((uint32_t)high & UINT32_C(0xfffff)), &first) ||
        !an_encode(lower, dst, base, 0, low, &second))
        return 0;
    if (words) {
        words[0] = first;
        words[1] = second;
    }
    return 1;
}

void an_store_word(char *destination, uint32_t word)
{
    unsigned char *bytes = (unsigned char *)destination;
    unsigned i;
    for (i = 0; i < 4; ++i)
        bytes[i] = (unsigned char)(word >> (i * 8u));
}
