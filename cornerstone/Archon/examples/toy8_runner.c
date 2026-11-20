/* Build with the matching generated toy8_emu.c and toy8_emu.h. */
#include "toy8_emu.h"

#include <inttypes.h>
#include <stdio.h>

int main(int argc, char **argv) {
    uint64_t registers[TOY8_EMU_REGISTER_COUNT] = {0};
    struct toy8_emu_state state = {0};
    uint8_t instruction[2];
    size_t offset = 0, size, i;
    FILE *program;

    if (argc != 2) {
        fprintf(stderr, "usage: %s PROGRAM.bin\n", argv[0]);
        return 2;
    }
    state.registers = registers;
    program = fopen(argv[1], "rb");
    if (!program) {
        perror(argv[1]);
        return 1;
    }
    while ((size = fread(instruction, 1, sizeof instruction, program)) != 0) {
        enum toy8_emu_status status =
            toy8_emu_step_state(instruction, size, &state);
        if (status != TOY8_EMU_OK) {
            fprintf(stderr, "%s: byte %zu: emulator status %d\n", argv[1], offset, (int)status);
            fclose(program);
            return 1;
        }
        offset += size;
    }
    if (ferror(program)) {
        fprintf(stderr, "%s: error reading program\n", argv[1]);
        fclose(program);
        return 1;
    }
    if (fclose(program) != 0) {
        perror(argv[1]);
        return 1;
    }
    for (i = 0; i < sizeof registers / sizeof *registers; ++i)
        printf("r%zu = %" PRIu64 "\n", i, registers[i]);
    return 0;
}
