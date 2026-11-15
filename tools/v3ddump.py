#!/usr/bin/env python3
"""v3ddump.py: prints a V3DRender hang dump (*V3DDump, V3DRender 0.06).

The dump holds the first job that hung since V3DRender was loaded: the
GPU's registers at the time, the job's lists and every BO the job held,
as the GPU left them (tile alloc and tile state included).

Usage: v3ddump.py DUMP [-x DIR] [--xml v3d_packet.xml] [--bo ADDR]
  -x DIR    also write each BO to DIR/<gpu address>.bin
  --bo ADDR also hex-dump the whole BO holding GPU address ADDR

Lists are decoded with Mesa's v3d_packet.xml (V3D 4.2); decoding of a
list stops at the first opcode the XML doesn't know (the binner's
compressed primitive data, say), and the rest is shown in hex.

SPDX-License-Identifier: GPL-2.0-or-later
Copyright (c) 2026 Andrew Youll
"""
import os
import struct
import sys
import xml.etree.ElementTree as ET

VER = 42
HANG = ["q", "start", "end", "ct0cs", "ct0ca", "ct0ea", "ct0ra", "ct1cs", "ct1ca",
        "ct1ea", "ct1ra", "pcs", "bfc", "rfc", "err_stat", "fdbgo", "fdbgb", "fdbgr",
        "fdbgs", "bpca", "bpcs", "bpoa", "bpos", "words_at"] + ["w%d" % i for i in range(8)]
QUEUES = ["bin", "render", "TFU", "compute", "cache clean"]


def load_packets(path):
    def ok(e):
        return int(e.get("min_ver", "0")) <= VER <= int(e.get("max_ver", "999"))
    packets = {}
    for p in ET.parse(path).getroot().iter("packet"):
        if not ok(p):
            continue
        fields = [f for f in p.iter("field") if ok(f)]
        bits = max([int(f.get("start")) + int(f.get("size")) for f in fields], default=0)
        packets.setdefault(int(p.get("code")), []).append((p.get("name"), fields, 1 + (bits + 7) // 8))
    return packets


def fields_text(fields, payload):
    v = int.from_bytes(payload, "little")
    out = []
    for f in fields:
        s, n = int(f.get("start")), int(f.get("size"))
        x = (v >> s) & ((1 << n) - 1)
        if f.get("minus_one") == "true":
            x += 1
        if f.get("type") == "address":
            x <<= 32 - n            # top-aligned, as Mesa's __gen_unpack_address
        if f.get("name") == "sub-id":
            continue
        out.append("%s=%s" % (f.get("name"), hex(x) if x > 9 else x))
    return ", ".join(out)


def decode(packets, mem, addr, end, limit=400):
    """Decodes from addr to end (or an unknown opcode); returns lines."""
    lines = []
    a = addr
    while a < end and len(lines) < limit:
        b = mem.read(a, 1)
        if b is None:
            lines.append("  %08X  (not in any BO of the job)" % a)
            break
        code = b[0]
        if code not in packets:
            raw = mem.read(a, min(32, end - a)) or b""
            lines.append("  %08X  ?? opcode %d; then: %s" % (a, code, raw.hex(" ")))
            break
        cands = packets[code]
        if len(cands) > 1:
            sub = (mem.read(a + 1, 1) or b"\0")[0] & 15
            m = [c for c in cands if any(f.get("name") == "sub-id" and
                 int(f.get("default", "0")) == sub for f in c[1])]
            cands = m or cands
        name, fields, length = cands[0]
        payload = mem.read(a + 1, length - 1) or b""
        lines.append("  %08X  %s%s" % (a, name, (": " + fields_text(fields, payload)) if fields else ""))
        a += length
        if name in ("Halt", "return"):
            break
    return lines


class Mem:
    def __init__(self, bos):
        self.bos = bos            # list of (addr, size, data)

    def find(self, addr):
        for a, size, data in self.bos:
            if a <= addr < a + max(size, 1):
                return a, size, data
        return None

    def read(self, addr, n):
        b = self.find(addr)
        if not b or not b[2]:
            return None
        o = addr - b[0]
        return b[2][o:o + n]


def hexdump(mem, addr, n):
    out = []
    for a in range(addr & ~15, addr + n, 16):
        d = mem.read(a, 16)
        if d is None:
            break
        out.append("  %08X  %s" % (a, d.hex(" ")))
    return out


def main():
    args = sys.argv[1:]
    xml = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "test", "host", "v3d_packet.xml")
    xdir = bo_at = None
    if "--xml" in args:
        i = args.index("--xml"); xml = args[i + 1]; del args[i:i + 2]
    if "-x" in args:
        i = args.index("-x"); xdir = args[i + 1]; del args[i:i + 2]
    if "--bo" in args:
        i = args.index("--bo"); bo_at = int(args[i + 1], 0); del args[i:i + 2]
    if len(args) != 1:
        raise SystemExit(__doc__)
    raw = open(args[0], "rb").read()
    w = struct.unpack_from("<12I", raw)
    if w[0] != 0x44443356 or w[1] != 0x31504D55:
        raise SystemExit("not a V3DRender dump")
    head = w[2]
    q, rstart, rend, bstart, bend, qma, qms, qts, nbos = w[3:12]
    hw = struct.unpack_from("<%dI" % len(HANG), raw, head * 4)
    h = dict(zip(HANG, hw))
    p = (head + len(HANG)) * 4
    bos = []
    for _ in range(nbos):
        a, size = struct.unpack_from("<2I", raw, p)
        p += 8
        bos.append((a, size, raw[p:p + size]))
        p += size
    mem = Mem(bos)
    packets = load_packets(xml)

    print("Hung: %s job" % (QUEUES[q] if q < len(QUEUES) else q))
    print("Render list %08X-%08X, bin list %08X-%08X" % (rstart, rend, bstart, bend))
    print("Tile alloc (qma) %08X, %d bytes (qms); tile state (qts) %08X" % (qma, qms, qts))
    for k in ("ct0", "ct1"):
        print("%s: CS %08X CA %08X EA %08X RA %08X" % (k.upper(), h[k + "cs"], h[k + "ca"], h[k + "ea"], h[k + "ra"]))
    print("PCS %08X BFC %d RFC %d ERR_STAT %08X" % (h["pcs"], h["bfc"], h["rfc"], h["err_stat"]))
    print("FDBGO %08X FDBGB %08X FDBGR %08X FDBGS %08X" % (h["fdbgo"], h["fdbgb"], h["fdbgr"], h["fdbgs"]))
    print("Binner memory: BPCA %08X BPCS %08X BPOA %08X BPOS %08X" % (h["bpca"], h["bpcs"], h["bpoa"], h["bpos"]))

    def where(addr):
        b = mem.find(addr)
        if not b:
            return "not in the job's BOs"
        role = []
        if b[0] <= rstart < b[0] + b[1]: role.append("render list")
        if b[0] <= bstart < b[0] + b[1]: role.append("bin list")
        if qma and b[0] <= qma < b[0] + b[1]: role.append("tile alloc")
        if qts and b[0] <= qts < b[0] + b[1]: role.append("tile state")
        return "BO %08X (%d KB%s) + &%X" % (b[0], b[1] // 1024,
                                           ", " + ", ".join(role) if role else "", addr - b[0])
    print()
    print("BOs (%d):" % nbos)
    for a, size, data in bos:
        print("  %08X %7d bytes%s" % (a, size, "" if data or not size else " (not kept)"))
    print()
    for k in ("ct0ca", "ct0ra", "ct1ca", "ct1ra", "bpca"):
        if h[k]:
            print("%s %08X is in %s" % (k.upper(), h[k], where(h[k])))
    print()
    print("Render list:")
    rl = decode(packets, mem, rstart, rend)
    print("\n".join(rl))
    import re
    for l in rl:
        m = re.search(r"Generic Tile List: start=(0x[0-9a-f]+), end=(0x[0-9a-f]+)", l)
        if m:
            gs, ge = int(m.group(1), 16), int(m.group(2), 16)
            print()
            print("Generic tile list %08X-%08X:" % (gs, ge))
            print("\n".join(decode(packets, mem, gs, ge)))
            break
    if bstart != bend:
        print()
        print("Bin list:")
        print("\n".join(decode(packets, mem, bstart, bend)))
    ca = h["ct1ca"] if q == 1 else h["ct0ca"]
    print()
    print("Around the hung list's CA (%08X):" % ca)
    print("\n".join(hexdump(mem, max(ca - 64, 0), 128)))
    if qma:
        print()
        print("Tile alloc, first 1 KB (each tile's first block):")
        print("\n".join(hexdump(mem, qma, 1024)))
        print()
        print("Tile lists, decoded from each 64-byte block start (until an unknown opcode):")
        for t in range(16):
            a = qma + 64 * t
            lines = decode(packets, mem, a, a + 64, limit=12)
            print(" tile block %d:" % t)
            print("\n".join(lines))
    if bo_at is not None:
        b = mem.find(bo_at)
        if b:
            print()
            print("BO %08X:" % b[0])
            print("\n".join(hexdump(mem, b[0], b[1])))
    if xdir:
        os.makedirs(xdir, exist_ok=True)
        for a, size, data in bos:
            if data:
                open(os.path.join(xdir, "%08X.bin" % a), "wb").write(data)


main()
