/* Header-free Clang oracle: can be compiled with --target without a sysroot.
 * Inspect output assembly for loads from incoming stack/register locations. */
typedef long long I64;
typedef struct { I64 a, b; } Pair;
typedef struct { float a, b; } FloatPair;
typedef struct { FloatPair pair; float c; } NestedHfa;
typedef struct { double a, b; } DoublePair;
typedef struct { void *p; int i; } PointerRecord;

int scalar_stack(int a, int b, int c, int d, int e, int f, int g, int h,
                 signed char i, short j, int k) {
  return i + 3 * j + 5 * k;
}

I64 pair_at_last_gpr(I64 a, I64 b, I64 c, I64 d, I64 e, I64 f, I64 g, Pair p) {
  return p.a + 3 * p.b;
}

float nested_hfa(NestedHfa hfa) {
  return hfa.pair.a + hfa.pair.b + hfa.c;
}

double pair_fp(DoublePair dp) {
  return dp.a + dp.b;
}

float float_fallback(float a, float b, float c, float d, float e, float f,
                     float g, float h, FloatPair pair) {
  return pair.a + pair.b;
}

int pointer_record_size(void) { return sizeof(PointerRecord); }

int ms_positional(int a, double b, int c, float d, int e) {
  return a + (int)b + c + (int)d + e;
}
