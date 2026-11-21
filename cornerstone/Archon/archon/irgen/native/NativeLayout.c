#include "NativeInternal.h"

#include <stdlib.h>
#include <string.h>

#define AN_LABEL_LIMIT (2u * AN_MAX_BLOCK + 4u * AN_MAX_INSTS)
/* An RV64IM ordered FP comparison can emit up to six local transfers. */
#define AN_REFERENCE_LIMIT (3u * AN_MAX_BLOCK + 6u * AN_MAX_INSTS)

typedef struct {
    size_t from;
    unsigned to;
} Reference;

struct ANLayout {
    size_t function, entry;
    const char *symbol;
    size_t *labels;
    size_t label_capacity, defined;
    Reference *references;
    size_t reference_capacity, reference_count;
};

void an_out_error(ANOut *out, ArchonBackendStatus status, const char *detail)
{
    if (out->status == ARCHON_BACKEND_OK) {
        out->status = status;
        out->detail = detail;
    }
}

static int fail(ANOut *out, ArchonBackendStatus status, const char *detail)
{
    an_out_error(out, status, detail);
    return 0;
}

static void *grow(ANOut *out, void *table, size_t current, size_t required,
    size_t limit, size_t item_size, size_t *capacity)
{
    size_t next = current ? current : 16u;
    void *replacement;
    if (required > limit || required > SIZE_MAX / item_size) {
        fail(out, ARCHON_BACKEND_LIMIT, "native layout table exceeds profile limit");
        return NULL;
    }
    while (next < required)
        next = next <= limit / 2 ? next * 2 : limit;
    if (next > SIZE_MAX / item_size)
        next = required;
    replacement = realloc(table, next * item_size);
    if (!replacement) {
        fail(out, ARCHON_BACKEND_NOMEM, "could not grow native layout table");
        return NULL;
    }
    *capacity = next;
    return replacement;
}

int an_layout_begin(ANOut *out, size_t function, const char *symbol)
{
    if (out->status != ARCHON_BACKEND_OK)
        return 0;
    if (out->layout)
        return fail(out, ARCHON_BACKEND_INTERNAL, "native layout already has an active function");
    out->layout = calloc(1, sizeof(*out->layout));
    if (!out->layout)
        return fail(out, ARCHON_BACKEND_NOMEM, "could not allocate native layout");
    out->layout->function = function;
    out->layout->entry = out->instructions;
    out->layout->symbol = symbol;
    return 1;
}

int an_layout_label(ANOut *out, size_t function, unsigned label)
{
    ANLayout *layout = out->layout;
    if (out->status != ARCHON_BACKEND_OK)
        return 0;
    if (!layout || layout->function != function)
        return fail(out, ARCHON_BACKEND_INTERNAL, "native label belongs to a different function");
    if (label >= AN_LABEL_LIMIT)
        return fail(out, ARCHON_BACKEND_LIMIT, "native label index exceeds profile limit");
    if (label >= layout->label_capacity) {
        size_t i, capacity;
        size_t *labels = grow(out, layout->labels, layout->label_capacity, (size_t)label + 1u,
            AN_LABEL_LIMIT, sizeof(*labels), &capacity);
        if (!labels)
            return 0;
        for (i = layout->label_capacity; i < capacity; ++i)
            labels[i] = SIZE_MAX;
        layout->labels = labels;
        layout->label_capacity = capacity;
    }
    if (layout->labels[label] != SIZE_MAX)
        return fail(out, ARCHON_BACKEND_INTERNAL, "duplicate native label");
    layout->labels[label] = out->instructions;
    ++layout->defined;
    return 1;
}

int an_layout_reference(ANOut *out, size_t function, unsigned label, unsigned prefix_words)
{
    ANLayout *layout = out->layout;
    size_t from;
    if (out->status != ARCHON_BACKEND_OK)
        return 0;
    if (!layout || layout->function != function)
        return fail(out, ARCHON_BACKEND_INTERNAL, "native transfer crosses local label namespace");
    if (label >= AN_LABEL_LIMIT || prefix_words > 1 ||
        out->instructions > SIZE_MAX - prefix_words - 2)
        return fail(out, ARCHON_BACKEND_INTERNAL, "invalid native transfer fixup");
    from = out->instructions + prefix_words;
    if (layout->reference_count && from < layout->references[layout->reference_count - 1u].from + 2u)
        return fail(out, ARCHON_BACKEND_INTERNAL, "overlapping native transfer fixups");
    if (layout->reference_count == layout->reference_capacity) {
        Reference *refs = grow(out, layout->references, layout->reference_capacity,
            layout->reference_count + 1u, AN_REFERENCE_LIMIT, sizeof(*refs),
            &layout->reference_capacity);
        if (!refs)
            return 0;
        layout->references = refs;
    }
    layout->references[layout->reference_count++] = (Reference){from, label};
    return 1;
}

int an_layout_entry(ANOut *out, const char *symbol, size_t *entry)
{
    if (!out->layout || !symbol || !out->layout->symbol || strcmp(symbol, out->layout->symbol))
        return fail(out, ARCHON_BACKEND_UNSUPPORTED,
            "raw machine code requires a locally defined function symbol");
    *entry = out->layout->entry;
    return 1;
}

int an_layout_finish(ANContext *ctx, ANOut *out, unsigned label_count)
{
    ANLayout *layout = out->layout;
    size_t i;
    const size_t end = out->instructions;
    if (out->status != ARCHON_BACKEND_OK)
        return 0;
    if (!layout || !label_count || label_count > layout->label_capacity ||
        label_count != layout->defined || end <= layout->entry)
        return fail(out, ARCHON_BACKEND_INTERNAL, "incomplete native function layout");

    if (out->machine_code && (out->size % 4 || end != out->size / 4 || !out->data))
        return fail(out, ARCHON_BACKEND_INTERNAL, "native byte count does not match layout");
    for (i = 0; i < label_count; ++i) {
        if (!(i % 128u) && !an_continue(ctx))
            return 0;
        if (layout->labels[i] < layout->entry || layout->labels[i] >= end)
            return fail(out, ARCHON_BACKEND_INTERNAL, "native label does not name an instruction");
    }
    for (i = 0; i < layout->reference_count; ++i) {
        Reference ref = layout->references[i];
        uint32_t words[2];
        if (!(i % 128u) && !an_continue(ctx))
            return 0;
        if (end < 2 || ref.from < layout->entry || ref.from > end - 2 || ref.to >= label_count)
            return fail(out, ARCHON_BACKEND_INTERNAL, "unresolved or invalid native local transfer");
        if (!an_encode_pcrel(ref.from, layout->labels[ref.to], AN_T3, AN_ZERO, AN_JALR, words))
            return fail(out, ARCHON_BACKEND_LIMIT, "native local transfer is out of range");
        if (out->machine_code) {
            static const char pending[8] = {0};
            if (memcmp(out->data + 4 * ref.from, pending, sizeof(pending)))
                return fail(out, ARCHON_BACKEND_INTERNAL, "native fixup would overwrite code");
            an_store_word(out->data + 4 * ref.from, words[0]);
            an_store_word(out->data + 4 * (ref.from + 1u), words[1]);
        }
    }
    return 1;
}

void an_layout_dispose(ANOut *out)
{
    if (out->layout) {
        free(out->layout->labels);
        free(out->layout->references);
        free(out->layout);
        out->layout = NULL;
    }
}
