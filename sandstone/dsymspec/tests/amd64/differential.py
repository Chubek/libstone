#!/usr/bin/env python3
"""Compare the AMD64 generated C encoder with LLVM MC 22 and ELF readobj."""
import argparse
import json
import re
import subprocess
from pathlib import Path


def run(args, source=None):
    return subprocess.run(args, input=source, text=True, check=True, capture_output=True).stdout


def vector(name, source, **bindings):
    return (name, source, bindings)


def mem(n, modrm, sib=-1, *, b=0, x=0, ds=0, disp=0, fix=0, ref=None):
    fields = dict(modrm=modrm, sib=sib, b=b, x=x, ds=ds, disp=disp, fix=fix)
    if ref is not None:
        fields["ref"] = "@" + ref
    return {f"{key}{n}": value for key, value in fields.items()}


def line(case):
    name, _, bindings = case
    return name + "".join(f" {key}={value}" for key, value in bindings.items()) + "\n"


def check_integer_matrix():
    for name in ("add", "or", "adc", "sbb", "and", "sub", "xor", "cmp"):
        for width, dst, src, ptr in (
            (8, "r9b", "r14b", "byte"), (16, "r9w", "r14w", "word"),
            (32, "r9d", "r14d", "dword"), (64, "r9", "r14", "qword"),
        ):
            reg, memory = f"r{width}", f"m{width}"
            textmem = f"{ptr} ptr [r12+127]"
            yield vector(f"x64_{name}_{reg}_{reg}", f"{name} {dst}, {src}", r0=9, r1=14)
            yield vector(f"x64_{name}_{reg}_{memory}", f"{name} {dst}, {textmem}", r0=9,
                         **mem(1, 68, 36, b=1, ds=1, disp=127))
            yield vector(f"x64_{name}_{memory}_{reg}", f"{name} {textmem}, {src}", r1=14,
                         **mem(0, 68, 36, b=1, ds=1, disp=127))
            yield vector(f"x64_{name}_{memory}_i8", f"{name} {textmem}, -128", imm=-128,
                         **mem(0, 68, 36, b=1, ds=1, disp=127))
            if width > 8:
                immwidth = min(width, 32)
                yield vector(f"x64_{name}_{memory}_i{immwidth}", f"{name} {textmem}, 128", imm=128,
                             **mem(0, 68, 36, b=1, ds=1, disp=127))


def check_fma_matrix():
    for instr in ("vfmadd", "vfmsub", "vfnmadd", "vfnmsub", "vfmaddsub", "vfmsubadd"):
        for order in (132, 213, 231):
            for kind in ("ps", "pd", "ss", "sd"):
                if instr in ("vfmaddsub", "vfmsubadd") and kind in ("ss", "sd"):
                    continue
                for width in ((128, 256) if kind in ("ps", "pd") else (128,)):
                    name = f"{instr}{order}{kind}"
                    p = "y" if width == 256 else "x"
                    yield vector(f"x64_{name}_v{width}_v{width}_v{width}",
                                 f"{name} {p}mm13, {p}mm9, {p}mm12", r0=13, r1=9, r2=12)


vectors = [
    vector("x64_add_r8_r8", "add al, cl", r0=0, r1=1),
    vector("x64_mov_r8_r8", "mov ah, bl", r0=16, r1=3),
    vector("x64_mov_r8_r8", "mov spl, bl", r0=4, r1=3),
    vector("x64_mov_r8_r8", "mov r9b, spl", r0=9, r1=4),
    vector("x64_mov_r8_i8_opcode", "mov bh, 0xaa", r0=19, imm=170),
    vector("x64_mov_r16_i16_opcode", "mov r12w, 0xabcd", r0=12, imm=0xabcd),
    vector("x64_mov_r32_i32_opcode", "mov r12d, 0x89abcdef", r0=12, imm=0x89abcdef),
    vector("x64_movabs_r64_i64_opcode", "movabs r15, 0x8000000000000000", r0=15, imm=-(1 << 63)),
    vector("x64_mov_r64_i32", "mov r9, -2147483648", r0=9, imm=-(1 << 31)),
    vector("x64_mov_r64_m64", "mov r11, [r13]", r0=11, **mem(1, 69, b=1, ds=1)),
    vector("x64_mov_r32_m32", "mov r11d, [r13d]", r0=11, **mem(1, 325, b=1, ds=1)),
    vector("x64_mov_r32_m32", "mov r11d, fs:[r12d+r9d*8-128]", r0=11,
           **mem(1, 832 + 4, 204, b=1, x=1, ds=1, disp=-128)),
    vector("x64_mov_r32_m32", "mov r11d, gs:[r13]", r0=11, **mem(1, 1024 + 69, b=1, ds=1)),
    vector("x64_mov_r64_m64", "mov r11, [r12*4+0x12345678]", r0=11,
           **mem(1, 4, 165, x=1, ds=4, disp=0x12345678)),
    vector("x64_mov_r8_m8", "mov ah, [rbp+4]", r0=16, **mem(1, 69, ds=1, disp=4)),
    vector("x64_or_m16_i8_lock", "lock or word ptr [r13], -128", imm=-128, **mem(0, 69, b=1, ds=1)),
    vector("x64_sbb_m64_i32", "sbb qword ptr [r12], 128", imm=128, **mem(0, 4, 36, b=1)),
    vector("x64_cmp_r32_m32", "cmp r15d, [r12+128]", r0=15, **mem(1, 132, 36, b=1, ds=4, disp=128)),
    vector("x64_test_r16_i16_acc", "test ax, 0x8042", r0=0, imm=0x8042),
    vector("x64_movsx_r64_r8", "movsx r9, spl", r0=9, r1=4),
    vector("x64_movzx_r16_r8", "movzx ax, ah", r0=0, r1=16),
    vector("x64_movsxd_r64_m32", "movsxd r9, [r12]", r0=9, **mem(1, 4, 36, b=1)),
    vector("x64_lea_r16_m16", "lea r9w, [r10+r11*2+5]", r0=9, **mem(1, 68, 90, b=1, x=1, ds=1, disp=5)),
    vector("x64_imul_r64_r64_i8", "imul r11, r12, -128", r0=11, r1=12, imm=-128),
    vector("x64_imul_r16_m16_i16", "imul r8w, [r13], 0x1234", r0=8, imm=0x1234,
           **mem(1, 69, b=1, ds=1)),
    vector("x64_div_r8", "div bh", r0=19),
    vector("x64_rcr_r64_r8", "rcr r14, cl", r0=14, r1=1),
    vector("x64_rol_m32", "rol dword ptr [rbp], 1", **mem(0, 69, ds=1)),
    vector("x64_shrd_m16_r16_i8", "shrd [r12], r9w, 15", r1=9, imm=15, **mem(0, 4, 36, b=1)),
    vector("x64_btr_m64_r64_lock", "lock btr [r12], r9", r1=9, **mem(0, 4, 36, b=1)),
    vector("x64_bsf_r32_r32", "bsf r9d, r15d", r0=9, r1=15),
    vector("x64_bswap_r64", "bswap r14", r0=14),
    vector("x64_xchg_r64_r64_acc1", "xchg r12, rax", r0=12, r1=0),
    vector("x64_cmovbe_r64_m64", "cmovbe r9, [r13]", r0=9, **mem(1, 69, b=1, ds=1)),
    vector("x64_setnp_r8", "setnp spl", r0=4),
    vector("x64_cmpxchg16b_m128_lock", "lock cmpxchg16b [r12]", **mem(0, 4, 36, b=1)),
    vector("x64_pop_r16", "pop r13w", r0=13),
    vector("x64_push_m64", "push qword ptr [r13]", **mem(0, 69, b=1, ds=1)),
    vector("x64_push_i8_64", "push -128", imm=-128),
    vector("x64_ret_i16", "ret 0xbeef", imm=0xbeef),
    vector("x64_retf_fixed_64", "retfq"),
    vector("x64_enter_i16_i8", "enter 0x1234, 11", imm=0x1234, imm2=11),
    vector("x64_call_r64", "call r15", r0=15),
    vector("x64_mov_r64_s16", "mov r9, fs", r0=9, r1=4),
    vector("x64_mov_s16_m16", "mov gs, word ptr [r12]", r0=5, **mem(1, 4, 36, b=1)),
    vector("x64_scasq_fixed_a64repne", "repne scasq"),
    vector("x64_out_i8_r32", "out 255, eax", r1=0, imm=255),
    vector("x64_in_r16_r16", "in ax, dx", r0=0, r1=2),
    vector("x64_mov_c64_r64", "mov cr8, r14", r0=8, r1=14),
    vector("x64_invlpg_m8", "invlpg [r10]", **mem(0, 2, b=1)),
    vector("x64_sysretq_fixed", "sysretq"),
    vector("x64_vmread_r64_r64", "vmread r8, r9", r0=8, r1=9),
    vector("x64_vmsave_fixed", "vmsave"),
    vector("x64_tzcnt_r16_r16", "tzcnt r9w, r15w", r0=9, r1=15),
    vector("x64_popcnt_r64_m64", "popcnt r11, [r13]", r0=11, **mem(1, 69, b=1, ds=1)),
    vector("x64_lzcnt_r32_r32", "lzcnt r10d, r9d", r0=10, r1=9),
    vector("x64_blsmsk_r64_m64", "blsmsk r10, [r13]", r0=10, **mem(1, 69, b=1, ds=1)),
    vector("x64_bextr_r32_m32_r32", "bextr r9d, [r13], r12d", r0=9, r2=12, **mem(1, 69, b=1, ds=1)),
    vector("x64_andn_r32_r32_r32", "andn r9d, r10d, r15d", r0=9, r1=10, r2=15),
    vector("x64_pdep_r64_r64_r64", "pdep r9, r10, r11", r0=9, r1=10, r2=11),
    vector("x64_bzhi_r64_r64_r64", "bzhi r9, r10, r11", r0=9, r1=10, r2=11),
    vector("x64_mulx_r64_r64_m64", "mulx r14, r9, [r13]", r0=14, r1=9, **mem(2, 69, b=1, ds=1)),
    vector("x64_rorx_r32_r32_i8", "rorx r15d, r10d, 255", r0=15, r1=10, imm=255),
    vector("x64_adcx_r32_r32", "adcx r15d, r10d", r0=15, r1=10),
    vector("x64_adox_r64_m64", "adox r15, [r14]", r0=15, **mem(1, 6, b=1)),
    vector("x64_rdseed_r16", "rdseed r13w", r0=13),
    vector("x64_rdrand_r32", "rdrand r13d", r0=13),
    vector("x64_rdpid_r64", "rdpid r12", r0=12),
    vector("x64_movbe_m16_r16", "movbe [r12], r10w", r1=10, **mem(0, 4, 36, b=1)),
    vector("x64_wrgsbase_r64", "wrgsbase r11", r0=11),
    vector("x64_clwb_m8", "clwb [r14]", **mem(0, 6, b=1)),
    vector("x64_clflushopt_m8", "clflushopt [r14]", **mem(0, 6, b=1)),
    vector("x64_cldemote_m8", "cldemote [r14]", **mem(0, 6, b=1)),
    vector("x64_movdiri_m64_r64", "movdiri [r12], r15", r1=15, **mem(0, 4, 36, b=1)),
    vector("x64_clzero_fixed", "clzero"),
    vector("x64_umonitor_r32", "umonitor r14d", r0=14),
    vector("x64_tpause_r32", "tpause r9d", r0=9),
    vector("x64_endbr64_fixed", "endbr64"),
    vector("x64_wrussq_m64_r64", "wrussq [r12], r13", r1=13, **mem(0, 4, 36, b=1)),
    vector("x64_xabort_i8", "xabort 0xe7", imm=231),
    vector("x64_enclu_fixed", "enclu"),
    vector("x64_serialize_fixed", "serialize"),
    vector("x64_senduipi_r64", "senduipi r14", r0=14),
    vector("x64_addss_v128_m32", "addss xmm15, [r13]", r0=15, **mem(1, 69, b=1, ds=1)),
    vector("x64_movlps_v128_m64", "movlps xmm13,[r14]", r0=13, **mem(1, 6, b=1)),
    vector("x64_movapd_m128_v128", "movapd [r14], xmm12", r1=12, **mem(0, 6, b=1)),
    vector("x64_cvtss2si_r64_m32", "cvtss2si r15, [r13]", r0=15, **mem(1, 69, b=1, ds=1)),
    vector("x64_movq2dq_v128_q64", "movq2dq xmm11, mm7", r0=11, r1=7),
    vector("x64_punpcklbw_q64_m32", "punpcklbw mm1, [r12]", r0=1, **mem(1, 4, 36, b=1)),
    vector("x64_pshufw_q64_q64_i8", "pshufw mm7, mm6, 255", r0=7, r1=6, imm=255),
    vector("x64_haddpd_v128_v128", "haddpd xmm11, xmm13", r0=11, r1=13),
    vector("x64_pabsd_q64_q64", "pabsd mm7, mm1", r0=7, r1=1),
    vector("x64_pmovzxbq_v128_m16", "pmovzxbq xmm12, word ptr [r10]", r0=12, **mem(1, 2, b=1)),
    vector("x64_pextrq_m64_v128_i8", "pextrq [r14], xmm15, 1", r1=15, imm=1, **mem(0, 6, b=1)),
    vector("x64_crc32_r64_r8", "crc32 r10, spl", r0=10, r1=4),
    vector("x64_crc32_r32_r16", "crc32 r10d, r11w", r0=10, r1=11),
    vector("x64_pcmpistri_v128_v128_i8", "pcmpistri xmm13, xmm14, 0xdf", r0=13, r1=14, imm=223),
    vector("x64_vaddps_v256_v256_v256", "vaddps ymm11, ymm9, ymm3", r0=11, r1=9, r2=3),
    vector("x64_vaddps_v128_v128_v128", "vaddps xmm11, xmm9, xmm15", r0=11, r1=9, r2=15),
    vector("x64_vmovsd_v128_v128_v128", "vmovsd xmm8, xmm10, xmm13", r0=8, r1=10, r2=13),
    vector("x64_vpsrlw_v256_v256_v128", "vpsrlw ymm14, ymm10, xmm15", r0=14, r1=10, r2=15),
    vector("x64_vpsrlw_v256_v256_i8", "vpsrlw ymm14, ymm10, 7", r0=14, r1=10, imm=7),
    vector("x64_vpsllvq_v256_v256_v256", "vpsllvq ymm14, ymm10, ymm15", r0=14, r1=10, r2=15),
    vector("x64_vextractf128_m128_v256_i8", "vextractf128 [r13], ymm15, 1", r1=15, imm=1,
           **mem(0, 69, b=1, ds=1)),
    vector("x64_vfnmadd231sd_v128_v128_m64", "vfnmadd231sd xmm13, xmm15, [r10]", r0=13, r1=15,
           **mem(2, 2, b=1)),
    vector("x64_vfmaddsub213pd_v256_v256_v256", "vfmaddsub213pd ymm13, ymm15, ymm10", r0=13, r1=15, r2=10),
    vector("x64_vaddps_v512_v512_v512_evex", "vaddps zmm17, zmm18, zmm19", r0=17, r1=18, r2=19, mask=0),
    vector("x64_vaddps_v128_v128_v128_evex", "{evex} vaddps xmm8, xmm2, xmm3", r0=8, r1=2, r2=3, mask=0),
    vector("x64_vaddps_v128_v128_v128_evex", "vaddps xmm16, xmm2, xmm3", r0=16, r1=2, r2=3, mask=0),
    vector("x64_vaddps_v512_v512_m32_evex_bcst", "vaddps zmm7 {k3}{z}, zmm22, [r12+16]{1to16}",
           r0=7, r1=22, mask=11, **mem(2, 68, 36, b=1, ds=1, disp=16)),
    vector("x64_vaddps_v512_v512_m512_evex", "vaddps zmm23 {k7}, zmm14, [r13+256]",
           r0=23, r1=14, mask=7, **mem(2, 69, b=1, ds=1, disp=256)),
    vector("x64_vaddps_v512_v512_m512_evex", "vaddps zmm23 {k7}, zmm14, [r13+257]",
           r0=23, r1=14, mask=7, **mem(2, 133, b=1, ds=4, disp=257)),
    vector("x64_vmovaps_m512_v512_evex", "vmovaps [r12+64] {k1}, zmm28",
           r1=28, mask=1, **mem(0, 68, 36, b=1, ds=1, disp=64)),
    vector("x64_kmovd_r32_k64", "kmovd r10d,k3", r0=10, r1=3),
    vector("x64_kmovq_m64_k64", "kmovq [r10],k7", r1=7, **mem(0, 2, b=1)),
    vector("x64_kandb_k64_k64_k64", "kandb k3,k6,k7", r0=3, r1=6, r2=7),
    vector("x64_kshiftlq_k64_k64_i8", "kshiftlq k3,k7,255", r0=3, r1=7, imm=255),
    vector("x64_aeskeygenassist_v128_v128_i8", "aeskeygenassist xmm15, xmm9, 255", r0=15, r1=9, imm=255),
    vector("x64_vaesenc_v256_v256_v256", "vaesenc ymm8, ymm9, ymm10", r0=8, r1=9, r2=10),
    vector("x64_vaesenc_v512_v512_v512_evex", "vaesenc zmm28, zmm21, zmm22", r0=28, r1=21, r2=22),
    vector("x64_vpclmulqdq_v128_v128_v128_i8", "vpclmulqdq xmm9, xmm10, xmm11, 1", r0=9, r1=10, r2=11, imm=1),
    vector("x64_sha256rnds2_v128_v128_v128", "sha256rnds2 xmm15, xmm8, xmm0", r0=15, r1=8, r2=0),
    vector("x64_vgf2p8mulb_v256_v256_v256", "vgf2p8mulb ymm0, ymm1, ymm2", r0=0, r1=1, r2=2),
    vector("x64_vgf2p8affineqb_v512_v512_v512_i8_evex", "vgf2p8affineqb zmm0, zmm1, zmm2, 7",
           r0=0, r1=1, r2=2, imm=7, mask=0),
    vector("x64_fld_m80", "fld tbyte ptr [r12]", **mem(0, 4, 36, b=1)),
    vector("x64_fmul_m64", "fmul qword ptr [r9]", **mem(0, 1, b=1)),
    vector("x64_fsub_t80_t80_reverse", "fsub st(7), st(0)", r0=7, r1=0),
    vector("x64_fsubp_t80_t80", "fsubp st(7), st(0)", r0=7, r1=0),
    vector("x64_fild_m64", "fild qword ptr [r14]", **mem(0, 6, b=1)),
    vector("x64_fisttp_m64", "fisttp qword ptr [r14]", **mem(0, 6, b=1)),
    vector("x64_fstsw_r16", "fstsw ax", r0=0),
]
vectors.extend(check_integer_matrix())
vectors.extend(check_fma_matrix())

rejected = [
    vector("x64_mov_r8_r8", None, r0=16, r1=4),  # AH requires no REX; SPL needs one.
    vector("x64_mov_r8_r8", None, r0=16, r1=11),
    vector("x64_mov_r8_m8", None, r0=16, **mem(1, 69, b=1, ds=1)),
    vector("x64_mov_r8_m8", None, r0=16, **mem(1, 4, 36, x=1)),
    vector("x64_mov_r64_i32", None, r0=0, imm=1 << 31),
    vector("x64_shrd_m16_r16_i8", None, r1=9, imm=256, **mem(0, 4, 36, b=1)),
    vector("x64_shrd_m16_r16_r8", None, r1=9, r2=4, **mem(0, 4, 36, b=1)),
    vector("x64_mov_r16_m16", None, r0=0, **mem(1, 4)),  # Missing SIB.
    vector("x64_mov_r16_m16", None, r0=0, **mem(1, 64)),  # Missing disp8.
    vector("x64_mov_r16_m16", None, r0=0, **mem(1, 64, ds=1, disp=-129)),
    vector("x64_mov_r16_m16", None, r0=0, **mem(1, 8)),  # Caller prefilled ModR/M.reg bits.
    vector("x64_mov_r64_m64", None, r0=0, **mem(1, 128, ds=4, fix=2, ref="external")),
    vector("x64_mov_s16_r16", None, r0=1, r1=9),  # Cannot MOV into CS.
    vector("x64_mov_c64_r64", None, r0=1, r1=9),  # Reserved CR1.
    vector("x64_push_s16_64", None, r0=3),  # No PUSH DS in long mode.
    vector("x64_jrcxz_i8_rel", None, imm=128),
    vector("x64_andn_r64_r64_r64", None, r0=16, r1=8, r2=5),
    vector("x64_vaddps_v128_v128_v128", None, r0=16, r1=8, r2=5),
    vector("x64_vaddps_v512_v512_v512_evex", None, r0=32, r1=8, r2=5, mask=0),
    vector("x64_vaddps_v512_v512_v512_evex", None, r0=9, r1=8, r2=5, mask=8),  # Zeroing without a mask.
    vector("x64_vmovaps_m512_v512_evex", None, r1=25, mask=9, **mem(0, 6, b=1)),  # No zeroing stores.
    vector("x64_vaddps_v512_v512_m32_evex_bcst", None, r0=17, r1=18, mask=0,
           **mem(2, 69, b=1, ds=1, disp=5)),
    vector("x64_vaddps_v512_v512_m32_evex_bcst", None, r0=17, r1=18, mask=0,
           **mem(2, 69, b=1, ds=1, disp=512)),
    vector("x64_sha256rnds2_v128_v128_v128", None, r0=15, r1=8, r2=1),
    vector("x64_fsubp_t80_t80", None, r0=7, r1=1),
]

relocations = [
    vector("x64_call_a64_rel32", "call external", sym="@external", addend=0),
    vector("x64_call_a64_rel32", "call external+4", sym="@external", addend=4),
    vector("x64_jne_a64_rel32", "jne external", sym="@external", addend=0),
    vector("x64_loop_a64_rel8", "loop external", sym="@external", addend=0),
    vector("x64_add_m64_i8", "addq $7, external+12(%rip)", imm=7,
           **mem(0, 5, ds=4, disp=12, fix=1, ref="external")),
    vector("x64_mov_r64_m64", "movq external@GOTPCREL+8(%rip), %r9", r0=9,
           **mem(1, 5, ds=4, disp=8, fix=2, ref="external")),
    vector("x64_mov_r64_m64", "movq %fs:external@TPOFF(%r12), %r10", r0=10,
           **mem(1, 512 + 132, 36, b=1, ds=4, fix=4, ref="external")),
    vector("x64_lea_r64_m64", "leaq external@TLSGD(%rip), %rdi", r0=7,
           **mem(1, 5, ds=4, fix=6, ref="external")),
    vector("x64_lea_r64_m64", "leaq external@TLSLD(%rip), %rdi", r0=7,
           **mem(1, 5, ds=4, fix=7, ref="external")),
    vector("x64_mov_r64_a64", "movq $external+7, %r9", r0=9, sym="@external", addend=7),
    vector("x64_movabs_r64_a64", "movabsq $external-9, %r9", r0=9, sym="@external", addend=-9),
]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("runner", type=Path, help="compiled encode.c executable")
    parser.add_argument("--work", type=Path, required=True, help="scratch directory, e.g. build/amd64")
    parser.add_argument("--llvm-mc", default="llvm-mc")
    parser.add_argument("--llvm-readobj", default="llvm-readobj")
    parser.add_argument("--llvm-objcopy", default="llvm-objcopy")
    opts = parser.parse_args()
    opts.work.mkdir(parents=True, exist_ok=True)
    mc = [opts.llvm_mc, "-triple=x86_64"]
    observed = run([str(opts.runner.resolve())], "".join(map(line, vectors + rejected + relocations))).splitlines()
    if len(observed) != len(vectors + rejected + relocations):
        raise SystemExit("bad number of encoder results")
    listing = run(mc + ["-x86-asm-syntax=intel", "-show-encoding"],
                  "".join(f"fixture_{i}:\n{case[1]}\n" for i, case in enumerate(vectors)))
    blocks = re.split(r"^fixture_\d+:", listing, flags=re.M)[1:]
    expected = ["".join(re.findall(r"0x([0-9a-f]{2})", "".join(re.findall(r"encoding: \[(.+?)\]", block))))
                for block in blocks]
    if len(expected) != len(vectors) or not all(expected):
        raise SystemExit("bad number of reference bytes or blocks")
    expected.extend("rejected" for _ in rejected)

    obj, raw = opts.work / "amd64-fixups.o", opts.work / "amd64-fixups.bin"
    run(mc + ["-filetype=obj", "-o", obj], "\n".join(case[1] for case in relocations) + "\n")
    run([opts.llvm_objcopy, "--only-section=.text", "-O", "binary", obj, raw])
    result = json.loads(run([opts.llvm_readobj, "--elf-output-style=JSON", "--relocations", "--expand-relocs", obj]))
    elf_relocs = [r["Relocation"] for s in result[0]["Relocations"] for r in s["Relocs"]]
    if len(elf_relocs) != len(relocations):
        raise SystemExit("bad number of reference ELF relocation records")
    content, place = raw.read_bytes(), 0
    for i, ref in enumerate(elf_relocs):
        offset, kind, sym, addend = ref["Offset"], ref["Type"]["Name"], ref["Symbol"]["Name"], ref["Addend"]
        if addend >= 1 << 63:
            addend -= 1 << 64
        # Width of the relocated field and any non-relocated suffix.
        tail = {"R_X86_64_PC8": 1, "R_X86_64_64": 8}.get(kind, 4)
        if relocations[i][1].startswith("addq"):
            tail += 1
        expected.append(content[place:offset + tail].hex() + f" {offset - place}:{kind}:{sym}:{addend}")
        place = offset + tail

    failures = 0
    for (name, asm, _), actual, want in zip(vectors + rejected + relocations, observed, expected, strict=True):
        if actual != want:
            print(f"{name} ({asm}): generated {actual}, expected {want}")
            failures += 1
    print(f"{len(vectors)} byte comparisons, {len(rejected)} rejections, {len(relocations)} ELF fixups")
    return bool(failures)


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except subprocess.CalledProcessError as exc:
        raise SystemExit(exc.stderr) from exc
