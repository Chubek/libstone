#pragma once

#include <string_view>

namespace aurocks::detail::portable {

inline constexpr std::string_view Runtime = R"aurocks(
/* Position-local chart items are keyed by (rule, dot, origin).
 * Completed productions also form symbol nodes keyed by (lhs, origin, end).
 * The latter pack alternative rules; advanced items pack alternative splits.
 * IDs are one-based so zero denotes an absent item, edge or child. */
typedef struct {
    size_t rule, dot, origin, end, next, edge, choice, waited_through;
    unsigned count, active;
} aurocks_item;
typedef struct { size_t prefix, child, next; } aurocks_edge;
typedef struct {
    size_t first, last, used, capacity, *slots;
} aurocks_column;
typedef struct {
    const char *input;
    size_t length, furthest;
    aurocks_column *chart;
    size_t *layout;
    aurocks_item *items;
    aurocks_edge *edges;
    size_t items_used, items_capacity, edges_used, edges_capacity, work;
    size_t todo[AUROCKS_RX_STATES], current[AUROCKS_RX_STATES], next[AUROCKS_RX_STATES];
    size_t seen[AUROCKS_RX_STATES], rx_epoch;
    int failed;
} aurocks_parser;

static int aurocks_step(aurocks_parser *p) {
    if (p->work++ < AUROCKS_WORK_LIMIT) return 1;
    p->failed = 1;
    return 0;
}

static void *aurocks_grow(void *ptr, size_t *capacity, size_t used, size_t width) {
    const size_t max_count = ((size_t)-1) / width;
    size_t n = *capacity;
    if (used < n) return ptr;
    if (used >= max_count) return NULL;
    if (!n) n = max_count < 16 ? max_count : 16;
    while (n <= used) n = n > max_count / 2 ? max_count : 2 * n;
    ptr = realloc(ptr, n * width);
    if (ptr) *capacity = n;
    return ptr;
}

static void aurocks_rx_push(aurocks_parser *p, size_t *count, size_t id) {
    if (p->seen[id] != p->rx_epoch) {
        p->seen[id] = p->rx_epoch;
        p->todo[(*count)++] = id;
    }
}

static int aurocks_word_byte(unsigned c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '_';
}

/* One anchored, longest-prefix match. Empty matches are safe in terminals;
 * layout loops only while the input actually advances. */
static size_t aurocks_rx_match(aurocks_parser *p, size_t state, size_t start) {
    size_t at = start, seeds = 1, best = (size_t)-1;
    size_t *current = p->current, *next = p->next;
    current[0] = state;
    for (;;) {
        size_t work = 0, consumed = 0, i;
        const unsigned ch = (unsigned char)p->input[at];
        if (!++p->rx_epoch) {
            memset(p->seen, 0, sizeof(p->seen));
            ++p->rx_epoch;
        }
        for (i = 0; i < seeds; ++i) aurocks_rx_push(p, &work, current[i]);
        while (work) {
            const aurocks_rx_state *s = &aurocks_states[p->todo[--work]];
            if (!aurocks_step(p)) return (size_t)-1;
            switch (s->kind) {
            case 0: best = at; break;
            case 1:
                if (at < p->length && ch < 256 &&
                    (aurocks_sets[s->other][ch / 8] & (1u << (ch % 8))))
                    next[consumed++] = s->next;
                break;
            case 2:
                aurocks_rx_push(p, &work, s->other);
                aurocks_rx_push(p, &work, s->next);
                break;
            case 3:
                if (at == start) aurocks_rx_push(p, &work, s->next);
                break;
            case 4:
                if (at == p->length) aurocks_rx_push(p, &work, s->next);
                break;
            case 5: case 6: {
                const int left = at > 0 && aurocks_word_byte((unsigned char)p->input[at - 1]);
                const int right = at < p->length && aurocks_word_byte(ch);
                if ((left != right) == (s->kind == 5))
                    aurocks_rx_push(p, &work, s->next);
                break;
            }
            }
        }
        if (!consumed) return best;
        { size_t *swap = current; current = next; next = swap; }
        seeds = consumed;
        ++at;
    }
}

static size_t aurocks_skip(aurocks_parser *p, size_t at) {
    const size_t start = at;
    if (!aurocks_has_skip) return at;
    while (at < p->length) {
        size_t end;
        if (p->layout[at]) {
            at = p->layout[at] - 1;
            break;
        }
        end = aurocks_rx_match(p, aurocks_skip_rx, at);
        if (end == (size_t)-1 || end == at) break;
        at = end;
    }
    p->layout[start] = at + 1;
    return at;
}

static size_t aurocks_slot(const aurocks_parser *p, const aurocks_column *c,
                           size_t rule, size_t dot, size_t origin) {
    const size_t mask = c->capacity - 1;
    size_t slot = ((rule * 1000003u) ^ (dot * 8191u) ^ (origin * 257u)) & mask;
    while (c->slots[slot]) {
        const aurocks_item *x = &p->items[c->slots[slot]];
        if (x->rule == rule && x->dot == dot && x->origin == origin) break;
        slot = (slot + 1) & mask;
    }
    return slot;
}

static size_t aurocks_find(const aurocks_parser *p, size_t end,
                           size_t rule, size_t dot, size_t origin) {
    const aurocks_column *c = &p->chart[end];
    return c->capacity ? c->slots[aurocks_slot(p, c, rule, dot, origin)] : 0;
}

static int aurocks_rehash(aurocks_parser *p, aurocks_column *c) {
    const size_t cap = c->capacity ? c->capacity * 2 : 16;
    size_t id, *slots;
    if (cap < c->capacity || cap > ((size_t)-1) / sizeof(*slots)) return 0;
    slots = (size_t *)calloc(cap, sizeof(*slots));
    if (!slots) return 0;
    free(c->slots);
    c->slots = slots;
    c->capacity = cap;
    for (id = c->first; id; id = p->items[id].next) {
        const aurocks_item *x = &p->items[id];
        slots[aurocks_slot(p, c, x->rule, x->dot, x->origin)] = id;
    }
    return 1;
}

static size_t aurocks_record(aurocks_parser *p, size_t end, size_t rule,
                             size_t dot, size_t origin, size_t prefix, size_t child) {
    aurocks_column *c = &p->chart[end];
    size_t slot, id;
    if (!c->capacity || c->used >= c->capacity / 2) {
        if (!aurocks_rehash(p, c)) goto fail;
    }
    slot = aurocks_slot(p, c, rule, dot, origin);
    id = c->slots[slot];
    if (!id) {
        aurocks_item *grown, item = {0};
        if (p->items_used >= AUROCKS_ITEM_LIMIT) goto fail;
        grown = (aurocks_item *)aurocks_grow(
            p->items, &p->items_capacity, p->items_used + 1, sizeof(*grown));
        if (!grown) goto fail;
        p->items = grown;
        id = ++p->items_used;
        item.rule = rule; item.dot = dot; item.origin = origin; item.end = end;
        p->items[id] = item;
        if (c->last) p->items[c->last].next = id;
        else c->first = id;
        c->last = id;
        c->slots[slot] = id;
        ++c->used;
    }
    if (prefix || child) {
        size_t e;
        aurocks_edge *grown;
        if (p->edges_used >= AUROCKS_EDGE_LIMIT) goto fail;
        grown = (aurocks_edge *)aurocks_grow(
            p->edges, &p->edges_capacity, p->edges_used + 1, sizeof(*grown));
        if (!grown) goto fail;
        p->edges = grown;
        e = ++p->edges_used;
        p->edges[e].prefix = prefix;
        p->edges[e].child = child;
        p->edges[e].next = p->items[id].edge;
        p->items[id].edge = e;
    }
    return id;
fail:
    p->failed = 1;
    return 0;
}

static void aurocks_predict(aurocks_parser *p, size_t end, size_t nt) {
    size_t i;
    for (i = aurocks_productions[nt]; i < aurocks_productions[nt + 1] && !p->failed; ++i)
        aurocks_record(p, end, aurocks_production_ids[i], 0, end, 0, 0);
}

static size_t aurocks_scan(aurocks_parser *p, const aurocks_symbol *s, size_t at) {
    if (s->kind == 3) return at; /* Explicit %empty, if present. */
    at = aurocks_skip(p, at);
    if (at > p->furthest) p->furthest = at;
    if (s->kind == 2) {
        const size_t end = aurocks_rx_match(p, s->id, at);
        return end == at ? (size_t)-1 : end;
    }
    if (s->length <= p->length - at && !memcmp(p->input + at, s->literal, s->length))
        return at + s->length;
    return (size_t)-1;
}

static void aurocks_recognize(aurocks_parser *p) {
    size_t at, id;
    aurocks_predict(p, 0, aurocks_start);
    for (at = 0; at <= p->length && !p->failed; ++at) {
        if (p->chart[at].first && at > p->furthest) p->furthest = at;
        for (id = p->chart[at].first; id && !p->failed; id = p->items[id].next) {
            const aurocks_item x = p->items[id];
            if (x.rule >= AUROCKS_RULES) {
                /* A packed symbol is itself a completion event. Later packed
                 * alternatives do not have to re-awaken the same waiters. */
                const size_t last = p->chart[x.origin].last;
                size_t w;
                p->items[id].waited_through = last;
                for (w = p->chart[x.origin].first; w && !p->failed; w = p->items[w].next) {
                    const aurocks_item y = p->items[w];
                    if (!aurocks_step(p)) break;
                    if (y.rule < AUROCKS_RULES && y.dot < aurocks_rules[y.rule].length) {
                        const aurocks_symbol *s = &aurocks_rhs[aurocks_rules[y.rule].offset + y.dot];
                        if (s->kind == 0 && s->id == x.rule - AUROCKS_RULES)
                            aurocks_record(p, at, y.rule, y.dot + 1, y.origin, w, id);
                    }
                    if (w == last) break;
                }
            } else {
                const aurocks_rule *r = &aurocks_rules[x.rule];
                if (x.dot == r->length) {
                    aurocks_record(p, at, AUROCKS_RULES + r->lhs, 0, x.origin, 0, id);
                } else {
                    const aurocks_symbol *s = &aurocks_rhs[r->offset + x.dot];
                    if (s->kind == 0) {
                        const size_t nullable = aurocks_find(p, at, AUROCKS_RULES + s->id, 0, at);
                        aurocks_predict(p, at, s->id);
                        /* Replay a nullable completion only for an item not in
                         * the completion's snapshot. If the event is still
                         * queued, it will visit this waiter itself. */
                        if (nullable && p->items[nullable].waited_through &&
                            id > p->items[nullable].waited_through && !p->failed)
                            aurocks_record(p, at, x.rule, x.dot + 1, x.origin, id, nullable);
                    } else {
                        const size_t end = aurocks_scan(p, s, at);
                        if (end != (size_t)-1)
                            aurocks_record(p, end, x.rule, x.dot + 1, x.origin, id, 0);
                    }
                }
            }
        }
    }
}

static int aurocks_priority(const aurocks_parser *p, size_t a, size_t b) {
    const aurocks_rule *x = &aurocks_rules[p->items[a].rule];
    const aurocks_rule *y = &aurocks_rules[p->items[b].rule];
    if (x->precedence != y->precedence) return x->precedence > y->precedence ? 1 : -1;
    if (x->preference != y->preference) return x->preference > y->preference ? 1 : -1;
    return 0;
}

typedef struct { size_t id, remaining; } aurocks_visit;

/* Bottom-up ambiguity counts, saturated at two. Only equally ranked
 * alternatives contribute to a symbol's count. Traversal uses a heap stack
 * rather than recursive C calls, including on deeply left-recursive trees. */
static int aurocks_resolve(aurocks_parser *p, size_t root,
                           aurocks_visit **stack, size_t *capacity) {
    size_t depth = 0, id = root;
    for (;;) {
        aurocks_item *x = &p->items[id];
        if (!x->count) {
            aurocks_visit *grown = (aurocks_visit *)aurocks_grow(
                *stack, capacity, depth, sizeof(*grown));
            if (!grown) return 0;
            *stack = grown;
            (*stack)[depth].id = id;
            (*stack)[depth++].remaining = x->edge;
            /* Active marker. Forest cycles are rejected at generation time;
             * keep this guard so corrupted metadata cannot loop here. */
            x->active = 1;
        }
        while (depth) {
            aurocks_visit *f = &(*stack)[depth - 1];
            x = &p->items[f->id];
            if (f->remaining) {
                const size_t e = f->remaining;
                const aurocks_edge *edge = &p->edges[e];
                const size_t prefix = edge->prefix, child = edge->child;
                const unsigned left = prefix ? p->items[prefix].count : 1;
                const unsigned right = child ? p->items[child].count : 1;
                unsigned n;
                int rank = 0;
                if ((prefix && p->items[prefix].active) || (child && p->items[child].active))
                    return 0;
                if (!left || !right) {
                    id = !left ? prefix : child;
                    break;
                }
                n = left > 1 || right > 1 ? 2 : 1;
                if (x->choice && x->rule >= AUROCKS_RULES)
                    rank = aurocks_priority(p, child, p->edges[x->choice].child);
                if (!x->choice || rank > 0) {
                    x->choice = e;
                    x->count = n;
                } else if (!rank) {
                    const size_t old = p->edges[x->choice].child;
                    if (x->rule >= AUROCKS_RULES && p->items[child].rule != p->items[old].rule) {
                        if (p->items[child].rule < p->items[old].rule) x->choice = e;
                    } else if (e < x->choice) {
                        x->choice = e;
                    }
                    x->count = 2;
                }
                f->remaining = edge->next;
            } else {
                if (!x->choice) x->count = 1; /* Empty production/prefix. */
                x->active = 0;
                --depth;
            }
        }
        if (!depth) return 1;
    }
}

typedef struct { size_t production, base, next; } aurocks_frame;
typedef struct { size_t child, begin, end; void *value; } aurocks_arg;

/* Walk the accepted tree, not just unique forest nodes: separate occurrences
 * run their actions independently, including same-position empty reductions. */
static void *aurocks_evaluate(aurocks_parser *p, size_t root) {
    size_t symbol = root, depth = 0, used = 0, frames_cap = 0, args_cap = 0, reductions = 0;
    aurocks_frame *frames = NULL;
    aurocks_arg *args = NULL;
    void *value = NULL;
    void **values = (void **)malloc(AUROCKS_MAX_RHS * sizeof(*values));
    if (!values) goto fail;
    for (;;) {
        if (symbol) {
            const size_t production = p->edges[p->items[symbol].choice].child;
            const size_t n = p->items[production].dot;
            size_t id = production, i = n;
            aurocks_frame *fg;
            if (reductions++ >= AUROCKS_ITEM_LIMIT) goto fail;
            fg = (aurocks_frame *)aurocks_grow(frames, &frames_cap, depth, sizeof(*fg));
            if (!fg) goto fail;
            frames = fg;
            if (n) {
                aurocks_arg *ag;
                if (n > (size_t)-1 - used) goto fail;
                ag = (aurocks_arg *)aurocks_grow(args, &args_cap, used + n - 1, sizeof(*ag));
                if (!ag) goto fail;
                args = ag;
            }
            while (i) {
                const aurocks_edge *e = &p->edges[p->items[id].choice];
                aurocks_arg *arg = &args[used + --i];
                arg->child = e->child;
                arg->begin = p->items[e->prefix].end;
                arg->end = p->items[id].end;
                arg->value = NULL;
                id = e->prefix;
            }
            frames[depth].production = production;
            frames[depth].base = used;
            frames[depth++].next = 0;
            used += n;
            symbol = 0;
        }
        while (depth) {
            aurocks_frame *f = &frames[depth - 1];
            const size_t rule_id = p->items[f->production].rule;
            const aurocks_rule *r = &aurocks_rules[rule_id];
            if (f->next < r->length) {
                symbol = args[f->base + f->next++].child;
                if (symbol) break;
            } else {
                char *token = NULL;
                size_t i;
                for (i = 0; i < r->length; ++i) {
                    const aurocks_symbol *s = &aurocks_rhs[r->offset + i];
                    const aurocks_arg *arg = &args[f->base + i];
                    values[i] = arg->value;
                    if (s->use) {
                        const size_t begin = aurocks_skip(p, arg->begin);
                        const size_t length = arg->end - begin;
                        char *text = (char *)malloc(length + 1);
                        if (!text) {
                            size_t j;
                            for (j = 0; j < i; ++j)
                                if (aurocks_rhs[r->offset + j].use) free(values[j]);
                            goto fail;
                        }
                        memcpy(text, p->input + begin, length);
                        text[length] = '\0';
                        values[i] = text;
                        if (s->use & 2) token = text;
                    }
                }
                value = aurocks_action(rule_id, values, token);
                used = f->base;
                --depth;
                if (depth) {
                    f = &frames[depth - 1];
                    args[f->base + f->next - 1].value = value;
                }
            }
        }
        if (!depth) break;
    }
    goto done;
fail:
    p->failed = 1;
    value = NULL;
done:
    free(values);
    free(frames);
    free(args);
    return value;
}

static void *aurocks_parse_input(const char *input, const char **error
#ifdef AUROCKS_WITH_FAULTS
                                 , aurocks_fault_observer observer, void *data, const void *unused
#endif
) {
    aurocks_parser *p;
    aurocks_visit *stack = NULL;
    size_t stack_capacity = 0, at, size, selected = 0;
    unsigned count = 0;
    void *result = NULL;
#ifdef AUROCKS_WITH_FAULTS
    (void)unused;
#endif
    *error = input;
    if (!input) return NULL;
    for (size = 0; size < AUROCKS_INPUT_LIMIT && input[size]; ++size) {}
    if (size >= AUROCKS_INPUT_LIMIT) return NULL;
    p = (aurocks_parser *)calloc(1, sizeof(*p));
    if (!p) return NULL;
    p->input = input;
    p->length = size;
    p->chart = (aurocks_column *)calloc(size + 1, sizeof(*p->chart));
    p->layout = (size_t *)calloc(size + 1, sizeof(*p->layout));
    if (!p->chart || !p->layout) goto done;
    aurocks_recognize(p);
    if (p->failed) goto done;
    for (at = 0; at <= size; ++at) {
        const size_t id = aurocks_find(p, at, AUROCKS_RULES + aurocks_start, 0, 0);
        if (id) {
            const size_t skipped = aurocks_skip(p, at);
            int rank = 1;
            if (skipped > p->furthest) p->furthest = skipped;
            if (skipped != size) continue;
            if (!aurocks_resolve(p, id, &stack, &stack_capacity)) goto done;
            if (selected)
                rank = aurocks_priority(p, p->edges[p->items[id].choice].child,
                                        p->edges[p->items[selected].choice].child);
            if (rank > 0) {
                selected = id;
                count = p->items[id].count;
            } else if (!rank) {
                const size_t new_rule = p->items[p->edges[p->items[id].choice].child].rule;
                const size_t old_rule = p->items[p->edges[p->items[selected].choice].child].rule;
                if (new_rule < old_rule || (new_rule == old_rule && at > p->items[selected].end))
                    selected = id;
                count = 2;
            }
        }
    }
    if (!p->failed && selected && (!aurocks_ambiguity_error || count == 1)) {
        result = aurocks_evaluate(p, selected);
        if (!p->failed) *error = NULL;
    }
#ifdef AUROCKS_WITH_FAULTS
    else if (observer && !p->failed) {
        observer(data, AUROCKS_FAULT_EARLEY,
                 selected ? AUROCKS_FAULT_AMBIGUITY : AUROCKS_FAULT_SYNTAX, p, p->furthest, NULL);
    }
#endif
done:
    if (*error) *error = input + p->furthest;
    if (p->chart)
        for (at = 0; at <= size; ++at) free(p->chart[at].slots);
    free(p->chart);
    free(p->layout);
    free(p->items);
    free(p->edges);
    free(stack);
    free(p);
    return result;
}
)aurocks";

} // namespace aurocks::detail::portable
