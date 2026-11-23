import struct, sys

b = open(sys.argv[1], "rb").read()
base = 0x400000
secs = [(".text", 0x1000, 0x110002b, 0x400, 0x1100200), (".rdata", 0x1102000, 0x235caa, 0x1100600, 0x235e00),
        (".data", 0x1338000, 0x140b00, 0x1336400, 0x88a00)]
def va2off(v):
    r = v - base
    for nm, va, vs, ra, rs in secs:
        if va <= r < va + rs:
            return ra + r - va
def off2va(off):
    for nm, va, vs, ra, rs in secs:
        if ra <= off < ra + rs:
            return base + va + off - ra
def d(v):
    return struct.unpack_from("<I", b, va2off(v))[0]
def is_text(x):
    return 0x401000 <= x < 0x401000 + 0x110002b
def is_rdata(x):
    return 0x1502000 <= x < 0x1502000 + 0x235caa

for slot in [int(x, 16) for x in sys.argv[2:]]:
    # walk back while previous entries are code pointers; the entry before the vtable is the RTTI COL (in .rdata)
    v = slot
    while is_text(d(v - 4)):
        v -= 4
    col = d(v - 4)
    print(f"slot {hex(slot)} -> vtable {hex(v)} (getDTI index {(slot - v) // 4}), COL {hex(col)}")
    if is_rdata(col):
        td = d(col + 12)
        nm = b[va2off(td + 8):va2off(td + 8) + 60].split(b"\0")[0]
        print("   RTTI", nm)
    # constructor xrefs: C7 06/00/01/07 imm32 (mov [reg], vtbl)
    pat = struct.pack("<I", v)
    p = b.find(pat)
    while p >= 0:
        va = off2va(p)
        if va and is_text(va):
            ctx = b[p - 3:p + 40]
            print("   xref", hex(va), ctx.hex())
        p = b.find(pat, p + 1)
