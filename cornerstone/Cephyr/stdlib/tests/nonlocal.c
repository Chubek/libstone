#include <assert.h>
#include <limits.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>

extern int cephyr_test_jump_registers(jmp_buf);

/* Returning to the outer frame must discard real intervening stack slots. */
static __attribute__((noinline)) void descend(jmp_buf env, int depth, int result)
{
    if (env == NULL)
        return;
    volatile int saved = result;
    if (depth == 0)
        longjmp(env, saved);
    descend(env, depth - 1, saved);
    (void)saved; /* Prevent sibling-call optimization. */
}

static _Noreturn void jump(jmp_buf env, int depth, int result)
{
    descend(env, depth, result);
    abort();
}

static jmp_buf outer;

static void nested(void)
{
    jmp_buf inner;
    switch (setjmp(inner)) {
    case 0:
        jump(inner, 5, -7);
    case -7:
        jump(outer, 3, INT_MIN);
    default:
        assert(0);
    }
}

int main(void)
{
    jmp_buf env;
    volatile int modified = 1;
    int unchanged = 123;
    if (setjmp(env) == 0) {
        modified = 2;
        jump(env, 8, 0);
    }
    assert(modified == 2 && unchanged == 123);

    switch (setjmp(env)) {
    case 0: jump(env, 1, 0);
    case 1: break;
    default: assert(0);
    }
    switch (setjmp(outer)) {
    case 0: nested(); assert(0);
    case INT_MIN: break;
    default: assert(0);
    }

    /* longjmp does not roll back other components of machine state. */
    FILE *stream = tmpfile();
    assert(stream != NULL);
    if (!setjmp(env)) {
        assert(fputc('x', stream) == 'x');
        jump(env, 1, 1);
    }
    assert(ftell(stream) == 1);
    rewind(stream);
    assert(fgetc(stream) == 'x' && fclose(stream) == 0);
    assert(cephyr_test_jump_registers(env) == 0);
    return 0;
}
