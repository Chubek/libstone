#include "calc.h"
#include "events.h"

#include <assert.h>

static int sum_int(calc_int_t *total, calc_int_t value) {
    *total += value;
    return CALC_WALK_CONTINUE;
}

#define CALC_VISITOR_NAME sum
#define CALC_VISITOR_CONTEXT calc_int_t
#define CALC_VALUE_int sum_int
#include "walk.h"
#undef CALC_VALUE_int
#undef CALC_VISITOR_CONTEXT
#undef CALC_VISITOR_NAME

static int skip_group(void *ctx, calc_expr_t node) {
    (void)ctx;
    (void)node;
    return CALC_WALK_SKIP;
}

#define CALC_VISITOR_NAME prune
#define CALC_VISIT_Group skip_group
#include "walk.h"
#undef CALC_VISIT_Group
#undef CALC_VISITOR_NAME

calc_expr_t make_number_in_other_unit(calc_arena *arena);

struct event_counts {
    size_t records, bytes, eof;
};

static int on_event(void *ctx, const struct ASDLEvent *event) {
    struct event_counts *counts = ctx;
    if (event->kind == ASDL_EVENT_DATA) {
        assert(event->offset == counts->bytes);
        counts->bytes += event->length;
        return counts->records++ == 0 ? ASDL_EVENT_PAUSE : ASDL_EVENT_CONTINUE;
    }
    assert(event->kind == ASDL_EVENT_EOF);
    assert(event->offset == counts->bytes);
    ++counts->eof;
    return ASDL_EVENT_CONTINUE;
}

int main(int argc, char **argv) {
    _Static_assert(CALC_FIXTURE_VERSION == 1, "embedded C prologue");
    _Static_assert(CALC_TYPE_COUNT == 3 && CALC_CONSTRUCTOR_COUNT == 8, "inventory");
    _Static_assert(calc_Plus == 1 && calc_Minus == 2, "enum ABI");
    _Static_assert(calc_Number_kind == 1 && calc_Group_kind == 6, "sum tag ABI");
    assert(argc == 2);

    calc_arena arena;
    calc_arena_init(&arena);
    assert(!calc_make_document(&arena, (calc_expr_seq){0}, "empty"));
    assert(arena.error == CALC_INVALID);
    assert(!make_number_in_other_unit(&arena));
    calc_arena_clear_error(&arena);

    calc_expr_t n = make_number_in_other_unit(&arena);
    assert(n && n->kind == calc_Number_kind && n->v.Number.value == 42);
    const char *name = calc_identifier(&arena, "fixture");
    assert(name && name == calc_identifier(&arena, "fixture"));

    const char payload[] = {'a', '\0', 'b'};
    calc_string_t string = calc_string_copy(&arena, payload, sizeof payload);
    calc_expr_t text = calc_Text(&arena, calc_string_some(string), calc_int_none());
    assert(text && text->v.Text.value.has_value);
    assert(text->v.Text.value.value.length == sizeof payload);
    assert(!memcmp(text->v.Text.value.value.data, payload, sizeof payload));

    calc_map_int_expr_t map = calc_map_int_expr_new(NULL);
    assert(calc_map_int_expr_set(&map, 1, n));
    assert(*calc_map_int_expr_get(&map, 1) == n);
    calc_expr_t indexed = calc_Index(&arena, map, calc_int_none());

    calc_array_expr_2_t pair = calc_array_expr_2_new();
    assert(calc_array_expr_2_set(&pair, 0, n));
    assert(calc_array_expr_2_set(&pair, 1, text));
    calc_expr_t paired = calc_Pair(&arena, pair, calc_int_none());

    calc_list_expr_t list = calc_list_expr_new();
    assert(calc_list_expr_push(&list, indexed));
    assert(calc_list_expr_push(&list, paired));
    calc_expr_t group = calc_Group(&arena, list, calc_int_some(2));

    calc_expr_seq forms = calc_expr_seq_copy(&arena, &group, 1);
    calc_document_t doc = calc_make_document(&arena, forms, name);
    assert(doc && calc_validate_document(doc, 5));
    assert(!calc_validate_document(doc, 3));
    assert(!strcmp(calc_expr_name(group), "Group"));
    calc_int_t total = 0;
    assert(sum_document(doc, &total) == CALC_WALK_CONTINUE);
    assert(total == 87); /* group offset + map key + two Number visits */
    assert(prune_document(doc, NULL) == CALC_WALK_CONTINUE);
    /* Borrowed list storage is shared with the AST; this creates a cycle. */
    assert(calc_list_expr_set(&list, 0, group));
    assert(!calc_validate_document(doc, 8));
    assert(prune_document(doc, NULL) == CALC_WALK_CONTINUE);

    calc_arena_destroy(&arena);
    ASDLHashMap_destroy(&map);
    ASDLFixedArray_destroy(&pair);
    ASDLVector_destroy(&list);

    struct calc_listener listener;
    struct event_counts counts = {0};
    assert(calc_listener_open(&listener, argv[1]));
    assert(calc_listener_add(&listener, (struct ASDLEventHandler){on_event, &counts}));
    assert(calc_listener_poll(&listener, false) == ASDL_EVENT_PAUSE);
    assert(counts.records == 1 && counts.bytes == 4 && counts.eof == 0);
    assert(calc_listener_poll(&listener, true) == ASDL_EVENT_CONTINUE);
    assert(calc_listener_poll(&listener, true) == ASDL_EVENT_CONTINUE);
    assert(counts.records == 2 && counts.bytes == 7 && counts.eof == 1);
    calc_listener_close(&listener);
    return 0;
}
