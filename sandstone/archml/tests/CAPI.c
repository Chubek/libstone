#include "ArchML.h"
#include <stdio.h>
#include <string.h>

#define CHECK(expr) do { if (!(expr)) { fprintf(stderr, "C API failure at line %d\n", __LINE__); sandstone_archml_document_free(doc); return 1; } } while (0)
int archml_c_test(const char *path) {
    sandstone_archml_error error;
    sandstone_archml_document *doc = sandstone_archml_load(path, &error);
    uint64_t operands[] = {1, 2, 3}, decoded[3] = {99, 99, 99};
    uint8_t bytes[4] = {0xaa, 0xaa, 0xaa, 0xaa};
    size_t size = 1, count = 2, consumed = 99, index = 99;
    char *text = NULL;
    CHECK(doc && !error.message[0]);
    CHECK(!strcmp(sandstone_archml_name(doc), "riscv64"));
    CHECK(sandstone_archml_encode(doc, "add", operands, 3, bytes, &size, &error) == SANDSTONE_ARCHML_BUFFER_TOO_SMALL);
    CHECK(size == 4 && bytes[0] == 0xaa);
    CHECK(sandstone_archml_encode(doc, "add", operands, 3, bytes, &size, &error) == SANDSTONE_ARCHML_OK);
    CHECK(bytes[0] == 0xb3 && bytes[2] == 0x31);
    CHECK(sandstone_archml_decode(doc, bytes, 4, &index, decoded, &count, &consumed, &error) == SANDSTONE_ARCHML_BUFFER_TOO_SMALL);
    CHECK(count == 3 && decoded[0] == 99 && index == 99 && consumed == 99);
    CHECK(sandstone_archml_decode(doc, bytes, 4, &index, decoded, &count, &consumed, &error) == SANDSTONE_ARCHML_OK);
    CHECK(count == 3 && consumed == 4 && !memcmp(operands, decoded, sizeof operands));
    CHECK(!strcmp(sandstone_archml_instruction_name(doc, index), "add"));
    operands[0] = 32;
    CHECK(sandstone_archml_encode(doc, "add", operands, 3, bytes, &size, &error) == SANDSTONE_ARCHML_INVALID);
    CHECK(error.message[0] && bytes[0] == 0xb3);
    CHECK(sandstone_archml_find(doc, "instruction", "add", &text, &size, &error) == SANDSTONE_ARCHML_OK);
    CHECK(text && strstr(text, "pcode") && !error.message[0]);
    sandstone_archml_free(text); text = NULL;
    CHECK(sandstone_archml_generate(doc, "json", NULL, &text, &size, &error) == SANDSTONE_ARCHML_OK);
    CHECK(text && size == strlen(text));
    sandstone_archml_free(text);
    CHECK(sandstone_archml_parse("(\n)", 3, &error) == NULL && error.line == 1);
    CHECK(sandstone_archml_encode(NULL, "add", operands, 3, bytes, &size, &error) == SANDSTONE_ARCHML_INVALID);
    sandstone_archml_document_free(doc);
    return 0;
}
