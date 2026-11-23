"""Find MT Framework property registrations: for each name, print the code that pushes the
name string, so the type/offset arguments around it can be read.
Usage: python tools/props.py BH6.exe mMapSize mShadowViewSize ..."""
import struct
import sys

b = open(sys.argv[1], "rb").read()
base = 0x400000
secs = [(0x1000, 0x110002b, 0x400), (0x1102000, 0x235caa, 0x1100600), (0x1338000, 0x140b00, 0x1336400)]


def off2va(off):
    for va, vs, ra in secs:
        if ra <= off < ra + vs:
            return base + va + off - ra


for name in sys.argv[2:]:
    needle = b"\0" + name.encode() + b"\0"
    i = b.find(needle)
    while i >= 0:
        sva = off2va(i + 1)
        pat = b"\x68" + struct.pack("<I", sva)  # push imm32
        p = b.find(pat)
        print(f"{name} @ {sva:08x}")
        while p >= 0:
            print(f"   push at {off2va(p):08x}: {b[p - 24:p].hex(' ')} | {b[p:p + 24].hex(' ')}")
            p = b.find(pat, p + 1)
        i = b.find(needle, i + 1)
