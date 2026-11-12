#!/usr/bin/env python3
"""cldecode.py: decodes control lists with Mesa's v3d_packet.xml (V3D 4.2)
and checks the kit's clear jobs: every opcode known, every packet the
length Mesa's packers give it, and the fields that matter as intended.

Usage: cldecode.py XML DIR
SPDX-License-Identifier: GPL-2.0-or-later
Copyright (c) 2026 Andrew Youll
"""
import sys
import xml.etree.ElementTree as ET

VER = 42
root = ET.parse(sys.argv[1]).getroot()
d = sys.argv[2]


def ok(e):
    return int(e.get("min_ver", "0")) <= VER <= int(e.get("max_ver", "999"))


packets = {}          # code -> list of (name, fields, length)
for p in root.iter("packet"):
    if not ok(p):
        continue
    fields = [f for f in p.iter("field") if ok(f)]
    bits = max([int(f.get("start")) + int(f.get("size")) for f in fields], default=0)
    length = 1 + (bits + 7) // 8
    packets.setdefault(int(p.get("code")), []).append((p.get("name"), fields, length))


def field_values(fields, payload):
    v = int.from_bytes(payload, "little")
    out = {}
    for f in fields:
        s, n = int(f.get("start")), int(f.get("size"))
        x = (v >> s) & ((1 << n) - 1)
        if f.get("minus_one") == "true":
            x += 1
        out[f.get("name")] = x
    return out


def decode(data):
    i, out = 0, []
    while i < len(data):
        code = data[i]
        if code not in packets:
            raise SystemExit("FAIL: unknown opcode %d at %d" % (code, i))
        cands = packets[code]
        if len(cands) > 1:      # sub-id packets (Tile Rendering Mode Cfg)
            sub = data[i + 1] & 15
            cands = [c for c in cands if any(f.get("name") == "sub-id" and
                     int(f.get("default", "0")) == sub for f in c[1])]
        name, fields, length = cands[0]
        out.append((name, field_values(fields, data[i + 1:i + length]), i))
        i += length
    if i != len(data):
        raise SystemExit("FAIL: list overruns its end")
    return out


fails = 0


def check(cond, what):
    global fails
    if not cond:
        fails += 1
        print("FAIL:", what)


for w, h in ((128, 128), (1920, 1080), (100, 37)):
    bcl = decode(open("%s/%d-bcl.bin" % (d, w), "rb").read())
    rcl = decode(open("%s/%d-rcl.bin" % (d, w), "rb").read())
    gen = decode(open("%s/%d-gen.bin" % (d, w), "rb").read())
    names = lambda l: [n for n, _, _ in l]
    check(names(bcl) == ["Tile Binning Mode Cfg", "Flush VCD cache", "Occlusion Query Counter",
                         "Start Tile Binning", "Flush"], "BCL %s" % names(bcl))
    tb = bcl[0][1]
    check(tb["Width (in pixels)"] == w and tb["Height (in pixels)"] == h, "binning size")
    check(tb["Number of Render Targets"] == 1, "one render target")
    check(tb["tile allocation initial block size"] == 1, "initial block 128 bytes")
    check(names(gen) == ["Tile Coordinates Implicit", "End of Loads", "Prim List Format",
                         "Set InstanceID", "Branch to Implicit Tile List",
                         "Store Tile Buffer General", "Clear Tile Buffers",
                         "End of Tile Marker", "Return from sub-list"], "generic list %s" % names(gen))
    st = gen[5][1]
    check(st["Buffer to Store"] == 0 and st["Memory Format"] == 0 and
          st["Output Image Format"] == 27 and st["Height in UB or Stride"] == w * 4 and
          st["Address"] == 0x00400000, "the store: %s" % st)
    n = names(rcl)
    check(n[:4] == ["Tile Rendering Mode Cfg (Common)", "Tile Rendering Mode Cfg (Clear Colors Part1)",
                    "Tile Rendering Mode Cfg (Color)", "Tile Rendering Mode Cfg (ZS Clear Values)"],
          "the mode config, common first and Z/S clear last: %s" % n[:4])
    c = rcl[0][1]
    check(c["Image Width (pixels)"] == w and c["Image Height (pixels)"] == h and
          c["Number of Render Targets"] == 1 and c["Early-Z disable"] == 1, "common: %s" % c)
    check(rcl[1][1]["Clear Color low 32 bits"] == 0x44332211, "clear colour")
    check(rcl[2][1]["Render Target 0 Internal Type"] == 2, "RT0 internal type 8")
    check(abs(rcl[3][1]["Z Clear Value"] - 0x3f800000) == 0, "Z clear 1.0")
    base = [v for nm, v, _ in rcl if nm == "Multicore Rendering Tile List Set Base"][0]
    check(base["address"] << 6 == 0x00800000, "tile list base = tile alloc")
    sc = [v for nm, v, _ in rcl if nm == "Multicore Rendering Supertile Cfg"][0]
    tx, ty = (w + 63) // 64, (h + 63) // 64
    check(sc["Total Frame Width in Tiles"] == tx and sc["Total Frame Height in Tiles"] == ty,
          "frame in tiles")
    fw, fh = sc["Total Frame Width in Supertiles"], sc["Total Frame Height in Supertiles"]
    sw, sh = sc["Supertile Width in Tiles"], sc["Supertile Height in Tiles"]
    check(fw * sw >= tx and fh * sh >= ty and fw * fh <= 256 and fw < 256 and fh < 256,
          "supertiles cover the frame within the limits")
    coords = [(v["column number in supertiles"], v["row number in supertiles"])
              for nm, v, _ in rcl if nm == "Supertile Coordinates"]
    check(sorted(coords) == sorted((x, y) for x in range(fw) for y in range(fh)),
          "every supertile once")
    g = [v for nm, v, _ in rcl if nm == "Start Address of Generic Tile List"][0]
    check(g["start"] == 0x00221000 and g["end"] == 0x00221000 + len(open("%s/%d-gen.bin" % (d, w), "rb").read()),
          "generic list start/end")
    check(n[-1] == "End of rendering", "ends rendering")
    check(n.count("Clear Tile Buffers") == 1 and n.count("End of Tile Marker") == 2,
          "the initial clear (twice, cleared once)")
    print("  %dx%d: BCL %d packets, RCL %d, generic %d; %dx%d tiles, supertiles %dx%d of %dx%d"
          % (w, h, len(bcl), len(rcl), len(gen), tx, ty, fw, fh, sw, sh))
print("cldecode: %s" % ("all ok" if not fails else "%d failed" % fails))
sys.exit(1 if fails else 0)
