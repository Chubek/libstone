.text
entry:
sub sp, sp, #688
sub sp, sp, #1, lsl #12
str xzr, [sp]
add sp, sp, #16, lsl #12
add x12, sp, x12, uxtx
add x29, sp, #4095
mov x0, x9
mov w10, #-128
mov w10, #0
movz x9, #65535
movk x9, #4660, lsl #16
movk x9, #65535, lsl #32
movz x11, #32768, lsl #48
add x9, x9, x10
sub x9, x9, x10
adds x9, x9, x10
subs x9, x9, x10
mul x9, x9, x10
smulh x11, x9, x10
and x9, x9, x10
orr x9, x9, x10
eor x9, x9, x10
eor x9, x9, #0x80000000
eor x9, x9, #0x8000000000000000
lslv x9, x9, x10
lsrv x9, x9, x10
asrv x9, x9, x10
lsl x9, x9, #63
ubfx x9, x9, #0, #32
sbfx x9, x9, #0, #8
cmp x9, x11
cmp x11, x9, asr #63
cmp x10, #0
cmn x10, #1
cset x9, eq
cset x9, lt
cset x9, lo
cset x9, mi
cset x9, ls
csinv x9, x11, xzr, ne
sdiv x11, x9, x10
udiv x11, x9, x10
msub x9, x11, x10, x9
b.vc exit
b.eq exit
b.ne entry
cbnz x10, entry
b exit
brk #0
exit:
ldrsb x9, [x9]
ldrb w9, [x9]
ldrsh x9, [x9]
ldrh w9, [x9]
ldrsw x9, [x9]
ldr w9, [x9]
strb w10, [x9]
strh w10, [x9]
str w10, [x9]
ldr x9, [sp, #32760]
str x9, [sp, #32760]
ldr q7, [sp, #65520]
str q0, [sp, #65520]
ldr s7, [sp, #16380]
ldr d1, [sp, #32760]
fmov s1, w10
fmov d1, x10
fmov w9, s0
fmov x9, d0
fadd s0, s0, s1
fsub s0, s0, s1
fmul s0, s0, s1
fdiv s0, s0, s1
fadd d0, d0, d1
fsub d0, d0, d1
fmul d0, d0, d1
fdiv d0, d0, d1
fcmp s0, s1
fcmpe s0, s1
fcmp d0, d1
fcmpe d0, d1
fcvt d0, s0
fcvt s0, d0
fcvtzs x9, s0
fcvtzu x9, s0
fcvtzs x9, d0
fcvtzu x9, d0
scvtf s0, x9
ucvtf s0, x9
scvtf d0, x9
ucvtf d0, x9
adrp x9, "obj"
add x9, x9, :lo12:"obj"
adrp x9, :got:"obj"
ldr x9, [x9, :got_lo12:"obj"]
bl "x0"
blr x13
ret x30
