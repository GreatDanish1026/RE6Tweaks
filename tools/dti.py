import struct, sys

path = sys.argv[1]
names = sys.argv[2:] or ["sCamera", "sRender", "sShadow", "sMain", "sScene", "uBioFreeCamera"]
b = open(path, "rb").read()
pe = struct.unpack_from("<I", b, 0x3c)[0]
nsec = struct.unpack_from("<H", b, pe + 6)[0]
optsz = struct.unpack_from("<H", b, pe + 20)[0]
base = struct.unpack_from("<I", b, pe + 24 + 28)[0]
secs = []
o = pe + 24 + optsz
for i in range(nsec):
    nm = b[o:o + 8].rstrip(b"\0").decode()
    vs, va, rs, ra = struct.unpack_from("<IIII", b, o + 8)
    secs.append((nm, va, vs, ra, rs))
    o += 40
print("base", hex(base))
for s in secs:
    print(s[0], hex(s[1]), hex(s[2]), hex(s[3]), hex(s[4]))

def off2va(off):
    for nm, va, vs, ra, rs in secs:
        if ra <= off < ra + rs:
            return base + va + off - ra
def va2off(v):
    r = v - base
    for nm, va, vs, ra, rs in secs:
        if va <= r < va + rs:
            return ra + r - va
def secname(v):
    r = v - base
    for nm, va, vs, ra, rs in secs:
        if va <= r < va + max(vs, rs):
            return nm

text = [s for s in secs if s[0] == ".text"][0]
for name in names:
    needle = name.encode() + b"\0"
    i = 0
    hits = []
    while True:
        i = b.find(needle, i)
        if i < 0:
            break
        if b[i - 1] == 0:
            hits.append(off2va(i))
        i += 1
    for sva in hits:
        refs = []
        p = b.find(struct.pack("<I", sva))
        while p >= 0:
            refs.append(p)
            p = b.find(struct.pack("<I", sva), p + 1)
        print(f"\n{name} string {hex(sva)}: {len(refs)} refs")
        for r in refs[:8]:
            v = off2va(r)
            sn = secname(v)
            ctx = struct.unpack_from("<8I", b, r - 4)
            print("  ref", hex(v), sn, " ".join(hex(x) for x in ctx))
            # DTI object candidate: name at +4 -> object at v-4. Look for mov eax, dti ; ret
            dti = None
            if sn == ".text" and b[r + 4] == 0xB9:
                dti = struct.unpack_from("<I", b, r + 5)[0]
                parent = struct.unpack_from("<I", b, r - 4)[0] if b[r - 1] == 0x68 else None
                print("    DTI", hex(dti), "parent push", hex(parent) if parent else None,
                      "next bytes", b[r + 9:r + 40].hex())
            if dti:
                pat = b"\xb8" + struct.pack("<I", dti) + b"\xc3"
                q = b.find(pat, text[3], text[3] + text[4])
                getters = []
                while q >= 0:
                    getters.append(off2va(q))
                    q = b.find(pat, q + 1, text[3] + text[4])
                for g in getters[:4]:
                    vt = []
                    z = b.find(struct.pack("<I", g))
                    while z >= 0:
                        if secname(off2va(z)) != ".text":
                            vt.append(off2va(z))
                        z = b.find(struct.pack("<I", g), z + 1)
                    print("    getDTI", hex(g), "in vtable slots", [hex(x) for x in vt[:6]])
