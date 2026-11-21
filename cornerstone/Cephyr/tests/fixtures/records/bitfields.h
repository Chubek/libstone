#include <stddef.h>

enum { SMALL_WIDTH = 1 + 2 };
typedef int Signed;
struct MixedBits {
    unsigned char prefix;
    unsigned small : SMALL_WIDTH;
    Signed delta : 5 * 2;
    _Bool flag : sizeof(char);
    unsigned : 2;
    unsigned char trailer;
    unsigned : 0;
    unsigned wide : 31;
    signed int sign : 1;
    unsigned char last;
};

struct CrossUnit { char prefix; unsigned value : 25, : 2; char last; };
struct Padding { char prefix; unsigned : 2; char last; };
struct Barrier { char prefix; unsigned : 0; char last; };
union UnnamedUnion { unsigned : 31; unsigned char tag; };
union ZeroUnion { unsigned : 0; unsigned char tag; };

_Static_assert(sizeof(struct MixedBits) == 12 && _Alignof(struct MixedBits) == 4, "mixed layout");
_Static_assert(offsetof(struct MixedBits, trailer) == 3, "trailer follows occupied bits");
_Static_assert(offsetof(struct MixedBits, last) == 8, "tail after the next unit");
_Static_assert(sizeof(struct CrossUnit) == 12 && offsetof(struct CrossUnit, last) == 8,
               "bit-field cannot cross its aligned unit");
_Static_assert(offsetof(struct Padding, last) == 2 && offsetof(struct Barrier, last) == 4,
               "unnamed storage and allocation-unit barrier");
#ifdef __aarch64__
_Static_assert(sizeof(struct Padding) == 4 && _Alignof(struct Padding) == 4, "AAPCS64 padding");
_Static_assert(sizeof(struct Barrier) == 8 && _Alignof(struct Barrier) == 4, "AAPCS64 barrier");
_Static_assert(_Alignof(union UnnamedUnion) == 4 && sizeof(union ZeroUnion) == 4, "AAPCS64 unions");
#else
_Static_assert(sizeof(struct Padding) == 3 && _Alignof(struct Padding) == 1, "SysV/RV64 padding");
_Static_assert(sizeof(struct Barrier) == 5 && _Alignof(struct Barrier) == 1, "SysV/RV64 barrier");
_Static_assert(_Alignof(union UnnamedUnion) == 1 && sizeof(union ZeroUnion) == 1, "SysV/RV64 unions");
#endif
_Static_assert(sizeof(union UnnamedUnion) == 4, "unnamed union storage");

extern struct MixedBits external_bits;
int cephyr_bits(void);
void external_update(struct MixedBits *);
