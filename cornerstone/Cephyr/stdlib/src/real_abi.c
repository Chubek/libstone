/* Public target compiler helper ABI. All operations after crossing the ABI
 * boundary are integer operations on object representations. */
#include "real_bits.h"

#if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__ || __LDBL_MANT_DIG__ != 113
#error "requires little-endian IEEE binary128 long double"
#endif
#if defined(__riscv) && (!defined(__riscv_float_abi_soft) || defined(__riscv_flen))
#error "RV64 libc requires the LP64 software float ABI"
#endif
_Static_assert(sizeof(float)==4 && sizeof(double)==8 && sizeof(long double)==16, "IEEE storage sizes");

static cephyr_real_bits raw_sf(float x) {
    union { float f; uint32_t u; } v = {.f=x};
    return (cephyr_real_bits){v.u,0};
}
static float val_sf(cephyr_real_bits x) {
    union { float f; uint32_t u; } v = {.u=(uint32_t)x.lo};
    return v.f;
}
static cephyr_real_bits raw_df(double x) {
    union { double f; uint64_t u; } v = {.f=x};
    return (cephyr_real_bits){v.u,0};
}
static double val_df(cephyr_real_bits x) {
    union { double f; uint64_t u; } v = {.u=x.lo};
    return v.f;
}
static cephyr_real_bits raw_tf(long double x) {
    union { long double f; cephyr_real_bits u; } v = {.f=x};
    return v.u;
}
static long double val_tf(cephyr_real_bits x) {
    union { long double f; cephyr_real_bits u; } v = {.u=x};
    return v.f;
}

#if defined(__aarch64__)
typedef int cmp_result;
#else
typedef long cmp_result;
#endif

#define BIN(T,s,w,name,op) T __##name##s##3(T a,T b) \
    { return val_##s(cephyr_real_calc(w,op,raw_##s(a),raw_##s(b))); }
#define CMP(T,s,w,name,nan_result) cmp_result __##name##s##2(T a,T b) \
    { int c=cephyr_real_cmp(w,raw_##s(a),raw_##s(b)); return c==2 ? nan_result : c; }
#define INT_CAST(T,s,w,I,i,n) \
    T __float##i##s(I a) { return val_##s(cephyr_real_from_int(w,a<0,a<0?0-(uint64_t)a:(uint64_t)a)); } \
    T __floatun##i##s(unsigned I a) { return val_##s(cephyr_real_from_int(w,0,a)); } \
    I __fix##s##i(T a) { return (I)cephyr_real_to_int(w,raw_##s(a),n,1); } \
    unsigned I __fixuns##s##i(T a) { return (unsigned I)cephyr_real_to_int(w,raw_##s(a),n,0); }
#define FAMILY(T,s,w) \
    BIN(T,s,w,add,'+') BIN(T,s,w,sub,'-') BIN(T,s,w,mul,'*') BIN(T,s,w,div,'/') \
    CMP(T,s,w,eq,1) CMP(T,s,w,ne,1) CMP(T,s,w,lt,1) CMP(T,s,w,le,1) CMP(T,s,w,cmp,1) \
    CMP(T,s,w,gt,-1) CMP(T,s,w,ge,-1) \
    cmp_result __unord##s##2(T a,T b) { return cephyr_real_cmp(w,raw_##s(a),raw_##s(b))==2; } \
    INT_CAST(T,s,w,int,si,32) INT_CAST(T,s,w,long,di,64)
FAMILY(float,sf,32)
FAMILY(double,df,64)
FAMILY(long double,tf,128)

#define CAST(name,A,a,wa,B,b,wb) B name(A x) \
    { return val_##b(cephyr_real_cast(wb,wa,raw_##a(x))); }
CAST(__extendsfdf2,float,sf,32,double,df,64)
CAST(__extendsftf2,float,sf,32,long double,tf,128)
CAST(__extenddftf2,double,df,64,long double,tf,128)
CAST(__trunctfdf2,long double,tf,128,double,df,64)
CAST(__trunctfsf2,long double,tf,128,float,sf,32)
CAST(__truncdfsf2,double,df,64,float,sf,32)

/* Freestanding compilers can lower clz calls elsewhere in libc to these
 * symbols on RV64IM. A doubling search avoids recognizing a clz idiom. */
int __clzdi2(uint64_t x)
{
    if (!x) return 64;
    int n = 0;
    for (unsigned step=32; step; step>>=1)
        if (!(x >> (64-step))) { n += step; x <<= step; }
    return n;
}
int __clzsi2(uint32_t x) { return __clzdi2((uint64_t)x)-32; }
