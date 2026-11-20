#include <stdio.h>
#include <string.h>

void *parse_entry(const char *input);
void aurocks_free(void *ptr);

int main(void)
{
    static const struct {
        const char *input;
        const char *capture;
    } cases[] = {
        {" \tlong: \tabbb !\t ", "abbb"},
        {"long:a!", "a"},
        {"long:abb?", NULL},
        {"rollback:ab!", "a"},
        {"rollback:abcb!", "abc"},
        {"zero:aaa!", "aaa"},
        {"zero:!", NULL},
        {"class:A\377" "2_??x", "A\377" "2_??"},
        {"class:z-9Z?x", "z-9Z?"},
        {"class:A12_?x", NULL},
        {"class:A-2_???x", NULL},
        {"dot:\200!", "\200"},
        {"dot:\n!", NULL},
        {"eof:end", "end"},
        {"eof:end!", NULL},
        {"bol:pick!", "pick"},
        {"bol:prefix!", NULL},
        {"word:cat!", "cat"},
        {"word:catA", NULL},
        {"inside:xcatz", "cat"},
        {"inside:x catz", NULL},
    };
    int success = 1;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        char *capture = parse_entry(cases[i].input);
        if ((capture == NULL) != (cases[i].capture == NULL) ||
            (capture && strcmp(capture, cases[i].capture) != 0)) {
            fprintf(stderr, "Case %zu (%s): expected %s, got %s\n", i, cases[i].input,
                    cases[i].capture ? cases[i].capture : "(no match)", capture ? capture : "(no match)");
            success = 0;
        }
        aurocks_free(capture);
    }
    return success ? 0 : 1;
}
