"""Class name -> DTI -> getDTI (mov eax, dti; ret) -> vtables that hold getDTI in slot 4, plus
the createProperty function (slot 3) and the vtable length (entries until a non-code value).
Usage: python -I tools/vtables.py BH6.exe uTVNoiseFilter uDOFFilter ..."""
import struct
import sys

b = open(sys.argv[1], "rb").read()
BASE = 0x400000
SECS = [(0x1000, 0x110002B, 0x400), (0x1102000, 0x235CAA, 0x1100600), (0x1338000, 0x140B00, 0x1336400)]


def va2off(v):
    for va, vs, ra in SECS:
        if BASE + va <= v < BASE + va + vs:
            return v - BASE - va + ra


def off2va(o):
    for va, vs, ra in SECS:
        if ra <= o < ra + vs:
            return BASE + va + o - ra


def d(v):
    o = va2off(v)
    return struct.unpack_from("<I", b, o)[0] if o is not None else None


def is_text(v):
    return v is not None and 0x401000 <= v < 0x1501000


for name in sys.argv[2:]:
    found = False
    i = b.find(b"\0" + name.encode() + b"\0")
    while i >= 0 and not found:
        sva = off2va(i + 1)
        pat = b"\x68" + struct.pack("<I", sva)
        p = b.find(pat)
        while p >= 0:
            if b[p + 5] == 0xB9:
                dti = struct.unpack_from("<I", b, p + 6)[0]
                g = b"\xb8" + struct.pack("<I", dti) + b"\xc3"
                q = b.find(g)
                while q >= 0:
                    gva = off2va(q)
                    r = b.find(struct.pack("<I", gva))
                    while r >= 0:
                        rv = off2va(r)
                        if rv and 0x1502000 <= rv < 0x1738000:
                            vt = rv - 16
                            n = 0
                            while n < 80 and is_text(d(vt + 4 * n)):
                                n += 1
                            print(f"{name:28s} dti {dti:08x} vtable {vt:08x} entries {n:2d} createProperty {d(vt + 12):08x}")
                            found = True
                        r = b.find(struct.pack("<I", gva), r + 1)
                    q = b.find(g, q + 1)
            p = b.find(pat, p + 1)
        i = b.find(b"\0" + name.encode() + b"\0", i + 1)
    if not found:
        print(f"{name:28s} not found")
