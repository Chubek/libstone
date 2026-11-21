struct Three;
struct Three cephyr_three(struct Three, unsigned int);
struct Three clang_three(struct Three, unsigned int);

struct Three { unsigned char b[3]; };
struct Padded { unsigned char tag; unsigned int n; };
union Word { unsigned long n; unsigned char bytes[8]; };
struct Pointer { int *p; };
struct Nested { struct { unsigned short n; } a[2]; };

typedef struct Three ThreeFn(struct Three, unsigned int);

struct Padded cephyr_padded(struct Padded);
struct Padded clang_padded(struct Padded);
union Word cephyr_union(union Word);
union Word clang_union(union Word);
struct Pointer cephyr_pointer(struct Pointer);
struct Pointer clang_pointer(struct Pointer);
struct Nested cephyr_nested(const struct Nested);
struct Nested clang_nested(const struct Nested);

struct Three cephyr_stack(unsigned long, unsigned long, unsigned long, unsigned long,
                          unsigned long, unsigned long, unsigned long, unsigned long,
                          struct Three, unsigned long, struct Padded);
struct Three clang_stack(unsigned long, unsigned long, unsigned long, unsigned long,
                         unsigned long, unsigned long, unsigned long, unsigned long,
                         struct Three, unsigned long, struct Padded);
int cephyr_case(ThreeFn *);

struct Nine { unsigned char b[9]; };
struct TwoWords { unsigned long lo; unsigned long hi; };
struct Seventeen { unsigned char b[17]; };
struct Big { unsigned long a, b, c; };

typedef struct Nine NineFn(struct Nine, unsigned long);
typedef struct Big BigFn(unsigned long, unsigned long, unsigned long, unsigned long,
                         unsigned long, unsigned long, struct Big, unsigned long);
struct Nine cephyr_nine(struct Nine, unsigned long);
struct Nine clang_nine(struct Nine, unsigned long);
#define SEVEN unsigned long a, unsigned long b, unsigned long c, unsigned long d, \
              unsigned long e, unsigned long f, unsigned long g
struct TwoWords cephyr_seven(SEVEN);
struct TwoWords clang_seven(SEVEN);
unsigned long cephyr_split_scalar(SEVEN, struct TwoWords, unsigned long);
unsigned long clang_split_scalar(SEVEN, struct TwoWords, unsigned long);
struct TwoWords cephyr_split(SEVEN, struct TwoWords, unsigned long);
struct TwoWords clang_split(SEVEN, struct TwoWords, unsigned long);
struct Nine cephyr_stacked(SEVEN, unsigned long, struct Nine, unsigned long);
struct Nine clang_stacked(SEVEN, unsigned long, struct Nine, unsigned long);
struct Big cephyr_big(unsigned long, unsigned long, unsigned long, unsigned long,
                      unsigned long, unsigned long, struct Big, unsigned long);
struct Big clang_big(unsigned long, unsigned long, unsigned long, unsigned long,
                     unsigned long, unsigned long, struct Big, unsigned long);
struct Seventeen cephyr_noargs(void);
struct Seventeen clang_noargs(void);
int cephyr_wide_case(NineFn *, BigFn *);
