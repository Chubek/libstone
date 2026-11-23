"""Inspect real ELF64 RELAs and bank output; optionally compile binary128 GAS with Cephyr."""
import argparse
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

from test_pipeline import assemble, build_bank, find_binary


def elf(path):
    data = path.read_bytes()
    assert data[:6] == b'\x7fELF\x02\x01'
    assert struct.unpack_from('<HH', data, 16) == (1, 243)
    base, = struct.unpack_from('<Q', data, 40)
    stride, count, names = struct.unpack_from('<HHH', data, 58)
    sh = [struct.unpack_from('<IIQQQQIIQQ', data, base + i*stride) for i in range(count)]
    def contents(h):
        return data[h[4]:h[4]+h[5]]
    def string(h, offset):
        return contents(h)[offset:].split(b'\0', 1)[0].decode()
    byname = {string(sh[names], h[0]): h for h in sh}
    tab = byname['.symtab']
    syms = [(string(sh[tab[6]], off), sec, val)
            for off, _, _, sec, val, _ in struct.iter_unpack('<IBBHQQ', contents(tab))]
    relocs = [(off, info & 0xffffffff, syms[info >> 32], add)
              for off, info, add in struct.iter_unpack('<QQq', contents(byname['.rela.text']))]
    return contents(byname['.text']), relocs, struct.unpack_from('<I', data, 48)[0]


def main():
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument('--build-dir', required=True)
    cli.add_argument('--cephyr', type=Path)
    cli.add_argument('--runtime', type=Path, help='also run mixed GCC/Cephyr binary128 test (crt0.o and libc.a)')
    cli.add_argument('--cross-prefix', default='riscv64-linux-gnu-')
    cli.add_argument('--qemu', default='qemu-riscv64')
    cli.add_argument('--qobjld', help='also execute static link with this qobjld')
    args = cli.parse_args()
    if args.runtime and not args.cephyr:
        cli.error('--runtime requires --cephyr')
    root = Path(__file__).resolve().parents[1]
    cc = find_binary(args.build_dir, 'sandstone-spec-compiler')
    assembler = find_binary(args.build_dir, 'sandstone-as')
    with tempfile.TemporaryDirectory() as tmp:
        d = Path(tmp)
        for spec in ('riscv64', 'rv64im'):
            bank = str(d / (spec + '.symas'))
            build_bank(str(root), cc, str(root / f'compiler/stdspec/{spec}.dsymspec'), bank)
            obj = d / 'fixture.o'
            assemble(assembler, bank, """
.option push
.option norvc
.option norelax
sd t6,-8(s11)
lwu a0,12(s2)
call outside
lla t0, object
.option pic
la a0, object
1:
auipc t3, %pcrel_hi(1f)
jalr zero, %pcrel_lo(1b)(t3)
1:
bne a0,zero,1b
.option pop
""", str(obj), fmt='elf')
            text, reloc, flags = elf(obj)
            words = struct.unpack('<11I', text)
            assert words == (0xfffdbc23, 0x00c96503, 0x97, 0x80e7, 0x297, 0x28293,
                             0x517, 0x53503, 0xe17, 0xe0067, 0x51063), words
            assert [(r[0], r[1], r[3]) for r in reloc] == [
                (8,19,0), (16,23,0), (20,24,0), (24,20,0), (28,24,0),
                (32,23,0), (36,24,0), (40,16,0)]
            assert [reloc[i][2][0] for i in (0,1,3)] == ['outside','object','object']
            assert [reloc[i][2][2] for i in (2,4,5,6,7)] == [16,24,40,32,40]
            assert flags == (0 if spec == 'rv64im' else 5)
            assemble(assembler, bank, """
1: auipc a0,%got_pcrel_hi(outside)
ld a0,%pcrel_lo(1b)(a0)
2: auipc t0,%pcrel_hi(outside)
addi t0,t0,%pcrel_lo(2b)
""", str(obj), fmt='elf')
            assert [r[1] for r in elf(obj)[1]] == [20,24,23,24]
            for source in ('sd a0,2048(sp)', 'lw f0,0(sp)', 'slli a0,a0,64',
                           '.option arch, rv64i\nmul a0,a0,a1', '.option norvc\nc.nop',
                           '.option pop', 'bnez a0,1f', 'li a0,0x10000000000000000',
                           *(['c.nop', 'fadd.d f0,f0,f1'] if spec=='rv64im' else [])):
                invalid = d/'invalid.s'
                invalid.write_text(source)
                res = subprocess.run([assembler,'--bank',bank,'-f','elf','-o',obj,invalid],
                                     capture_output=True)
                assert res.returncode != 0, source
            # Evaluate the bank-encoded ADDI/SLLI sequence independently.
            for value in (0, -2049, 2048, -2**63, 2**63-1, 0x0123456789abcdef, 2**64-1):
                binary = d/'li.bin'
                assemble(assembler, bank, f'li a0,{value}', str(binary))
                acc = None
                for w, in struct.iter_unpack('<I', binary.read_bytes()):
                    assert (w & 0xfff) == 0x513
                    src = (w >> 15) & 31
                    assert src in (0,10)
                    x = 0 if src == 0 else acc
                    if (w >> 12) & 7 == 1:
                        acc = x << (w >> 20)
                    else:
                        assert (w >> 12) & 7 == 0
                        acc = x + ((w >> 20) - ((w >> 31) << 12))
                    acc &= 2**64-1
                assert acc == value & (2**64-1)

        if args.cephyr:
            sys.path.insert(0,str(root.parent/'cornerstone/Cephyr/tests'))
            from test_cephyr_c11_rv64_long_double import NATIVE, NATIVE_RV, EXTERNAL, EXTERNAL_RV
            src = d/'quad.c'
            src.write_text(NATIVE + NATIVE_RV)
            for pic in ('-fPIC','-fno-pic'):
                gas = d/'quad.s'
                subprocess.run([args.cephyr.resolve(), '--target=riscv64-linux-gnu', pic,
                                '--no-factir', '-S', src, '-o', gas], check=True)
                assemble(assembler, bank, gas.read_text(), str(obj), fmt='elf')
                _, relocs, flags = elf(obj)
                assert flags==0 and any(r[1]==19 and r[2][0]=='__addtf3' for r in relocs)
                assert any(r[1]==20 for r in relocs) == (pic=='-fPIC')
                assert {16,19,23,24} <= {r[1] for r in relocs}
                if args.runtime:
                    run = lambda *cmd: subprocess.run(cmd, check=True, timeout=60)
                    abi = ['-march=rv64im', '-mabi=lp64', '-mno-relax']
                    reference, external = d/'gnu.o', d/'external.o'
                    run(args.cross_prefix+'as', *abi, gas, '-o', reference)
                    harness = d/'external.c'
                    harness.write_text(EXTERNAL.replace('int main(void)', 'int common_tests(void)') + EXTERNAL_RV)
                    run(args.cross_prefix+'gcc', *abi, '-O2','-std=c11','-fno-pic','-fno-pie',
                        '-ffreestanding','-fno-stack-protector','-I',root.parent/'cornerstone/Cephyr/stdlib/include',
                        '-c',harness,'-o',external)
                    for assembled in (reference, obj):
                        exe = d/'check'
                        objects = [args.runtime/'crt0.o', assembled, external, args.runtime/'libc.a']
                        run(args.cross_prefix+'ld','--no-relax','-static','-e','_start',*objects,'-o',exe)
                        run(args.qemu,'-cpu','rv64i,m=true',exe)
                        run(args.cross_prefix+'objcopy','--dump-section',
                            f'.text={assembled}.linked',exe)
                        if args.qobjld and assembled==obj:
                            run(args.qobjld,'-e','_start',*objects,'-o',exe)
                            run(args.qemu,'-cpu','rv64i,m=true',exe)
                    assert Path(str(reference)+'.linked').read_bytes()==Path(str(obj)+'.linked').read_bytes()
    print('RV GAS bank, encoding, relocation and gating checks passed')


if __name__ == '__main__':
    main()
