"""For each property/key name: find code that stores its string address (mov [esp+N], imm32 or
push imm32), the enclosing function, the vtables that point at that function and the class
names behind them (from the DTI constructor that pushes the name).
Usage: python -I tools/owners.py BH6.exe mLODBeginDistance mDrawDist ..."""
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


def fstart(v):
    o = va2off(v)
    while not (b[o - 1] == 0xCC and b[o - 2] == 0xCC):
        o -= 1
    return off2va(o)


def is_text(v):
    return v is not None and 0x401000 <= v < 0x1501000


def dti_name(dti):
    pat = b"\xb9" + struct.pack("<I", dti)
    p = b.find(pat)
    while p >= 0:
        if b[p - 5] == 0x68:
            so = va2off(struct.unpack_from("<I", b, p - 4)[0])
            if so:
                return b[so:so + 64].split(b"\0")[0].decode(errors="replace")
        p = b.find(pat, p + 1)
    return None


def classes_for(fn):
    out = []
    p = b.find(struct.pack("<I", fn))
    while p >= 0:
        slot = off2va(p)
        if slot and 0x1502000 <= slot < 0x1738000:
            # vtable start: walk back over code pointers
            v = slot
            while is_text(d(v - 4)):
                v -= 4
            g = d(v + 16)  # getDTI in slot 4
            name = None
            if is_text(g):
                go = va2off(g)
                if b[go] == 0xB8 and b[go + 5] == 0xC3:
                    name = dti_name(struct.unpack_from("<I", b, go + 1)[0])
            out.append((slot, v, (slot - v) // 4, name))
        p = b.find(struct.pack("<I", fn), p + 1)
    return out


for name in sys.argv[2:]:
    needle = b"\0" + name.encode() + b"\0"
    i = b.find(needle)
    while i >= 0:
        sva = off2va(i + 1)
        print(f"{name} @ {sva:08x}")
        imm = struct.pack("<I", sva)
        p = b.find(imm)
        while p >= 0:
            site = off2va(p)
            if is_text(site):
                fn = fstart(site)
                cls = classes_for(fn)
                print(f"   ref {site:08x} in fn {fn:08x}: " + (", ".join(
                    f"{n or '?'} (vtable {v:08x} slot {k})" for _, v, k, n in cls) or "no vtable ref"))
            p = b.find(imm, p + 1)
        i = b.find(needle, i + 1)
