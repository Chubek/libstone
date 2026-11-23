	.text
	.p2align 4
	.globl parsed
	.type parsed, %function
parsed:
	sub sp, sp, #688
	str x29, [sp, #672]
	str x30, [sp, #680]
	add x29, sp, #672
.Larchon_0_0:
	add x9, sp, #624
	str x9, [sp, #0]
	add x9, sp, #632
	str x9, [sp, #8]
	add x9, sp, #640
	str x9, [sp, #16]
	add x9, sp, #656
	str x9, [sp, #24]
	adrp x9, "__cephyr_tu_string_0"
	add x9, x9, :lo12:"__cephyr_tu_string_0"
	str x9, [sp, #32]
	ldr x0, [sp, #32]
	ldr x1, [sp, #8]
	bl "strtold"
	str q0, [sp, #48]
	ldr x9, [sp, #48]
	str x9, [sp, #64]
	ldr x9, [sp, #56]
	str x9, [sp, #72]
	ldr x9, [sp, #16]
	ldr x10, [sp, #64]
	str x10, [x9]
	ldr x9, [sp, #16]
	ldr x10, [sp, #72]
	add x9, x9, #8
	str x10, [x9]
	adrp x9, "__cephyr_tu_string_1"
	add x9, x9, :lo12:"__cephyr_tu_string_1"
	str x9, [sp, #80]
	ldr x0, [sp, #80]
	ldr x1, [sp, #0]
	bl "wcstold"
	str q0, [sp, #96]
	ldr x9, [sp, #96]
	str x9, [sp, #112]
	ldr x9, [sp, #104]
	str x9, [sp, #120]
	ldr x9, [sp, #24]
	ldr x10, [sp, #112]
	str x10, [x9]
	ldr x9, [sp, #24]
	ldr x10, [sp, #120]
	add x9, x9, #8
	str x10, [x9]
	ldr x9, [sp, #8]
	ldr x9, [x9]
	str x9, [sp, #128]
	ldr x9, [sp, #128]
	ldrb w9, [x9]
	str x9, [sp, #136]
	ldr x9, [sp, #136]
	str x9, [sp, #144]
	movz x9, #33
	str x9, [sp, #152]
	ldr x9, [sp, #144]
	str x9, [sp, #160]
	ldr x9, [sp, #152]
	str x9, [sp, #168]
	ldr x9, [sp, #160]
	ldr x10, [sp, #168]
	cmp x9, x10
	cset x9, eq
	str x9, [sp, #176]
	movz x9, #0
	str x9, [sp, #184]
	ldr x9, [sp, #176]
	ldr x10, [sp, #184]
	cmp x9, x10
	cset x9, eq
	str x9, [sp, #192]
	movz x9, #0
	str x9, [sp, #200]
	ldr x9, [sp, #176]
	str x9, [sp, #208]
	movz x9, #0
	str x9, [sp, #216]
	ldr x9, [sp, #208]
	cbnz x9, .Larchon_0_8
	ldr x9, [sp, #216]
	str x9, [sp, #608]
	ldr x9, [sp, #608]
	str x9, [sp, #312]
	b .Larchon_0_2
.Larchon_0_8:
.Larchon_0_1:
	ldr x9, [sp, #0]
	ldr x9, [x9]
	str x9, [sp, #224]
	ldr x9, [sp, #224]
	ldr w9, [x9]
	str x9, [sp, #232]
	ldr x9, [sp, #232]
	str x9, [sp, #240]
	movz x9, #63
	str x9, [sp, #248]
	ldr x9, [sp, #240]
	str x9, [sp, #256]
	ldr x9, [sp, #248]
	str x9, [sp, #264]
	ldr x9, [sp, #256]
	ldr x10, [sp, #264]
	cmp x9, x10
	cset x9, eq
	str x9, [sp, #272]
	movz x9, #0
	str x9, [sp, #280]
	ldr x9, [sp, #272]
	ldr x10, [sp, #280]
	cmp x9, x10
	cset x9, eq
	str x9, [sp, #288]
	movz x9, #0
	str x9, [sp, #296]
	ldr x9, [sp, #272]
	str x9, [sp, #304]
	ldr x9, [sp, #304]
	str x9, [sp, #608]
	ldr x9, [sp, #608]
	str x9, [sp, #312]
.Larchon_0_2:
	movz x9, #0
	str x9, [sp, #320]
	ldr x9, [sp, #312]
	ldr x10, [sp, #320]
	cmp x9, x10
	cset x9, eq
	str x9, [sp, #328]
	movz x9, #0
	str x9, [sp, #336]
	ldr x9, [sp, #312]
	str x9, [sp, #344]
	movz x9, #0
	str x9, [sp, #352]
	ldr x9, [sp, #344]
	cbnz x9, .Larchon_0_9
	ldr x9, [sp, #352]
	str x9, [sp, #608]
	ldr x9, [sp, #608]
	str x9, [sp, #496]
	b .Larchon_0_4
.Larchon_0_9:
.Larchon_0_3:
	ldr x9, [sp, #24]
	ldr x9, [x9]
	str x9, [sp, #360]
	ldr x9, [sp, #24]
	add x9, x9, #8
	ldr x9, [x9]
	str x9, [sp, #368]
	movz x9, #1
	str x9, [sp, #376]
	movz x9, #0
	str x9, [sp, #384]
	ldr x9, [sp, #360]
	ldr x10, [sp, #368]
	str x10, [sp, #408]
	str x9, [sp, #400]
	ldr x9, [sp, #376]
	ldr x10, [sp, #384]
	str x10, [sp, #424]
	str x9, [sp, #416]
	ldr q0, [sp, #400]
	ldr q1, [sp, #416]
	bl "__eqtf2"
	str x0, [sp, #432]
	ldr x9, [sp, #432]
	sbfx x9, x9, #0, #32
	str x9, [sp, #440]
	movz x9, #0
	str x9, [sp, #448]
	ldr x9, [sp, #440]
	ldr x10, [sp, #448]
	cmp x9, x10
	cset x9, eq
	str x9, [sp, #456]
	movz x9, #0
	str x9, [sp, #464]
	ldr x9, [sp, #456]
	ldr x10, [sp, #464]
	cmp x9, x10
	cset x9, eq
	str x9, [sp, #472]
	movz x9, #0
	str x9, [sp, #480]
	ldr x9, [sp, #456]
	str x9, [sp, #488]
	ldr x9, [sp, #488]
	str x9, [sp, #608]
	ldr x9, [sp, #608]
	str x9, [sp, #496]
.Larchon_0_4:
	movz x9, #0
	str x9, [sp, #504]
	ldr x9, [sp, #496]
	ldr x10, [sp, #504]
	cmp x9, x10
	cset x9, eq
	str x9, [sp, #512]
	movz x9, #0
	str x9, [sp, #520]
	ldr x9, [sp, #496]
	str x9, [sp, #528]
	ldr x9, [sp, #528]
	cbnz x9, .Larchon_0_10
	b .Larchon_0_6
.Larchon_0_10:
.Larchon_0_5:
	ldr x9, [sp, #16]
	ldr x9, [x9]
	str x9, [sp, #536]
	ldr x9, [sp, #16]
	add x9, x9, #8
	ldr x9, [x9]
	str x9, [sp, #544]
	ldr x9, [sp, #536]
	str x9, [sp, #608]
	ldr x9, [sp, #544]
	str x9, [sp, #616]
	ldr x9, [sp, #608]
	str x9, [sp, #568]
	ldr x9, [sp, #616]
	str x9, [sp, #576]
	b .Larchon_0_7
.Larchon_0_6:
	movz x9, #0
	str x9, [sp, #552]
	movz x9, #0
	str x9, [sp, #560]
	ldr x9, [sp, #552]
	str x9, [sp, #608]
	ldr x9, [sp, #560]
	str x9, [sp, #616]
	ldr x9, [sp, #608]
	str x9, [sp, #568]
	ldr x9, [sp, #616]
	str x9, [sp, #576]
.Larchon_0_7:
	ldr x9, [sp, #568]
	ldr x10, [sp, #576]
	str x10, [sp, #600]
	str x9, [sp, #592]
	ldr q0, [sp, #592]
	ldr x29, [sp, #672]
	ldr x30, [sp, #680]
	add sp, sp, #688
	ret x30
	.size parsed, .-parsed
	.p2align 4
	.globl receive
	.type receive, %function
receive:
	sub sp, sp, #752
	str x1, [sp, #568]
	str x2, [sp, #576]
	str x3, [sp, #584]
	str x4, [sp, #592]
	str x5, [sp, #600]
	str x6, [sp, #608]
	str x7, [sp, #616]
	str q0, [sp, #624]
	str q1, [sp, #640]
	str q2, [sp, #656]
	str q3, [sp, #672]
	str q4, [sp, #688]
	str q5, [sp, #704]
	str q6, [sp, #720]
	str q7, [sp, #736]
	str x0, [sp, #0]
.Larchon_1_0:
	add x9, sp, #480
	str x9, [sp, #8]
	add x9, sp, #488
	str x9, [sp, #16]
	add x9, sp, #528
	str x9, [sp, #24]
	add x9, sp, #544
	str x9, [sp, #32]
	ldr x9, [sp, #0]
	sbfx x9, x9, #0, #32
	str x9, [sp, #40]
	ldr x9, [sp, #8]
	ldr x10, [sp, #40]
	str w10, [x9]
	ldr x9, [sp, #16]
	add x10, sp, #752
	str x10, [x9]
	add x10, sp, #624
	str x10, [x9, #8]
	add x10, sp, #752
	str x10, [x9, #16]
	mov w10, #-56
	str w10, [x9, #24]
	mov w10, #-128
	str w10, [x9, #28]
	movz x9, #0
	str x9, [sp, #48]
	movz x9, #0
	str x9, [sp, #56]
	ldr x9, [sp, #24]
	ldr x10, [sp, #48]
	str x10, [x9]
	ldr x9, [sp, #24]
	ldr x10, [sp, #56]
	add x9, x9, #8
	str x10, [x9]
	movz x9, #0
	str x9, [sp, #64]
	str x9, [sp, #72]
	ldr x9, [sp, #32]
	ldr x10, [sp, #72]
	str w10, [x9]
.Larchon_1_1:
	ldr x9, [sp, #32]
	ldrsw x9, [x9]
	str x9, [sp, #80]
	ldr x9, [sp, #80]
	sbfx x9, x9, #0, #32
	str x9, [sp, #88]
	ldr x9, [sp, #8]
	ldrsw x9, [x9]
	str x9, [sp, #96]
	ldr x9, [sp, #96]
	sbfx x9, x9, #0, #32
	str x9, [sp, #104]
	ldr x9, [sp, #88]
	sbfx x9, x9, #0, #32
	str x9, [sp, #112]
	ldr x9, [sp, #104]
	sbfx x9, x9, #0, #32
	str x9, [sp, #120]
	ldr x9, [sp, #112]
	ldr x10, [sp, #120]
	cmp x9, x10
	cset x9, lt
	str x9, [sp, #128]
	movz x9, #0
	str x9, [sp, #136]
	ldr x9, [sp, #128]
	ldr x10, [sp, #136]
	cmp x9, x10
	cset x9, eq
	str x9, [sp, #144]
	movz x9, #0
	str x9, [sp, #152]
	ldr x9, [sp, #128]
	str x9, [sp, #160]
	ldr x9, [sp, #160]
	cbnz x9, .Larchon_1_8
	b .Larchon_1_4
.Larchon_1_8:
.Larchon_1_2:
	movz x9, #28
	str x9, [sp, #168]
	ldr x9, [sp, #16]
	ldr x10, [sp, #168]
	add x9, x9, x10
	str x9, [sp, #176]
	ldr x9, [sp, #176]
	ldrsw x9, [x9]
	str x9, [sp, #184]
	ldr x9, [sp, #184]
	sbfx x9, x9, #0, #32
	str x9, [sp, #192]
	movz x9, #0
	str x9, [sp, #200]
	ldr x9, [sp, #192]
	ldr x10, [sp, #200]
	cmp x9, x10
	cset x9, lt
	str x9, [sp, #208]
	ldr x9, [sp, #208]
	cbnz x9, .Larchon_1_9
	b .Larchon_1_6
.Larchon_1_9:
	b .Larchon_1_5
.Larchon_1_3:
	ldr x9, [sp, #32]
	ldrsw x9, [x9]
	str x9, [sp, #216]
	ldr x9, [sp, #216]
	sbfx x9, x9, #0, #32
	str x9, [sp, #224]
	movz x9, #1
	str x9, [sp, #232]
	ldr x9, [sp, #224]
	sbfx x9, x9, #0, #32
	str x9, [sp, #240]
	ldr x9, [sp, #232]
	str x9, [sp, #248]
	ldr x9, [sp, #240]
	ldr x10, [sp, #248]
	add x9, x9, x10
	str x9, [sp, #256]
	sbfx x9, x9, #0, #32
	str x9, [sp, #264]
	sbfx x9, x9, #0, #32
	str x9, [sp, #272]
	sbfx x9, x9, #0, #32
	str x9, [sp, #280]
	ldr x9, [sp, #32]
	ldr x10, [sp, #280]
	str w10, [x9]
	b .Larchon_1_1
.Larchon_1_4:
	ldr x9, [sp, #24]
	ldr x9, [x9]
	str x9, [sp, #288]
	ldr x9, [sp, #24]
	add x9, x9, #8
	ldr x9, [x9]
	str x9, [sp, #296]
	ldr x9, [sp, #288]
	ldr x10, [sp, #296]
	str x10, [sp, #312]
	str x9, [sp, #304]
	ldr q0, [sp, #304]
	add sp, sp, #752
	ret x30
.Larchon_1_5:
	movz x9, #16
	str x9, [sp, #320]
	ldr x9, [sp, #16]
	ldr x10, [sp, #320]
	add x9, x9, x10
	str x9, [sp, #328]
	ldr x9, [sp, #328]
	ldr x9, [x9]
	str x9, [sp, #336]
	ldr x9, [sp, #336]
	ldr x10, [sp, #192]
	add x9, x9, x10
	str x9, [sp, #344]
	movz x9, #16
	str x9, [sp, #352]
	ldr x9, [sp, #192]
	ldr x10, [sp, #352]
	add x9, x9, x10
	str x9, [sp, #360]
	sbfx x9, x9, #0, #32
	str x9, [sp, #368]
	ldr x9, [sp, #176]
	ldr x10, [sp, #368]
	str w10, [x9]
	ldr x9, [sp, #344]
	str x9, [sp, #472]
	ldr x9, [sp, #472]
	str x9, [sp, #448]
	b .Larchon_1_7
.Larchon_1_6:
	ldr x9, [sp, #16]
	ldr x9, [x9]
	str x9, [sp, #376]
	ldr x9, [sp, #376]
	str x9, [sp, #384]
	movz x9, #15
	str x9, [sp, #392]
	ldr x9, [sp, #384]
	ldr x10, [sp, #392]
	add x9, x9, x10
	str x9, [sp, #400]
	movz x9, #65520
	movk x9, #65535, lsl #16
	movk x9, #65535, lsl #32
	movk x9, #65535, lsl #48
	str x9, [sp, #408]
	ldr x9, [sp, #400]
	ldr x10, [sp, #408]
	and x9, x9, x10
	str x9, [sp, #416]
	ldr x9, [sp, #416]
	str x9, [sp, #424]
	movz x9, #16
	str x9, [sp, #432]
	ldr x9, [sp, #424]
	ldr x10, [sp, #432]
	add x9, x9, x10
	str x9, [sp, #440]
	ldr x9, [sp, #16]
	ldr x10, [sp, #440]
	str x10, [x9]
	ldr x9, [sp, #424]
	str x9, [sp, #472]
	ldr x9, [sp, #472]
	str x9, [sp, #448]
.Larchon_1_7:
	ldr x9, [sp, #448]
	ldr x9, [x9]
	str x9, [sp, #456]
	ldr x9, [sp, #448]
	add x9, x9, #8
	ldr x9, [x9]
	str x9, [sp, #464]
	ldr x9, [sp, #24]
	ldr x10, [sp, #456]
	str x10, [x9]
	ldr x9, [sp, #24]
	ldr x10, [sp, #464]
	add x9, x9, #8
	str x10, [x9]
	b .Larchon_1_3
	.size receive, .-receive
	.p2align 4
	.globl send
	.type send, %function
send:
	sub sp, sp, #272
	str x29, [sp, #256]
	str x30, [sp, #264]
	add x29, sp, #256
.Larchon_2_0:
	movz x9, #9
	str x9, [sp, #32]
	str x9, [sp, #40]
	str x9, [sp, #48]
	movz x9, #0
	str x9, [sp, #56]
	movz x9, #0
	movk x9, #16368, lsl #48
	str x9, [sp, #64]
	movz x9, #0
	movk x9, #16384, lsl #48
	str x9, [sp, #72]
	movz x9, #0
	movk x9, #16392, lsl #48
	str x9, [sp, #80]
	movz x9, #0
	movk x9, #16400, lsl #48
	str x9, [sp, #88]
	movz x9, #0
	movk x9, #16404, lsl #48
	str x9, [sp, #96]
	movz x9, #0
	movk x9, #16408, lsl #48
	str x9, [sp, #104]
	movz x9, #0
	movk x9, #16412, lsl #48
	str x9, [sp, #112]
	movz x9, #0
	movk x9, #16416, lsl #48
	str x9, [sp, #120]
	adrp x9, "table"
	add x9, x9, :lo12:"table"
	str x9, [sp, #128]
	movz x9, #3
	str x9, [sp, #136]
	ldr x9, [sp, #136]
	str x9, [sp, #144]
	movz x9, #48
	str x9, [sp, #152]
	ldr x9, [sp, #128]
	ldr x10, [sp, #152]
	add x9, x9, x10
	str x9, [sp, #160]
	ldr x9, [sp, #160]
	ldr x9, [x9]
	str x9, [sp, #168]
	ldr x9, [sp, #160]
	add x9, x9, #8
	ldr x9, [x9]
	str x9, [sp, #176]
	ldr x9, [sp, #168]
	ldr x10, [sp, #176]
	str x10, [sp, #200]
	str x9, [sp, #192]
	ldr x0, [sp, #48]
	ldr d0, [sp, #56]
	ldr d1, [sp, #64]
	ldr d2, [sp, #72]
	ldr d3, [sp, #80]
	ldr d4, [sp, #88]
	ldr d5, [sp, #96]
	ldr d6, [sp, #104]
	ldr d7, [sp, #112]
	ldr x9, [sp, #120]
	str x9, [sp, #0]
	ldr x9, [sp, #192]
	str x9, [sp, #16]
	ldr x9, [sp, #200]
	str x9, [sp, #24]
	bl "__cephyr_tu_vararg_0"
	str q0, [sp, #208]
	ldr x9, [sp, #208]
	str x9, [sp, #224]
	ldr x9, [sp, #216]
	str x9, [sp, #232]
	ldr x9, [sp, #224]
	ldr x10, [sp, #232]
	str x10, [sp, #248]
	str x9, [sp, #240]
	ldr q0, [sp, #240]
	ldr x29, [sp, #256]
	ldr x30, [sp, #264]
	add sp, sp, #272
	ret x30
	.size send, .-send
	.p2align 4
	.globl callback
	.type callback, %function
callback:
	sub sp, sp, #224
	str x29, [sp, #208]
	str x30, [sp, #216]
	add x29, sp, #208
	str x0, [sp, #0]
	fmov x9, d0
	str x9, [sp, #8]
	str q1, [sp, #16]
.Larchon_3_0:
	add x9, sp, #176
	str x9, [sp, #32]
	add x9, sp, #184
	str x9, [sp, #40]
	add x9, sp, #192
	str x9, [sp, #48]
	ldr x9, [sp, #32]
	ldr x10, [sp, #0]
	str x10, [x9]
	ldr x9, [sp, #40]
	ldr x10, [sp, #8]
	str x10, [x9]
	ldr x9, [sp, #16]
	str x9, [sp, #56]
	ldr x9, [sp, #24]
	str x9, [sp, #64]
	ldr x9, [sp, #48]
	ldr x10, [sp, #56]
	str x10, [x9]
	ldr x9, [sp, #48]
	ldr x10, [sp, #64]
	add x9, x9, #8
	str x10, [x9]
	ldr x9, [sp, #32]
	ldr x9, [x9]
	str x9, [sp, #72]
	ldr x9, [sp, #40]
	ldr x9, [x9]
	str x9, [sp, #80]
	ldr x9, [sp, #48]
	ldr x9, [x9]
	str x9, [sp, #88]
	ldr x9, [sp, #48]
	add x9, x9, #8
	ldr x9, [x9]
	str x9, [sp, #96]
	ldr x9, [sp, #88]
	ldr x10, [sp, #96]
	str x10, [sp, #120]
	str x9, [sp, #112]
	ldr d0, [sp, #80]
	ldr q1, [sp, #112]
	ldr x13, [sp, #72]
	blr x13
	str q0, [sp, #128]
	ldr x9, [sp, #128]
	str x9, [sp, #144]
	ldr x9, [sp, #136]
	str x9, [sp, #152]
	ldr x9, [sp, #144]
	ldr x10, [sp, #152]
	str x10, [sp, #168]
	str x9, [sp, #160]
	ldr q0, [sp, #160]
	ldr x29, [sp, #208]
	ldr x30, [sp, #216]
	add sp, sp, #224
	ret x30
	.size callback, .-callback
	.p2align 4
	.globl compute
	.type compute, %function
compute:
	sub sp, sp, #672
	str x29, [sp, #656]
	str x30, [sp, #664]
	add x29, sp, #656
	str x0, [sp, #0]
	str x1, [sp, #8]
	fmov w9, s0
	str x9, [sp, #16]
	fmov x9, d1
	str x9, [sp, #24]
.Larchon_4_0:
	add x9, sp, #608
	str x9, [sp, #32]
	add x9, sp, #616
	str x9, [sp, #40]
	add x9, sp, #624
	str x9, [sp, #48]
	add x9, sp, #632
	str x9, [sp, #56]
	add x9, sp, #640
	str x9, [sp, #64]
	ldr x9, [sp, #32]
	ldr x10, [sp, #0]
	str x10, [x9]
	ldr x9, [sp, #40]
	ldr x10, [sp, #8]
	str x10, [x9]
	ldr x9, [sp, #48]
	ldr x10, [sp, #16]
	str w10, [x9]
	ldr x9, [sp, #56]
	ldr x10, [sp, #24]
	str x10, [x9]
	ldr x9, [sp, #32]
	ldr x9, [x9]
	str x9, [sp, #72]
	ldr x9, [sp, #72]
	ldr x9, [x9]
	str x9, [sp, #80]
	ldr x9, [sp, #72]
	add x9, x9, #8
	ldr x9, [x9]
	str x9, [sp, #88]
	ldr x9, [sp, #40]
	ldr x9, [x9]
	str x9, [sp, #96]
	ldr x0, [sp, #96]
	bl "__floatunditf"
	str q0, [sp, #112]
	ldr x9, [sp, #112]
	str x9, [sp, #128]
	ldr x9, [sp, #120]
	str x9, [sp, #136]
	ldr x9, [sp, #80]
	ldr x10, [sp, #88]
	str x10, [sp, #152]
	str x9, [sp, #144]
	ldr x9, [sp, #128]
	ldr x10, [sp, #136]
	str x10, [sp, #168]
	str x9, [sp, #160]
	ldr q0, [sp, #144]
	ldr q1, [sp, #160]
	bl "__addtf3"
	str q0, [sp, #176]
	ldr x9, [sp, #176]
	str x9, [sp, #192]
	ldr x9, [sp, #184]
	str x9, [sp, #200]
	ldr x9, [sp, #48]
	ldr w9, [x9]
	str x9, [sp, #208]
	ldr s0, [sp, #208]
	bl "__extendsftf2"
	str q0, [sp, #224]
	ldr x9, [sp, #224]
	str x9, [sp, #240]
	ldr x9, [sp, #232]
	str x9, [sp, #248]
	ldr x9, [sp, #192]
	ldr x10, [sp, #200]
	str x10, [sp, #264]
	str x9, [sp, #256]
	ldr x9, [sp, #240]
	ldr x10, [sp, #248]
	str x10, [sp, #280]
	str x9, [sp, #272]
	ldr q0, [sp, #256]
	ldr q1, [sp, #272]
	bl "__multf3"
	str q0, [sp, #288]
	ldr x9, [sp, #288]
	str x9, [sp, #304]
	ldr x9, [sp, #296]
	str x9, [sp, #312]
	ldr x9, [sp, #56]
	ldr x9, [x9]
	str x9, [sp, #320]
	ldr d0, [sp, #320]
	bl "__extenddftf2"
	str q0, [sp, #336]
	ldr x9, [sp, #336]
	str x9, [sp, #352]
	ldr x9, [sp, #344]
	str x9, [sp, #360]
	ldr x9, [sp, #304]
	ldr x10, [sp, #312]
	str x10, [sp, #376]
	str x9, [sp, #368]
	ldr x9, [sp, #352]
	ldr x10, [sp, #360]
	str x10, [sp, #392]
	str x9, [sp, #384]
	ldr q0, [sp, #368]
	ldr q1, [sp, #384]
	bl "__divtf3"
	str q0, [sp, #400]
	ldr x9, [sp, #400]
	str x9, [sp, #416]
	ldr x9, [sp, #408]
	str x9, [sp, #424]
	ldr x9, [sp, #64]
	ldr x10, [sp, #416]
	str x10, [x9]
	ldr x9, [sp, #64]
	ldr x10, [sp, #424]
	add x9, x9, #8
	str x10, [x9]
	ldr x9, [sp, #32]
	ldr x9, [x9]
	str x9, [sp, #432]
	ldr x9, [sp, #64]
	ldr x9, [x9]
	str x9, [sp, #440]
	ldr x9, [sp, #64]
	add x9, x9, #8
	ldr x9, [x9]
	str x9, [sp, #448]
	movz x9, #0
	movk x9, #32768, lsl #48
	str x9, [sp, #456]
	ldr x9, [sp, #448]
	ldr x10, [sp, #456]
	eor x9, x9, x10
	str x9, [sp, #464]
	ldr x9, [sp, #432]
	ldr x10, [sp, #440]
	str x10, [x9]
	ldr x9, [sp, #432]
	ldr x10, [sp, #464]
	add x9, x9, #8
	str x10, [x9]
	ldr x9, [sp, #64]
	ldr x9, [x9]
	str x9, [sp, #472]
	ldr x9, [sp, #64]
	add x9, x9, #8
	ldr x9, [x9]
	str x9, [sp, #480]
	ldr x9, [sp, #40]
	ldr x9, [x9]
	str x9, [sp, #488]
	ldr x0, [sp, #488]
	bl "__floatunditf"
	str q0, [sp, #496]
	ldr x9, [sp, #496]
	str x9, [sp, #512]
	ldr x9, [sp, #504]
	str x9, [sp, #520]
	ldr x9, [sp, #472]
	ldr x10, [sp, #480]
	str x10, [sp, #536]
	str x9, [sp, #528]
	ldr x9, [sp, #512]
	ldr x10, [sp, #520]
	str x10, [sp, #552]
	str x9, [sp, #544]
	ldr q0, [sp, #528]
	ldr q1, [sp, #544]
	bl "__subtf3"
	str q0, [sp, #560]
	ldr x9, [sp, #560]
	str x9, [sp, #576]
	ldr x9, [sp, #568]
	str x9, [sp, #584]
	ldr x9, [sp, #576]
	ldr x10, [sp, #584]
	str x10, [sp, #600]
	str x9, [sp, #592]
	ldr q0, [sp, #592]
	ldr x29, [sp, #656]
	ldr x30, [sp, #664]
	add sp, sp, #672
	ret x30
	.size compute, .-compute
	.p2align 4
	.globl narrow
	.type narrow, %function
narrow:
	sub sp, sp, #672
	str x29, [sp, #656]
	str x30, [sp, #664]
	add x29, sp, #656
	str q0, [sp, #0]
.Larchon_5_0:
	add x9, sp, #640
	str x9, [sp, #16]
	ldr x9, [sp, #0]
	str x9, [sp, #24]
	ldr x9, [sp, #8]
	str x9, [sp, #32]
	ldr x9, [sp, #16]
	ldr x10, [sp, #24]
	str x10, [x9]
	ldr x9, [sp, #16]
	ldr x10, [sp, #32]
	add x9, x9, #8
	str x10, [x9]
	ldr x9, [sp, #16]
	ldr x9, [x9]
	str x9, [sp, #40]
	ldr x9, [sp, #16]
	add x9, x9, #8
	ldr x9, [x9]
	str x9, [sp, #48]
	ldr x9, [sp, #40]
	ldr x10, [sp, #48]
	str x10, [sp, #72]
	str x9, [sp, #64]
	ldr q0, [sp, #64]
	bl "__fixtfdi"
	str x0, [sp, #80]
	ldr x9, [sp, #80]
	sbfx x9, x9, #0, #32
	str x9, [sp, #88]
	movz x9, #2
	str x9, [sp, #96]
	movz x9, #0
	str x9, [sp, #104]
	ldr x9, [sp, #104]
	str x9, [sp, #112]
	ldr x9, [sp, #96]
	str x9, [sp, #120]
	movz x9, #65534
	movk x9, #65535, lsl #16
	movk x9, #65535, lsl #32
	movk x9, #65535, lsl #48
	str x9, [sp, #128]
	str x9, [sp, #136]
	ldr x9, [sp, #88]
	sbfx x9, x9, #0, #32
	str x9, [sp, #144]
	ldr x9, [sp, #136]
	str x9, [sp, #152]
	ldr x9, [sp, #144]
	ldr x10, [sp, #152]
	cmp x9, x10
	cset x9, eq
	str x9, [sp, #160]
	movz x9, #0
	str x9, [sp, #168]
	ldr x9, [sp, #160]
	ldr x10, [sp, #168]
	cmp x9, x10
	cset x9, eq
	str x9, [sp, #176]
	movz x9, #0
	str x9, [sp, #184]
	ldr x9, [sp, #160]
	str x9, [sp, #192]
	movz x9, #0
	str x9, [sp, #200]
	ldr x9, [sp, #192]
	cbnz x9, .Larchon_5_7
	ldr x9, [sp, #200]
	str x9, [sp, #632]
	ldr x9, [sp, #632]
	str x9, [sp, #312]
	b .Larchon_5_2
.Larchon_5_7:
.Larchon_5_1:
	ldr x9, [sp, #16]
	ldr x9, [x9]
	str x9, [sp, #208]
	ldr x9, [sp, #16]
	add x9, x9, #8
	ldr x9, [x9]
	str x9, [sp, #216]
	movz x9, #0
	movk x9, #32768, lsl #48
	str x9, [sp, #224]
	ldr x9, [sp, #216]
	ldr x10, [sp, #224]
	eor x9, x9, x10
	str x9, [sp, #232]
	ldr x9, [sp, #208]
	ldr x10, [sp, #232]
	str x10, [sp, #248]
	str x9, [sp, #240]
	ldr q0, [sp, #240]
	bl "__fixunstfdi"
	str x0, [sp, #256]
	movz x9, #2
	str x9, [sp, #264]
	ldr x9, [sp, #256]
	ldr x10, [sp, #264]
	cmp x9, x10
	cset x9, eq
	str x9, [sp, #272]
	movz x9, #0
	str x9, [sp, #280]
	ldr x9, [sp, #272]
	ldr x10, [sp, #280]
	cmp x9, x10
	cset x9, eq
	str x9, [sp, #288]
	movz x9, #0
	str x9, [sp, #296]
	ldr x9, [sp, #272]
	str x9, [sp, #304]
	ldr x9, [sp, #304]
	str x9, [sp, #632]
	ldr x9, [sp, #632]
	str x9, [sp, #312]
.Larchon_5_2:
	movz x9, #0
	str x9, [sp, #320]
	ldr x9, [sp, #312]
	ldr x10, [sp, #320]
	cmp x9, x10
	cset x9, eq
	str x9, [sp, #328]
	movz x9, #0
	str x9, [sp, #336]
	ldr x9, [sp, #312]
	str x9, [sp, #344]
	movz x9, #0
	str x9, [sp, #352]
	ldr x9, [sp, #344]
	cbnz x9, .Larchon_5_8
	ldr x9, [sp, #352]
	str x9, [sp, #632]
	ldr x9, [sp, #632]
	str x9, [sp, #464]
	b .Larchon_5_4
.Larchon_5_8:
.Larchon_5_3:
	ldr x9, [sp, #16]
	ldr x9, [x9]
	str x9, [sp, #360]
	ldr x9, [sp, #16]
	add x9, x9, #8
	ldr x9, [x9]
	str x9, [sp, #368]
	ldr x9, [sp, #360]
	ldr x10, [sp, #368]
	str x10, [sp, #392]
	str x9, [sp, #384]
	ldr q0, [sp, #384]
	bl "__trunctfdf2"
	fmov x9, d0
	str x9, [sp, #400]
	movz x9, #0
	movk x9, #16388, lsl #48
	str x9, [sp, #408]
	ldr x9, [sp, #408]
	eor x9, x9, #0x8000000000000000
	str x9, [sp, #416]
	ldr x9, [sp, #400]
	ldr x10, [sp, #416]
	fmov d0, x9
	fmov d1, x10
	fcmp d0, d1
	cset x9, eq
	str x9, [sp, #424]
	movz x9, #0
	str x9, [sp, #432]
	ldr x9, [sp, #424]
	ldr x10, [sp, #432]
	cmp x9, x10
	cset x9, eq
	str x9, [sp, #440]
	movz x9, #0
	str x9, [sp, #448]
	ldr x9, [sp, #440]
	ldr x10, [sp, #448]
	cmp x9, x10
	cset x9, eq
	str x9, [sp, #456]
	ldr x9, [sp, #456]
	str x9, [sp, #632]
	ldr x9, [sp, #632]
	str x9, [sp, #464]
.Larchon_5_4:
	movz x9, #0
	str x9, [sp, #472]
	ldr x9, [sp, #464]
	ldr x10, [sp, #472]
	cmp x9, x10
	cset x9, eq
	str x9, [sp, #480]
	movz x9, #0
	str x9, [sp, #488]
	ldr x9, [sp, #464]
	str x9, [sp, #496]
	movz x9, #0
	str x9, [sp, #504]
	ldr x9, [sp, #496]
	cbnz x9, .Larchon_5_9
	ldr x9, [sp, #504]
	str x9, [sp, #632]
	ldr x9, [sp, #632]
	str x9, [sp, #608]
	b .Larchon_5_6
.Larchon_5_9:
.Larchon_5_5:
	ldr x9, [sp, #16]
	ldr x9, [x9]
	str x9, [sp, #512]
	ldr x9, [sp, #16]
	add x9, x9, #8
	ldr x9, [x9]
	str x9, [sp, #520]
	ldr x9, [sp, #512]
	ldr x10, [sp, #520]
	str x10, [sp, #536]
	str x9, [sp, #528]
	ldr q0, [sp, #528]
	bl "__trunctfsf2"
	fmov w9, s0
	str x9, [sp, #544]
	movz x9, #0
	movk x9, #16416, lsl #16
	str x9, [sp, #552]
	ldr x9, [sp, #552]
	eor x9, x9, #0x80000000
	str x9, [sp, #560]
	ldr x9, [sp, #544]
	ldr x10, [sp, #560]
	fmov s0, w9
	fmov s1, w10
	fcmp s0, s1
	cset x9, eq
	str x9, [sp, #568]
	movz x9, #0
	str x9, [sp, #576]
	ldr x9, [sp, #568]
	ldr x10, [sp, #576]
	cmp x9, x10
	cset x9, eq
	str x9, [sp, #584]
	movz x9, #0
	str x9, [sp, #592]
	ldr x9, [sp, #584]
	ldr x10, [sp, #592]
	cmp x9, x10
	cset x9, eq
	str x9, [sp, #600]
	ldr x9, [sp, #600]
	str x9, [sp, #632]
	ldr x9, [sp, #632]
	str x9, [sp, #608]
.Larchon_5_6:
	ldr x9, [sp, #608]
	str x9, [sp, #616]
	str x9, [sp, #624]
	mov x0, x9
	ldr x29, [sp, #656]
	ldr x30, [sp, #664]
	add sp, sp, #672
	ret x30
	.size narrow, .-narrow
	.p2align 4
	.globl predicates
	.type predicates, %function
predicates:
	sub sp, sp, #1344
	str x29, [sp, #1328]
	str x30, [sp, #1336]
	add x29, sp, #1328
	str q0, [sp, #0]
	str q1, [sp, #16]
.Larchon_6_0:
	add x9, sp, #1296
	str x9, [sp, #32]
	add x9, sp, #1312
	str x9, [sp, #40]
	ldr x9, [sp, #0]
	str x9, [sp, #48]
	ldr x9, [sp, #8]
	str x9, [sp, #56]
	ldr x9, [sp, #32]
	ldr x10, [sp, #48]
	str x10, [x9]
	ldr x9, [sp, #32]
	ldr x10, [sp, #56]
	add x9, x9, #8
	str x10, [x9]
	ldr x9, [sp, #16]
	str x9, [sp, #64]
	ldr x9, [sp, #24]
	str x9, [sp, #72]
	ldr x9, [sp, #40]
	ldr x10, [sp, #64]
	str x10, [x9]
	ldr x9, [sp, #40]
	ldr x10, [sp, #72]
	add x9, x9, #8
	str x10, [x9]
	ldr x9, [sp, #32]
	ldr x9, [x9]
	str x9, [sp, #80]
	ldr x9, [sp, #32]
	add x9, x9, #8
	ldr x9, [x9]
	str x9, [sp, #88]
	movz x9, #0
	str x9, [sp, #96]
	movz x9, #0
	str x9, [sp, #104]
	ldr x9, [sp, #80]
	ldr x10, [sp, #88]
	str x10, [sp, #120]
	str x9, [sp, #112]
	ldr x9, [sp, #96]
	ldr x10, [sp, #104]
	str x10, [sp, #136]
	str x9, [sp, #128]
	ldr q0, [sp, #112]
	ldr q1, [sp, #128]
	bl "__eqtf2"
	str x0, [sp, #144]
	ldr x9, [sp, #144]
	sbfx x9, x9, #0, #32
	str x9, [sp, #152]
	movz x9, #0
	str x9, [sp, #160]
	ldr x9, [sp, #152]
	ldr x10, [sp, #160]
	cmp x9, x10
	cset x9, eq
	str x9, [sp, #168]
	movz x9, #0
	str x9, [sp, #176]
	ldr x9, [sp, #168]
	ldr x10, [sp, #176]
	cmp x9, x10
	cset x9, eq
	str x9, [sp, #184]
	movz x9, #0
	str x9, [sp, #192]
	ldr x9, [sp, #168]
	str x9, [sp, #200]
	movz x9, #2
	str x9, [sp, #208]
	ldr x9, [sp, #32]
	ldr x9, [x9]
	str x9, [sp, #216]
	ldr x9, [sp, #32]
	add x9, x9, #8
	ldr x9, [x9]
	str x9, [sp, #224]
	ldr x9, [sp, #40]
	ldr x9, [x9]
	str x9, [sp, #232]
	ldr x9, [sp, #40]
	add x9, x9, #8
	ldr x9, [x9]
	str x9, [sp, #240]
	ldr x9, [sp, #216]
	ldr x10, [sp, #224]
	str x10, [sp, #264]
	str x9, [sp, #256]
	ldr x9, [sp, #232]
	ldr x10, [sp, #240]
	str x10, [sp, #280]
	str x9, [sp, #272]
	ldr q0, [sp, #256]
	ldr q1, [sp, #272]
	bl "__eqtf2"
	str x0, [sp, #288]
	ldr x9, [sp, #288]
	sbfx x9, x9, #0, #32
	str x9, [sp, #296]
	movz x9, #0
	str x9, [sp, #304]
	ldr x9, [sp, #296]
	ldr x10, [sp, #304]
	cmp x9, x10
	cset x9, eq
	str x9, [sp, #312]
	movz x9, #0
	str x9, [sp, #320]
	ldr x9, [sp, #312]
	ldr x10, [sp, #320]
	cmp x9, x10
	cset x9, eq
	str x9, [sp, #328]
	ldr x9, [sp, #208]
	str x9, [sp, #336]
	ldr x9, [sp, #328]
	str x9, [sp, #344]
	ldr x9, [sp, #344]
	lsl x9, x9, #1
	str x9, [sp, #352]
	sbfx x9, x9, #0, #32
	str x9, [sp, #360]
	ldr x9, [sp, #200]
	str x9, [sp, #368]
	ldr x9, [sp, #360]
	sbfx x9, x9, #0, #32
	str x9, [sp, #376]
	ldr x9, [sp, #368]
	ldr x10, [sp, #376]
	add x9, x9, x10
	str x9, [sp, #384]
	sbfx x9, x9, #0, #32
	str x9, [sp, #392]
	movz x9, #4
	str x9, [sp, #400]
	ldr x9, [sp, #32]
	ldr x9, [x9]
	str x9, [sp, #408]
	ldr x9, [sp, #32]
	add x9, x9, #8
	ldr x9, [x9]
	str x9, [sp, #416]
	ldr x9, [sp, #40]
	ldr x9, [x9]
	str x9, [sp, #424]
	ldr x9, [sp, #40]
	add x9, x9, #8
	ldr x9, [x9]
	str x9, [sp, #432]
	ldr x9, [sp, #408]
	ldr x10, [sp, #416]
	str x10, [sp, #456]
	str x9, [sp, #448]
	ldr x9, [sp, #424]
	ldr x10, [sp, #432]
	str x10, [sp, #472]
	str x9, [sp, #464]
	ldr q0, [sp, #448]
	ldr q1, [sp, #464]
	bl "__letf2"
	str x0, [sp, #480]
	ldr x9, [sp, #480]
	sbfx x9, x9, #0, #32
	str x9, [sp, #488]
	movz x9, #0
	str x9, [sp, #496]
	ldr x9, [sp, #488]
	ldr x10, [sp, #496]
	cmp x9, x10
	cset x9, lt
	str x9, [sp, #504]
	ldr x9, [sp, #400]
	str x9, [sp, #512]
	ldr x9, [sp, #504]
	str x9, [sp, #520]
	ldr x9, [sp, #520]
	lsl x9, x9, #2
	str x9, [sp, #528]
	sbfx x9, x9, #0, #32
	str x9, [sp, #536]
	ldr x9, [sp, #392]
	sbfx x9, x9, #0, #32
	str x9, [sp, #544]
	ldr x9, [sp, #536]
	sbfx x9, x9, #0, #32
	str x9, [sp, #552]
	ldr x9, [sp, #544]
	ldr x10, [sp, #552]
	add x9, x9, x10
	str x9, [sp, #560]
	sbfx x9, x9, #0, #32
	str x9, [sp, #568]
	movz x9, #8
	str x9, [sp, #576]
	ldr x9, [sp, #32]
	ldr x9, [x9]
	str x9, [sp, #584]
	ldr x9, [sp, #32]
	add x9, x9, #8
	ldr x9, [x9]
	str x9, [sp, #592]
	ldr x9, [sp, #40]
	ldr x9, [x9]
	str x9, [sp, #600]
	ldr x9, [sp, #40]
	add x9, x9, #8
	ldr x9, [x9]
	str x9, [sp, #608]
	ldr x9, [sp, #584]
	ldr x10, [sp, #592]
	str x10, [sp, #632]
	str x9, [sp, #624]
	ldr x9, [sp, #600]
	ldr x10, [sp, #608]
	str x10, [sp, #648]
	str x9, [sp, #640]
	ldr q0, [sp, #624]
	ldr q1, [sp, #640]
	bl "__letf2"
	str x0, [sp, #656]
	ldr x9, [sp, #656]
	sbfx x9, x9, #0, #32
	str x9, [sp, #664]
	movz x9, #1
	str x9, [sp, #672]
	ldr x9, [sp, #664]
	ldr x10, [sp, #672]
	cmp x9, x10
	cset x9, lt
	str x9, [sp, #680]
	ldr x9, [sp, #576]
	str x9, [sp, #688]
	ldr x9, [sp, #680]
	str x9, [sp, #696]
	ldr x9, [sp, #696]
	lsl x9, x9, #3
	str x9, [sp, #704]
	sbfx x9, x9, #0, #32
	str x9, [sp, #712]
	ldr x9, [sp, #568]
	sbfx x9, x9, #0, #32
	str x9, [sp, #720]
	ldr x9, [sp, #712]
	sbfx x9, x9, #0, #32
	str x9, [sp, #728]
	ldr x9, [sp, #720]
	ldr x10, [sp, #728]
	add x9, x9, x10
	str x9, [sp, #736]
	sbfx x9, x9, #0, #32
	str x9, [sp, #744]
	movz x9, #16
	str x9, [sp, #752]
	ldr x9, [sp, #32]
	ldr x9, [x9]
	str x9, [sp, #760]
	ldr x9, [sp, #32]
	add x9, x9, #8
	ldr x9, [x9]
	str x9, [sp, #768]
	ldr x9, [sp, #40]
	ldr x9, [x9]
	str x9, [sp, #776]
	ldr x9, [sp, #40]
	add x9, x9, #8
	ldr x9, [x9]
	str x9, [sp, #784]
	ldr x9, [sp, #760]
	ldr x10, [sp, #768]
	str x10, [sp, #808]
	str x9, [sp, #800]
	ldr x9, [sp, #776]
	ldr x10, [sp, #784]
	str x10, [sp, #824]
	str x9, [sp, #816]
	ldr q0, [sp, #800]
	ldr q1, [sp, #816]
	bl "__eqtf2"
	str x0, [sp, #832]
	ldr x9, [sp, #832]
	sbfx x9, x9, #0, #32
	str x9, [sp, #840]
	movz x9, #0
	str x9, [sp, #848]
	ldr x9, [sp, #840]
	ldr x10, [sp, #848]
	cmp x9, x10
	cset x9, eq
	str x9, [sp, #856]
	ldr x9, [sp, #752]
	str x9, [sp, #864]
	ldr x9, [sp, #856]
	str x9, [sp, #872]
	ldr x9, [sp, #872]
	lsl x9, x9, #4
	str x9, [sp, #880]
	sbfx x9, x9, #0, #32
	str x9, [sp, #888]
	ldr x9, [sp, #744]
	sbfx x9, x9, #0, #32
	str x9, [sp, #896]
	ldr x9, [sp, #888]
	sbfx x9, x9, #0, #32
	str x9, [sp, #904]
	ldr x9, [sp, #896]
	ldr x10, [sp, #904]
	add x9, x9, x10
	str x9, [sp, #912]
	sbfx x9, x9, #0, #32
	str x9, [sp, #920]
	movz x9, #32
	str x9, [sp, #928]
	ldr x9, [sp, #32]
	ldr x9, [x9]
	str x9, [sp, #936]
	ldr x9, [sp, #32]
	add x9, x9, #8
	ldr x9, [x9]
	str x9, [sp, #944]
	ldr x9, [sp, #40]
	ldr x9, [x9]
	str x9, [sp, #952]
	ldr x9, [sp, #40]
	add x9, x9, #8
	ldr x9, [x9]
	str x9, [sp, #960]
	ldr x9, [sp, #952]
	ldr x10, [sp, #960]
	str x10, [sp, #984]
	str x9, [sp, #976]
	ldr x9, [sp, #936]
	ldr x10, [sp, #944]
	str x10, [sp, #1000]
	str x9, [sp, #992]
	ldr q0, [sp, #976]
	ldr q1, [sp, #992]
	bl "__letf2"
	str x0, [sp, #1008]
	ldr x9, [sp, #1008]
	sbfx x9, x9, #0, #32
	str x9, [sp, #1016]
	movz x9, #1
	str x9, [sp, #1024]
	ldr x9, [sp, #1016]
	ldr x10, [sp, #1024]
	cmp x9, x10
	cset x9, lt
	str x9, [sp, #1032]
	ldr x9, [sp, #928]
	str x9, [sp, #1040]
	ldr x9, [sp, #1032]
	str x9, [sp, #1048]
	ldr x9, [sp, #1048]
	lsl x9, x9, #5
	str x9, [sp, #1056]
	sbfx x9, x9, #0, #32
	str x9, [sp, #1064]
	ldr x9, [sp, #920]
	sbfx x9, x9, #0, #32
	str x9, [sp, #1072]
	ldr x9, [sp, #1064]
	sbfx x9, x9, #0, #32
	str x9, [sp, #1080]
	ldr x9, [sp, #1072]
	ldr x10, [sp, #1080]
	add x9, x9, x10
	str x9, [sp, #1088]
	sbfx x9, x9, #0, #32
	str x9, [sp, #1096]
	movz x9, #64
	str x9, [sp, #1104]
	ldr x9, [sp, #32]
	ldr x9, [x9]
	str x9, [sp, #1112]
	ldr x9, [sp, #32]
	add x9, x9, #8
	ldr x9, [x9]
	str x9, [sp, #1120]
	ldr x9, [sp, #40]
	ldr x9, [x9]
	str x9, [sp, #1128]
	ldr x9, [sp, #40]
	add x9, x9, #8
	ldr x9, [x9]
	str x9, [sp, #1136]
	ldr x9, [sp, #1128]
	ldr x10, [sp, #1136]
	str x10, [sp, #1160]
	str x9, [sp, #1152]
	ldr x9, [sp, #1112]
	ldr x10, [sp, #1120]
	str x10, [sp, #1176]
	str x9, [sp, #1168]
	ldr q0, [sp, #1152]
	ldr q1, [sp, #1168]
	bl "__letf2"
	str x0, [sp, #1184]
	ldr x9, [sp, #1184]
	sbfx x9, x9, #0, #32
	str x9, [sp, #1192]
	movz x9, #0
	str x9, [sp, #1200]
	ldr x9, [sp, #1192]
	ldr x10, [sp, #1200]
	cmp x9, x10
	cset x9, lt
	str x9, [sp, #1208]
	ldr x9, [sp, #1104]
	str x9, [sp, #1216]
	ldr x9, [sp, #1208]
	str x9, [sp, #1224]
	ldr x9, [sp, #1224]
	lsl x9, x9, #6
	str x9, [sp, #1232]
	sbfx x9, x9, #0, #32
	str x9, [sp, #1240]
	ldr x9, [sp, #1096]
	sbfx x9, x9, #0, #32
	str x9, [sp, #1248]
	ldr x9, [sp, #1240]
	sbfx x9, x9, #0, #32
	str x9, [sp, #1256]
	ldr x9, [sp, #1248]
	ldr x10, [sp, #1256]
	add x9, x9, x10
	str x9, [sp, #1264]
	sbfx x9, x9, #0, #32
	str x9, [sp, #1272]
	sbfx x9, x9, #0, #32
	str x9, [sp, #1280]
	sbfx x9, x9, #0, #32
	str x9, [sp, #1288]
	mov x0, x9
	ldr x29, [sp, #1328]
	ldr x30, [sp, #1336]
	add sp, sp, #1344
	ret x30
	.size predicates, .-predicates
	.p2align 4
	.globl forward
	.type forward, %function
forward:
	sub sp, sp, #592
	str x29, [sp, #576]
	str x30, [sp, #584]
	add x29, sp, #576
	fmov x9, d0
	str x9, [sp, #48]
	fmov x9, d1
	str x9, [sp, #56]
	fmov x9, d2
	str x9, [sp, #64]
	fmov x9, d3
	str x9, [sp, #72]
	fmov x9, d4
	str x9, [sp, #80]
	fmov x9, d5
	str x9, [sp, #88]
	fmov x9, d6
	str x9, [sp, #96]
	fmov x9, d7
	str x9, [sp, #104]
	ldr x9, [sp, #592]
	str x9, [sp, #112]
	ldr x9, [sp, #608]
	str x9, [sp, #128]
	ldr x9, [sp, #616]
	str x9, [sp, #136]
	ldr x9, [sp, #624]
	str x9, [sp, #144]
	ldr x9, [sp, #632]
	str x9, [sp, #152]
.Larchon_7_0:
	add x9, sp, #464
	str x9, [sp, #160]
	add x9, sp, #472
	str x9, [sp, #168]
	add x9, sp, #480
	str x9, [sp, #176]
	add x9, sp, #488
	str x9, [sp, #184]
	add x9, sp, #496
	str x9, [sp, #192]
	add x9, sp, #504
	str x9, [sp, #200]
	add x9, sp, #512
	str x9, [sp, #208]
	add x9, sp, #520
	str x9, [sp, #216]
	add x9, sp, #528
	str x9, [sp, #224]
	add x9, sp, #544
	str x9, [sp, #232]
	add x9, sp, #560
	str x9, [sp, #240]
	ldr x9, [sp, #160]
	ldr x10, [sp, #48]
	str x10, [x9]
	ldr x9, [sp, #168]
	ldr x10, [sp, #56]
	str x10, [x9]
	ldr x9, [sp, #176]
	ldr x10, [sp, #64]
	str x10, [x9]
	ldr x9, [sp, #184]
	ldr x10, [sp, #72]
	str x10, [x9]
	ldr x9, [sp, #192]
	ldr x10, [sp, #80]
	str x10, [x9]
	ldr x9, [sp, #200]
	ldr x10, [sp, #88]
	str x10, [x9]
	ldr x9, [sp, #208]
	ldr x10, [sp, #96]
	str x10, [x9]
	ldr x9, [sp, #216]
	ldr x10, [sp, #104]
	str x10, [x9]
	ldr x9, [sp, #224]
	ldr x10, [sp, #112]
	str x10, [x9]
	ldr x9, [sp, #128]
	str x9, [sp, #248]
	ldr x9, [sp, #136]
	str x9, [sp, #256]
	ldr x9, [sp, #232]
	ldr x10, [sp, #248]
	str x10, [x9]
	ldr x9, [sp, #232]
	ldr x10, [sp, #256]
	add x9, x9, #8
	str x10, [x9]
	ldr x9, [sp, #144]
	str x9, [sp, #264]
	ldr x9, [sp, #152]
	str x9, [sp, #272]
	ldr x9, [sp, #240]
	ldr x10, [sp, #264]
	str x10, [x9]
	ldr x9, [sp, #240]
	ldr x10, [sp, #272]
	add x9, x9, #8
	str x10, [x9]
	ldr x9, [sp, #160]
	ldr x9, [x9]
	str x9, [sp, #280]
	ldr x9, [sp, #168]
	ldr x9, [x9]
	str x9, [sp, #288]
	ldr x9, [sp, #176]
	ldr x9, [x9]
	str x9, [sp, #296]
	ldr x9, [sp, #184]
	ldr x9, [x9]
	str x9, [sp, #304]
	ldr x9, [sp, #192]
	ldr x9, [x9]
	str x9, [sp, #312]
	ldr x9, [sp, #200]
	ldr x9, [x9]
	str x9, [sp, #320]
	ldr x9, [sp, #208]
	ldr x9, [x9]
	str x9, [sp, #328]
	ldr x9, [sp, #216]
	ldr x9, [x9]
	str x9, [sp, #336]
	ldr x9, [sp, #224]
	ldr x9, [x9]
	str x9, [sp, #344]
	ldr x9, [sp, #232]
	ldr x9, [x9]
	str x9, [sp, #352]
	ldr x9, [sp, #232]
	add x9, x9, #8
	ldr x9, [x9]
	str x9, [sp, #360]
	ldr x9, [sp, #352]
	ldr x10, [sp, #360]
	str x10, [sp, #376]
	str x9, [sp, #368]
	ldr x9, [sp, #240]
	ldr x9, [x9]
	str x9, [sp, #384]
	ldr x9, [sp, #240]
	add x9, x9, #8
	ldr x9, [x9]
	str x9, [sp, #392]
	ldr x9, [sp, #384]
	ldr x10, [sp, #392]
	str x10, [sp, #408]
	str x9, [sp, #400]
	ldr d0, [sp, #280]
	ldr d1, [sp, #288]
	ldr d2, [sp, #296]
	ldr d3, [sp, #304]
	ldr d4, [sp, #312]
	ldr d5, [sp, #320]
	ldr d6, [sp, #328]
	ldr d7, [sp, #336]
	ldr x9, [sp, #344]
	str x9, [sp, #0]
	ldr x9, [sp, #368]
	str x9, [sp, #16]
	ldr x9, [sp, #376]
	str x9, [sp, #24]
	ldr x9, [sp, #400]
	str x9, [sp, #32]
	ldr x9, [sp, #408]
	str x9, [sp, #40]
	bl "external_many"
	str q0, [sp, #416]
	ldr x9, [sp, #416]
	str x9, [sp, #432]
	ldr x9, [sp, #424]
	str x9, [sp, #440]
	ldr x9, [sp, #432]
	ldr x10, [sp, #440]
	str x10, [sp, #456]
	str x9, [sp, #448]
	ldr q0, [sp, #448]
	ldr x29, [sp, #576]
	ldr x30, [sp, #584]
	add sp, sp, #592
	ret x30
	.size forward, .-forward
	.section .note.GNU-stack,"",%progbits
	.section .data
	.balign 16
	.globl table
	.type table, %object
table:
	.byte 0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xfe,0x7f
	.byte 0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00
	.zero 15
	.byte 0x80,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xff
	.byte 0x3f,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x8f
	.byte 0x3f,0x55,0x55,0x55,0x55,0x55,0x55,0x55,0x55,0x55,0x55,0x55,0x55,0x55,0x55,0xfd
	.byte 0x3f
	.size table, .-table
	.section .rodata
	.balign 1
	.local __cephyr_tu_string_0
	.type __cephyr_tu_string_0, %object
__cephyr_tu_string_0:
	.byte 0x30,0x78,0x31,0x2e,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30
	.byte 0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x31
	.byte 0x70,0x30,0x21,0x00
	.size __cephyr_tu_string_0, .-__cephyr_tu_string_0
	.section .rodata
	.balign 4
	.local __cephyr_tu_string_1
	.type __cephyr_tu_string_1, %object
__cephyr_tu_string_1:
	.byte 0x30,0x00,0x00,0x00,0x78,0x00,0x00,0x00,0x31,0x00,0x00,0x00,0x70,0x00,0x00,0x00
	.byte 0x2d,0x00,0x00,0x00,0x31,0x00,0x00,0x00,0x36,0x00,0x00,0x00,0x34,0x00,0x00,0x00
	.byte 0x39,0x00,0x00,0x00,0x34,0x00,0x00,0x00,0x3f,0x00,0x00,0x00,0x00,0x00,0x00,0x00
	.size __cephyr_tu_string_1, .-__cephyr_tu_string_1
	.set __cephyr_tu_vararg_0, external_var
