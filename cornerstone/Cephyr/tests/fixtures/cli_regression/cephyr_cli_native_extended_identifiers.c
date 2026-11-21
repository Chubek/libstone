#define NUMBER\u00C9 7
#define TWICE(\u03B1) ((α) + (\U000003B1))
#define PASTE(a, b) a ## b
#define STRINGIZE(x) #x

#if !defined(NUMBERÉ) || PASTE(NUMBER, É) != 7 || TWICE(2) != 4
#error extended macro names did not resolve by code point
#endif

static int use_local_names(int \u03b1) {
    typedef int \u00C9;
    É \U0001D11E = α + NUMBERÉ;
    int prefix\u0301 = 3;
    struct \u03A3 { int \u0394; } data = { .Δ = 𝄞 };
    goto \u03bb;
    return 100;
λ:
    return data.\U00000394 + prefix́;
}

int main(void) {
    const char *str = STRINGIZE(\U000000E9);
    return use_local_names(2) != 12 || (unsigned char)str[0] != 0xc3 ||
           (unsigned char)str[1] != 0xa9 || str[2] != 0;
}
