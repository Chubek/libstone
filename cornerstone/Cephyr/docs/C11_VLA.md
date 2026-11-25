# Variable-length array objects

Cephyr implements automatic block-scope variable-length array objects on its
bundled AMD64, AArch64, and RV64 LP64 native targets, including multiple
variable dimensions and fixed outer dimensions with variable inner dimensions.
Each bound is evaluated once when its declaration executes; subscripts,
pointer arithmetic on derived array pointers, and `sizeof` use the captured
sizes. For example:

```c
unsigned long slice_size(int rows, int columns) {
    int matrix[rows][columns];
    matrix[0][0] = 42;
    columns = 1;               /* Does not change the captured stride. */
    return sizeof matrix[0];   /* Uses the original columns. */
}
```

`sizeof(int[n])` also evaluates its bound, whereas `_Alignof(int[n])` is
constant and does not evaluate it. Nonpositive runtime bounds or sizes that
exceed `PTRDIFF_MAX` trap. Arrays cannot have initializers, linkage, or static
storage duration. The native stack allocation is aligned to 16 bytes and
probes in 4 KiB steps; the surrounding stack's size limit still applies.
Control transfers that enter a VLA's scope without executing its declaration
are diagnosed; leaving the scope or returning reclaims its storage.

This is not full C11 variably modified type support. Cephyr still diagnoses
variably modified typedefs, explicit pointer-to-VLA declarators and type
names, and variable inner parameter dimensions. Accordingly,
`__STDC_NO_VLA__` remains defined.
