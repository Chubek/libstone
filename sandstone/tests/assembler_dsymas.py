"""End-to-end built-in profiles: no bank, legacy compiler, or external assembler."""
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile


def main():
    cli, default_profile = sys.argv[1:]
    with tempfile.TemporaryDirectory(dir=Path.cwd()) as tmp:
        source = Path(tmp) / "test.s"
        output = Path(tmp) / "test.out"

        def assemble(text, *options, fmt="bin", error=None):
            source.write_text(text)
            result = subprocess.run(
                [cli, *options, "-f", fmt, "-o", str(output), str(source)],
                capture_output=True, text=True,
            )
            if error is not None:
                assert result.returncode and error in result.stderr, result
                return None
            assert result.returncode == 0, result.stderr
            return output.read_bytes()

        nops = {
            "amd64": "90", "x86": "90", "aarch64": "1f2003d5",
            "riscv32": "13000000", "riscv64": "13000000",
        }
        if default_profile in nops:
            assert assemble("nop\n").hex() == nops[default_profile]

        assert assemble("mov eax, ebx\nmov ah, bl\nmov spl, bl\n",
                        "--target", "amd64").hex() == "89d888dc4088dc"
        assert assemble("jmp done\nnop\ndone: ret\n",
                        "--target", "amd64").hex() == "e90100000090c3"
        text = "addq %rcx, %rax\ncall ext+8\n"
        doc = json.loads(assemble(text, "--target", "amd64", "-s", "att", fmt="obj"))
        assert doc["sections"][0]["data"] == "4801c8e800000000"
        assert doc["relocs"] == [{
            "section": ".text", "offset": 4, "symbol": "ext", "addend": 8,
            "type": "R_X86_64_PLT32", "width": 32, "pcrel": True,
        }]
        elf = assemble(text, "--target", "amd64", "-s", "att", fmt="elf")
        assert elf[:6] == b"\x7fELF\x02\x01"
        shoff = struct.unpack_from("<Q", elf, 40)[0]
        shentsize, shnum = struct.unpack_from("<HH", elf, 58)
        rela = [
            struct.unpack_from("<QQq", elf, struct.unpack_from("<Q", elf, off + 24)[0])
            for off in range(shoff, shoff + shentsize * shnum, shentsize)
            if struct.unpack_from("<I", elf, off + 4)[0] == 4  # SHT_RELA
        ]
        assert len(rela) == 1 and rela[0][0] == 4 and rela[0][2] == 4, rela

        assert assemble("mov x1, #42\nadd x2, x1, x3\nret\n",
                        "--target", "aarch64").hex() == "410580d22200038bc0035fd6"
        text = "addi x1, x2, -16\nlw a0, 12(a1)\n"
        for profile in ("riscv32", "rv64im"):
            assert assemble(text, "--target", profile).hex() == "930001ff03a5c500"
        assert assemble("fmadd.s f1,f2,f3,f4,7\n",
                        "--target", "riscv64").hex() == "c3703120"
        assemble("mul x1, x2, x3\n", "--target", "rv64im", "-F", "-m",
                 error="unavailable in active ISA")
        assemble(text, "--target", "unknown", error="unknown built-in target")
        assemble(text, "--target", "rv64im", "--bank", "unused.symas",
                 error="mutually exclusive")
        assemble(text, "--bank", "missing.symas", error="cannot open bank")


if __name__ == "__main__":
    main()
