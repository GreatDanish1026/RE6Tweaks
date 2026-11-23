"""Compare RE6Tweaks probe snapshots: lists each object's float fields that differ between
snapshots, flagging values in a field-of-view-like range (10..120).
Usage: python tools/snapdiff.py <probe folder> [pattern]   (pattern filters object labels)"""
import glob
import os
import re
import sys

folder = sys.argv[1]
pat = sys.argv[2] if len(sys.argv) > 2 else ""
snaps = []
for path in sorted(glob.glob(os.path.join(folder, "snapshot-*.txt"))):
    objs = {}
    cur = None
    for line in open(path, encoding="latin-1"):
        if line.startswith("=== "):
            m = re.match(r"=== (.*?) ([0-9a-f]{8}) (\S+), (\d+) bytes", line)
            label = m.group(1).split(" (static")[0] + " " + m.group(3)
            cur = objs.setdefault(label, {})
            cur["__addr"] = m.group(2)
            continue
        m = re.match(r"\+([0-9a-f]{5})  ([0-9a-f]{8})  (f=\S+)?\s*(-> (\S+))?", line)
        if m and cur is not None:
            cur[int(m.group(1), 16)] = (m.group(2), m.group(3)[2:] if m.group(3) else None, m.group(5))
    snaps.append((os.path.basename(path), objs))

labels = sorted({l for _, o in snaps for l in o if pat in l})
for label in labels:
    present = [(n, o[label]) for n, o in snaps if label in o]
    if len(present) < 2:
        continue
    print(f"\n### {label}  ({', '.join(n + '@' + d['__addr'] for n, d in present)})")
    offs = sorted({k for _, d in present for k in d if k != "__addr"})
    shown = 0
    for off in offs:
        vals = [d.get(off, ("--------", None, None)) for _, d in present]
        if len({v[0] for v in vals}) == 1:
            continue
        if not any(v[1] for v in vals):
            continue  # not float-like in any snapshot
        fl = []
        for v in vals:
            try:
                fl.append(float(v[1]) if v[1] else None)
            except ValueError:
                fl.append(None)
        fov = all(x is not None and 10 <= x <= 120 for x in fl)
        print(f"  +{off:05x} {'FOV?' if fov else '    '} " + "  ".join(f"{(v[1] or v[0]):>12}" for v in vals))
        shown += 1
        if shown > 120:
            print("  ...")
            break
