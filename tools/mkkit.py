#!/usr/bin/env python3
"""mkkit.py: the Pi test kit as a RISC OS zip.

    tools/mkkit.py OUT.zip FOLDER FILE,xxx ...

Each FILE,xxx goes into FOLDER in the zip under its plain name, with its
RISC OS file type in the Acorn "ARC0" extra field (as SparkFS and
Info-ZIP on RISC OS write it), so unzipping on RISC OS gives the types
back.

SPDX-License-Identifier: GPL-2.0-or-later
Copyright (c) 2026 Andrew Youll
"""
import os, struct, sys, time, zipfile

def arc0(filetype, cs):
    load = 0xFFF00000 | (filetype << 8) | ((cs >> 32) & 0xFF)
    return struct.pack("<HH4sIII4x", 0x4341, 20, b"ARC0", load, cs & 0xFFFFFFFF, 0x13)

def main():
    out, folder, files = sys.argv[1], sys.argv[2], sys.argv[3:]
    now = time.time()
    cs = int((now + 2208988800) * 100)
    t = time.localtime(now)[:6]
    with zipfile.ZipFile(out, "w") as z:
        d = zipfile.ZipInfo(folder + "/", t)
        d.create_system = 13
        d.external_attr = 0x41ED0010
        d.extra = arc0(0x1000 & 0xFFF, cs)
        z.writestr(d, b"")
        for f in files:
            base = os.path.basename(f)
            name, ftype = base.rsplit(",", 1)
            i = zipfile.ZipInfo(folder + "/" + name, t)
            i.create_system = 13
            i.compress_type = zipfile.ZIP_DEFLATED
            i.external_attr = 0x81A40000
            i.extra = arc0(int(ftype, 16), cs)
            z.writestr(i, open(f, "rb").read())

main()
