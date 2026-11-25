volatile int value = 31;
static int adjacent[] __attribute__((aligned(32))) = {3, 7};
int *pointer = adjacent + 1;

int transform(int input) { return input + value; }

static int offset(int input) { return input + 3; }
int (*volatile alternate)(int) = offset;

static void __attribute__((constructor(200))) initialize(void) {
  value += 2;
}
