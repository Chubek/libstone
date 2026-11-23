"""Check the actual assembly boundary, both CLI overrides, and passthrough."""
from pathlib import Path
import subprocess
import sys
import tempfile

compiler = Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory() as directory:
    d = Path(directory)
    source = d / "bounds.c"
    source.write_text("""
extern unsigned long object;
unsigned long *address(void) { return &object; }
unsigned long zero_hi(void) { return 0xffffffffUL; }
unsigned long signed_lo(void) { return 0xffffffff80000000UL; }
unsigned long below(void) { return 0xffffffff7fffffffUL; }
unsigned long above(void) { return 0x100000000UL; }
""")
    config = d / "config.toml"
    config.write_text('[paths]\ncache_path=".cache"\n[sandslab]\nenabled=true\n')

    def asm(*flags, target="x86_64-linux-gnu", file=source):
        return subprocess.check_output([compiler, "--target=" + target, "--config", config,
                                        "--no-factir", "-fPIC", *flags, "-S", file, "-o", "-"])

    baseline, candidate = asm("--no-sandslab"), asm()
    assert candidate == asm("--no-sandslab", "--sandslab")
    assert b"\tmovabsq $0xffffffff80000000, %rax\n" in baseline
    assert b"\t.byte 184,255,255,255,255\n" in candidate
    assert b"\t.byte 72,199,192,0,0,0,128\n" in candidate
    for text in (b"\tmovabsq $0xffffffff7fffffff, %rax\n",
                 b"\tmovabsq $0x100000000, %rax\n", b"object@GOTPCREL(%rip)"):
        assert text in baseline and text in candidate, text
    assert asm("--sandslab", target="aarch64-linux-gnu") == asm("--no-sandslab", target="aarch64-linux-gnu")
    raw = d / "input.s"
    raw.write_bytes(b"\t.text\n\tmovabsq $0x7, %rax\n\tmovabsq $object, %rax\n")
    assert asm("--sandslab", file=raw) == raw.read_bytes()
