#include <stddef.h>
#include <stdio.h>

typedef int scan_fn(const unsigned char *, size_t, size_t *, size_t *, size_t *, size_t *);
extern scan_fn lr_scan_c;
extern scan_fn lr_scan_alt_c;
#ifdef FIRESTONE_LR_TEST_ASSEMBLY
extern scan_fn lr_scan_asm;
extern scan_fn lr_scan_alt_asm;
#endif

/* Compare every token (and EOF) against an independent matcher. */
static int check_input(scan_fn *scan, const char *backend, const unsigned char *text, size_t size, size_t index) {
    size_t cursor = 0;
    for (;;) {
        const size_t start = cursor;
        size_t want_begin = start, want_token = 0, want_length = 0;
        size_t got_begin = (size_t)-1, got_length = (size_t)-1;
        int want_status = 1;
        while (want_begin < size && text[want_begin] == ' ') ++want_begin;
        if (want_begin == size) {
            want_status = 0;
        } else if (text[want_begin] == 0 || text[want_begin] == 255) {
            want_token = 50;
            want_length = 1;
        } else if (text[want_begin] == 'A') {
            const int whole = size - want_begin >= 4 && text[want_begin + 1] == 'B'
                && text[want_begin + 2] == 'C' && text[want_begin + 3] == 'D';
            want_token = whole ? 40 : 30;
            want_length = whole ? 4 : 1;
        } else if (text[want_begin] == '@' || text[want_begin] == 'Z') {
            want_token = text[want_begin] == 'Z' ? (size_t)-1 : 0;
            want_length = 1;
        } else if (text[want_begin] >= 'a' && text[want_begin] <= 'z') {
            size_t end = want_begin + 1;
            while (end < size && text[end] >= 'a' && text[end] <= 'z') ++end;
            want_length = end - want_begin;
            want_token = want_length == 2 && text[want_begin] == 'i' && text[want_begin + 1] == 'f' ? 10 : 20;
        } else {
            want_status = -1;
            want_length = 1;
        }
        size_t got_token = ~want_token;
        const int status = scan(text, size, &cursor, &got_token, &got_begin, &got_length);
        if (status != want_status || got_begin != want_begin || cursor != want_begin + want_length ||
            got_length != want_length || got_token != want_token) {
            fprintf(stderr, "%s: scanner mismatch on corpus item %zu/%zu at byte %zu\n",
                    backend, size, index, start);
            return 0;
        }
        if (want_status == 0) return 1;
    }
}

static int check_corpus(scan_fn *scan, const char *backend) {
    for (unsigned byte = 0; byte < 256; ++byte) {
        const unsigned char text = (unsigned char)byte;
        if (!check_input(scan, backend, &text, 1, byte)) return 0;
    }
    static const unsigned char alphabet[] = {0, 255, '+', 'i', 'f', 'x', 'A', 'B', 'C', 'D', ' ', '@', 'Z'};
    unsigned char text[4];
    size_t count = sizeof(alphabet);
    for (size_t size = 2; size <= sizeof(text); ++size) {
        count *= sizeof(alphabet);
        for (size_t index = 0; index < count; ++index) {
            size_t digits = index;
            for (size_t pos = 0; pos < size; ++pos) {
                text[pos] = alphabet[digits % sizeof(alphabet)];
                digits /= sizeof(alphabet);
            }
            if (!check_input(scan, backend, text, size, index)) return 0;
        }
    }
    return 1;
}

static int check_scanner(scan_fn *scan, const char *backend) {
    static const unsigned char keyword[] = "if+";
    static const unsigned char identifier[] = "ifx+";
    static const unsigned char fallback[] = "ABC";
    static const unsigned char extended[] = "ABCD+";
    static const unsigned char invalid[] = {'+'};
    static const unsigned char lo[] = {0, '+'};
    static const unsigned char hi[] = {255, '+'};
    static const unsigned char zero_id[] = {'@'};
    static const unsigned char largest_id[] = {'Z'};
    static const struct {
        const unsigned char *input;
        size_t size;
        int status;
        size_t token;
        size_t length;
    } cases[] = {
        {keyword, sizeof(keyword) - 1, 1, 10, 2},
        {identifier, sizeof(identifier) - 1, 1, 20, 3},
        {fallback, sizeof(fallback) - 1, 1, 30, 1},
        {extended, sizeof(extended) - 1, 1, 40, 4},
        {invalid, sizeof(invalid), -1, 0, 1},
        {lo, sizeof(lo), 1, 50, 1},
        {hi, sizeof(hi), 1, 50, 1},
        {zero_id, sizeof(zero_id), 1, 0, 1},
        {largest_id, sizeof(largest_id), 1, (size_t)-1, 1},
        {NULL, 0, 0, 0, 0}
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        size_t cursor = 0, token = 0, begin = 42, length = 0;
        const int status = scan(cases[i].input, cases[i].size, &cursor, &token, &begin, &length);
        if (status != cases[i].status || begin != 0 || cursor != cases[i].length ||
            token != cases[i].token || length != cases[i].length) {
            fprintf(stderr, "%s: scanner mismatch at case %zu (status %d, token %zu, length %zu)\n",
                    backend, i, status, token, length);
            return 0;
        }
    }
    size_t cursor = 1, token = 2, begin = 3, length = 4;
    if (scan(NULL, 0, &cursor, &token, &begin, &length) != -2 ||
        scan(NULL, 1, &cursor, &token, &begin, &length) != -2 ||
        scan(keyword, sizeof(keyword) - 1, NULL, &token, &begin, &length) != -2 ||
        scan(keyword, sizeof(keyword) - 1, &cursor, NULL, &begin, &length) != -2 ||
        scan(keyword, sizeof(keyword) - 1, &cursor, &token, NULL, &length) != -2 ||
        scan(keyword, sizeof(keyword) - 1, &cursor, &token, &begin, NULL) != -2 ||
        cursor != 1 || token != 2 || begin != 3 || length != 4) return 0;
    static const unsigned char multiple[] = "  if x+ ";
    const size_t size = sizeof(multiple) - 1;
    cursor = 0;
    if (scan(multiple, size, &cursor, &token, &begin, &length) != 1 ||
        cursor != 4 || token != 10 || begin != 2 || length != 2) return 0;
    if (scan(multiple, size, &cursor, &token, &begin, &length) != 1 ||
        cursor != 6 || token != 20 || begin != 5 || length != 1) return 0;
    if (scan(multiple, size, &cursor, &token, &begin, &length) != -1 ||
        cursor != 7 || token != 0 || begin != 6 || length != 1) return 0;
    if (scan(multiple, size, &cursor, &token, &begin, &length) != 0 ||
        cursor != size || token != 0 || begin != size || length != 0) return 0;
    return check_corpus(scan, backend);
}

static int check_alternate(scan_fn *scan, const char *backend) {
    static const unsigned char input[] = {'i', '#', '#'};
    size_t cursor = 0, token = 0, begin = 0, length = 0;
    if (scan(input, sizeof(input), &cursor, &token, &begin, &length) != -1 ||
        cursor != 1 || token != 0 || begin != 0 || length != 1 ||
        scan(input, sizeof(input), &cursor, &token, &begin, &length) != 1 ||
        cursor != 2 || token != 90 || begin != 1 || length != 1 ||
        scan(input, sizeof(input), &cursor, &token, &begin, &length) != 1 ||
        cursor != 3 || token != 90 || begin != 2 || length != 1 ||
        scan(input, sizeof(input), &cursor, &token, &begin, &length) != 0 ||
        cursor != 3 || token != 0 || begin != 3 || length != 0) {
        fprintf(stderr, "%s: scanner with nonzero start state failed\n", backend);
        return 0;
    }
    return 1;
}

int main(void) {
    if (!check_scanner(lr_scan_c, "C99") || !check_alternate(lr_scan_alt_c, "C99")) return 1;
#ifdef FIRESTONE_LR_TEST_ASSEMBLY
    if (!check_scanner(lr_scan_asm, "assembly") || !check_alternate(lr_scan_alt_asm, "assembly")) return 1;
#endif
    return 0;
}
