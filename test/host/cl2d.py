#!/usr/bin/env python3
"""cl2d.py: checks the render lists of V3DRender's desktop copies (0.17)
against Mesa's v3d_packet.xml (V3D 4.2): every packet known and of
Mesa's length, the frame, the supertiles, and each tile's coordinates,
load and store, in the order asked for.

Usage: cl2d.py XML DIR    (DIR: the sim's out/cl2d)
SPDX-License-Identifier: GPL-2.0-or-later
Copyright (c) 2026 Andrew Youll
"""
import importlib.util
import os
import sys

xml, d = sys.argv[1], sys.argv[2]
spec = importlib.util.spec_from_file_location(
    "cldecode", os.path.join(os.path.dirname(os.path.abspath(__file__)), "cldecode.py"))
# cldecode runs its own checks on import; borrow only its tables
src = open(spec.origin).read().split("\nfails = 0\n")[0]
ns = {"__name__": "cldecode_tables", "sys": sys}
sys.argv = [sys.argv[0], xml, d]
exec(compile(src, spec.origin, "exec"), ns)
decode = ns["decode"]

fails = 0


def check(cond, what):
    global fails
    if not cond:
        fails += 1
        print("FAIL:", what)


SCR = int(open(os.path.join(d, "base")).read())   # the framestore's GPU address (the sim's)


def run(name, w, h, bpp, stride, src, dst, order):
    rcl = decode(open(os.path.join(d, name), "rb").read())
    n = [x for x, _, _ in rcl]
    check(n[:7] == ["Tile Rendering Mode Cfg (Common)", "Tile Rendering Mode Cfg (Clear Colors Part1)",
                    "Tile Rendering Mode Cfg (Color)", "Tile Rendering Mode Cfg (ZS Clear Values)",
                    "Tile List Initial Block Size", "Multicore Rendering Tile List Set Base",
                    "Multicore Rendering Supertile Cfg"], "%s: the header %s" % (name, n[:7]))
    c = rcl[0][1]
    check(c["Image Width (pixels)"] == w and c["Image Height (pixels)"] == h, "%s: frame" % name)
    check(c["Number of Render Targets"] == 1 and c["Maximum BPP of all render targets"] == 0
          and c["Internal Depth Type"] == 2, "%s: common %s" % (name, c))
    check(rcl[2][1]["Render Target 0 Internal Type"] == 2, "%s: RT0 8-bit" % name)
    check(rcl[3][1]["Z Clear Value"] == 0x3F800000, "%s: Z 1.0" % name)
    check(rcl[4][1]["Use auto-chained tile lists"] == 1, "%s: auto-chained" % name)
    tx, ty = (w + 63) // 64, (h + 63) // 64
    sc = rcl[6][1]
    check(sc["Total Frame Width in Tiles"] == tx and sc["Total Frame Height in Tiles"] == ty,
          "%s: frame in tiles" % name)
    fw, fh = sc["Total Frame Width in Supertiles"], sc["Total Frame Height in Supertiles"]
    sw, sh = sc["Supertile Width in Tiles"], sc["Supertile Height in Tiles"]
    check(fw * sw >= tx and fh * sh >= ty and fw * fh < 256, "%s: supertiles" % name)
    check(sc["Number of Bin Tile Lists"] == 1 and sc["Multicore Enable"] == 0, "%s: one list" % name)
    body = rcl[7:]
    check(len(body) == tx * ty * 5 + 1 and body[-1][0] == "End of rendering", "%s: body length" % name)
    rows = list(range(ty))[::-1] if order & 2 else list(range(ty))
    cols = list(range(tx))[::-1] if order & 1 else list(range(tx))
    want = [(r, cc) for r in rows for cc in cols]
    fmt = 27 if bpp == 4 else 7
    for i, (r, cc) in enumerate(want):
        t = body[i * 5:i * 5 + 5]
        check([x for x, _, _ in t] == ["Tile Coordinates", "Load Tile Buffer General", "End of Loads",
                                       "Store Tile Buffer General", "End of Tile Marker"],
              "%s: tile %d packets" % (name, i))
        co = t[0][1]
        check((co["tile row number"], co["tile column number"]) == (r, cc), "%s: tile %d at %s" % (name, i, (r, cc)))
        ld, st = t[1][1], t[3][1]
        check(ld["Buffer to Load"] == 0 and ld["Memory Format"] == 0 and ld["Input Image Format"] == fmt
              and ld["Height in UB or Stride"] == stride and ld["Address"] == SCR + src and ld["Height"] == 0
              and ld["Flip Y"] == 0 and ld["R/B swap"] == 0 and ld["Channel Reverse"] == 0,
              "%s: load %s" % (name, ld))
        check(st["Buffer to Store"] == 0 and st["Memory Format"] == 0 and st["Output Image Format"] == fmt
              and st["Height in UB or Stride"] == stride and st["Address"] == SCR + dst and st["Height"] == 0
              and st["Dither Mode"] == 0 and st["Clear buffer being stored"] == 0,
              "%s: store %s" % (name, st))


def fill(name, w, h, stride, dst, rgba):
    rcl = decode(open(os.path.join(d, name), "rb").read())
    check(all(x == "NOP" for x, _, _ in rcl[12:15]) and all(x == "NOP" for x, _, _ in rcl[19::5]) and len(rcl[19::5]) == ((w + 63) // 64) * ((h + 63) // 64),
          "%s: the padding is NOPs" % name)
    rcl = [p for p in rcl if p[0] != "NOP"]
    n = [x for x, _, _ in rcl]
    check(n[1] == "Tile Rendering Mode Cfg (Clear Colors Part1)"
          and rcl[1][1]["Clear Color low 32 bits"] == rgba, "%s: clear colour" % name)
    tx, ty = (w + 63) // 64, (h + 63) // 64
    first = rcl[7:12]
    check([x for x, _, _ in first] == ["Tile Coordinates", "End of Loads", "Store Tile Buffer General",
                                       "Clear Tile Buffers", "End of Tile Marker"]
          and first[2][1]["Buffer to Store"] == 8 and first[3][1]["Clear all Render Targets"] == 1,
          "%s: the clearing tile %s" % (name, first))
    body = rcl[12:]
    check(len(body) == tx * ty * 4 + 1 and body[-1][0] == "End of rendering", "%s: body" % name)
    for i in range(tx * ty):
        t = body[i * 4:i * 4 + 4]
        co, st = t[0][1], t[2][1]
        check([x for x, _, _ in t] == ["Tile Coordinates", "End of Loads", "Store Tile Buffer General",
                                       "End of Tile Marker"]
              and (co["tile row number"], co["tile column number"]) == divmod(i, tx)
              and st["Buffer to Store"] == 0 and st["Output Image Format"] == 27
              and st["Height in UB or Stride"] == stride and st["Address"] == SCR + dst
              and st["Clear buffer being stored"] == 0, "%s: tile %d" % (name, i))


fill("fill-300x200.bin", 300, 200, 7680, 7680 * 500 + 40, 0x00FF8040)
run("copy-200x100.bin", 200, 100, 4, 7680, 10 * 7680 + 40, 300 * 7680 + 800, 0)
run("scroll-130x70.bin", 130, 70, 4, 7680, 0, 7680 * 20 + 32, 3)
run("copy16-100x64.bin", 100, 64, 2, 3840, 0, 3840 * 300, 0)

def present(name, w, h, src, dst, load, store):
    """(0.31) PRESENT's lists: one frame, each tile loaded from the source
    with its own General word and stored to the screen with another"""
    rcl = decode(open(os.path.join(d, name), "rb").read())
    c = rcl[0][1]
    check(c["Image Width (pixels)"] == w and c["Image Height (pixels)"] == h, "%s: frame" % name)
    tx, ty = (w + 63) // 64, (h + 63) // 64
    body = rcl[7:]
    check(len(body) == tx * ty * 5 + 1 and body[-1][0] == "End of rendering", "%s: body length" % name)
    for i in range(tx * ty):
        t = body[i * 5:i * 5 + 5]
        check([x for x, _, _ in t] == ["Tile Coordinates", "Load Tile Buffer General", "End of Loads",
                                       "Store Tile Buffer General", "End of Tile Marker"],
              "%s: tile %d packets" % (name, i))
        co, ld, st = t[0][1], t[1][1], t[3][1]
        check((co["tile row number"], co["tile column number"]) == divmod(i, tx), "%s: tile %d" % (name, i))
        check(ld["Address"] == src and ld["Buffer to Load"] == 0 and ld["Height"] == 0 and ld["Flip Y"] == 0
              and ld["Channel Reverse"] == 0 and all(ld[k] == v for k, v in load.items()),
              "%s: load %s" % (name, ld))
        check(st["Address"] == dst and st["Buffer to Store"] == 0 and st["Height"] == 0 and st["Dither Mode"] == 0
              and st["Clear buffer being stored"] == 0 and all(st[k] == v for k, v in store.items()),
              "%s: store %s" % (name, st))


for ln in open(os.path.join(d, "present")):
    kind, a, b, w, h = ln.split()
    a, b, w, h = int(a), int(b), int(w), int(h)
    if kind == "5551":   # ARGB8888 raster onto a 1:5:5:5 TRGB screen
        present("present-5551.bin", w, h, a, b,
                {"Memory Format": 0, "Input Image Format": 27, "R/B swap": 1, "Force alpha 1": 1,
                 "Height in UB or Stride": 1216},
                {"Memory Format": 0, "Output Image Format": 45, "R/B swap": 1, "Height in UB or Stride": 1280})
    else:                # a UIF frame (padded to 72 rows: 9 blocks) untiled to the staging buffer
        present("present-uif.bin", w, h, a, b,
                {"Memory Format": 4, "Input Image Format": 27, "R/B swap": 0, "Force alpha 1": 1,
                 "Height in UB or Stride": 9},
                {"Memory Format": 0, "Output Image Format": 27, "R/B swap": 0, "Height in UB or Stride": 400})
print("cl2d: %s" % ("all ok" if not fails else "%d failed" % fails))
sys.exit(1 if fails else 0)
