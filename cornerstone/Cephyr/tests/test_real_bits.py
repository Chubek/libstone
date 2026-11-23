"""Exact-rational differential checks for Libstone's software IEEE arithmetic."""
import ctypes as C
from fractions import Fraction
import os
from pathlib import Path
import random
import subprocess
import tempfile


class Words(C.Structure):
    _fields_ = [("lo", C.c_uint64), ("hi", C.c_uint64)]

    @classmethod
    def of(cls, x):
        return cls(x & ((1 << 64)-1), x >> 64)

    def raw(self):
        return self.lo + (self.hi << 64)


FORMATS = {32: (23, 127), 64: (52, 1023), 128: (112, 16383)}


def value(w, raw):
    f, bias = FORMATS[w]
    e, m = divmod(raw % (1 << (w-1)), 1 << f)
    if e == 2*bias+1:
        return None
    if e:
        m += 1 << f
    return (-1 if raw >> (w-1) else 1) * Fraction(m) * Fraction(2)**(max(1, e)-bias-f)


def encode(w, x, negative_zero=False):
    f, bias = FORMATS[w]
    sign = int(x < 0 or (not x and negative_zero)) << (w-1)
    if not x:
        return sign
    x = abs(x)
    e = x.numerator.bit_length() - x.denominator.bit_length()
    if x < Fraction(2)**e:
        e -= 1
    e = max(e, 1-bias)
    m = round(x / Fraction(2)**(e-f))
    if m == 2 << f:
        m //= 2
        e += 1
    if e > bias:
        return sign + ((2*bias+1) << f)
    return sign + (m if m < 1 << f else ((e+bias-1) << f) + m)


def test_real_bits():
    root = Path(__file__).resolve().parents[1] / "stdlib/src"
    with tempfile.TemporaryDirectory() as d:
        dll = Path(d) / "bits.so"
        subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-fPIC", "-shared",
                        "-O2", root / "real_bits.c", "-o", dll], check=True)
        lib = C.CDLL(str(dll))
        calc = lib.cephyr_real_calc
        calc.argtypes = (C.c_uint, C.c_char, Words, Words)
        calc.restype = Words
        cast = lib.cephyr_real_cast
        cast.argtypes = (C.c_uint, C.c_uint, Words)
        cast.restype = Words
        cmp = lib.cephyr_real_cmp
        cmp.argtypes = (C.c_uint, Words, Words)
        cmp.restype = C.c_int
        from_int = lib.cephyr_real_from_int
        from_int.argtypes = (C.c_uint, C.c_int, C.c_uint64)
        from_int.restype = Words
        to_int = lib.cephyr_real_to_int
        to_int.argtypes = (C.c_uint, Words, C.c_uint, C.c_int)
        to_int.restype = C.c_uint64

        # Host constant folding may convert through x87's explicit integer
        # bit, even when neither the host nor the destination ABI is x87.
        for exponent, mantissa in [(0,1), (0,(1<<63)-1), (1,1<<63),
                                   (16383,(1<<63)+1), (32766,(1<<64)-1)]:
            for sign in (0,1):
                ext = sign << 79 | exponent << 64 | mantissa
                exact = (-1 if sign else 1) * Fraction(mantissa) * Fraction(2)**(max(exponent,1)-16383-63)
                quad = cast(128,80,Words.of(ext))
                assert quad.raw() == encode(128,exact)
                assert cast(80,128,quad).raw() == ext

        rng = random.Random(839)
        for w, (f, bias) in FORMATS.items():
            s = 1 << (w-1)
            inf = (2*bias+1) << f
            boundary = [0, 1, 2, 3, (1 << f)-1, 1 << f, (1 << f)+1,
                        (bias-1) << f, bias << f, (bias << f)+1, inf-1, inf-2]
            boundary += [s+b for b in boundary]
            for n in [0, 1, (1 << 24)+1, (1 << 24)+3, (1 << 53)+1, (1 << 53)+3,
                      (1 << 63)-1, 1 << 63, (1 << 64)-1] + [rng.getrandbits(64) for _ in range(32)]:
                for neg in (0,1):
                    assert from_int(w,neg,n).raw() == encode(w,-n if neg else n,neg)
            for i in range(1200 + len(boundary)**2):
                if i < len(boundary)**2:
                    a, b = boundary[i//len(boundary)], boundary[i%len(boundary)]
                else:
                    a, b = rng.getrandbits(w), rng.getrandbits(w)
                x, y = value(w,a), value(w,b)
                if x is None or y is None:
                    continue
                A, B = Words.of(a), Words.of(b)
                for n in (32,64):
                    for signed in (False,True):
                        low = -(1 << (n-1)) if signed else 0
                        high = (1 << (n-int(signed))) - 1
                        expected = min(high,max(low,int(x))) % (1 << 64)
                        assert to_int(w,A,n,signed) == expected, ("int",w,n,signed,hex(a))
                for to in FORMATS:
                    expected = encode(to, x, bool(a & s))
                    assert cast(to,w,A).raw() == expected, ("cast",w,to,hex(a))
                assert cmp(w,A,B) == (x > y)-(x < y)
                for op in "+-*/":
                    if op == "/" and not y:
                        continue
                    if op == "+": z = x+y
                    elif op == "-": z = x-y
                    elif op == "*": z = x*y
                    else: z = x/y
                    minus_zero = (bool((a^b) & s) if op in "*/" else
                                  bool(not x and not y and a & s and (bool(b & s) ^ (op=="-"))))
                    expected = encode(w,z,minus_zero)
                    got = calc(w,op.encode(),A,B).raw()
                    assert got == expected, (w,op,hex(a),hex(b),hex(got),hex(expected))
            # Invalid operations return a quiet NaN; payload and sign are not specified.
            for op, a, b in [("-",inf,inf), ("*",0,inf), ("/",0,0), ("/",inf,inf), ("+",inf+1,0)]:
                got = calc(w,op.encode(),Words.of(a),Words.of(b)).raw()
                assert got & inf == inf and got & (1 << (f-1))
                assert cmp(w, Words.of(got), Words.of(0)) == 2
            assert calc(w,b"/",Words.of(bias << f),Words.of(s)).raw() == s+inf


if __name__ == "__main__":
    test_real_bits()
