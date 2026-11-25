extern volatile int value;
extern int *pointer;
extern int transform(int);
extern int (*volatile alternate)(int);

int (*volatile callback)(int) = transform;

int (*get_callback(void))(int) { return transform; }
volatile int *get_value(void) { return &value; }

static void __attribute__((constructor(150))) initialize_first(void) {
  value *= 2;
}

__attribute__((export_name("public_run"))) int run(void) {
  volatile int local = *pointer;
  return value + transform(2) + callback(local) + alternate(local);
}
