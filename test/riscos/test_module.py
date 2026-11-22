#!/usr/bin/env python3
"""test_module.py: V3DRender,ffa itself, run in Unicorn (emu.py) with a
stand-in for the parts of a Raspberry Pi 4 and RISC OS it uses:

- the V3D's registers, the PM block and the ASB gates as MMIO, with a
  GPU that "runs" a job when its start register is written and finishes
  it when the test says (setting the interrupt status bit, as hardware);
- BCMSupport's property mailbox (board revision, V3D clock);
- OS_Memory 12, 13 and 65, physical memory pools (OS_DynamicArea 0, 1,
  21 and 22, mapped page by page), OS_Hardware 0 and 1 (a HAL_IRQClear
  that counts its calls), OS_ClaimDeviceVector and its release.

The interrupt is delivered by calling the module's device vector entry
in IRQ mode, with its own stack, as the kernel does.

Usage: test_module.py MODULE REGS_JSON
SPDX-License-Identifier: GPL-2.0-or-later
Copyright (c) 2026 Andrew Youll
"""
import json
import re
import struct
import sys

from unicorn import UcError
from unicorn.arm_const import UC_ARM_REG_CPSR, UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_PC
from emu import Machine, Module, EmuError, REGS, SCRATCH, RETURN, X_BIT

R = json.load(open(sys.argv[2]))
V3D_LOG = 0x60000000            # where OS_Memory 13 maps the V3D
PM_LOG = 0x61000000
HAL_CLEAR = RETURN + 0x100      # a HAL routine: counts, returns
ARMOP = RETURN + 0x200          # (0.15) the kernel's ARMops 0, 1 and 3 (8 bytes each)
IRQ_STACK = 0x31000000
AREA_BASE = 0x80000000          # dynamic areas are given out from here
GB = 0x40000000                 # physical page number = address >> 12, as on a Pi

checks = failures = 0


def check(cond, what):
    global checks, failures
    checks += 1
    if not cond:
        failures += 1
        print("FAIL:", what)


class Pi4(Machine):
    def __init__(self):
        super().__init__()
        uc = self.uc
        self.hub = {R["HUB_IDENT0"]: 0x42554856, R["HUB_IDENT1"]: 0x000E1124,
                    R["HUB_IDENT3"]: 0x00000E00, R["MMU_DEBUG_INFO"]: (6 << 8) | (2 << 4)}
        self.core = {R["CTL_IDENT0"]: 0x04443356}
        self.asb = {0x11008: 3, 0x1100C: 3}   # both gates stopped (a fresh boot)
        self.grafx = 0                          # V3D held in reset
        self.started = []                       # (queue, start, end)
        self.writes = []
        uc.mmio_map(V3D_LOG, 0x20000, self._v3d_read, None, self._v3d_write, None)
        uc.mmio_map(PM_LOG, 0x1000, self._pm_read, None, self._pm_write, None)
        uc.mem_write(HAL_CLEAR, struct.pack("<I", 0xE12FFF1E))     # BX lr
        self.hal_clears = 0
        # ARMop 0 Cache_CleanInvalidateAll, 1 Cache_CleanAll: BX lr, counted;
        # 3 Cache_RangeThreshold: MOV r0, #&100000 (1 MB); BX lr
        uc.mem_write(ARMOP, struct.pack("<IIIIII", 0xE12FFF1E, 0, 0xE12FFF1E, 0,
                                        0, 0) + struct.pack("<II", 0xE3A00601, 0xE12FFF1E))
        self.armop_calls = [0, 0]
        uc.hook_add(1 << 2, self._armop, begin=ARMOP, end=ARMOP + 8)
        uc.hook_add(1 << 2, self._code, begin=HAL_CLEAR, end=HAL_CLEAR)  # UC_HOOK_CODE
        uc.mem_map(IRQ_STACK, 0x10000)
        self.saved = None
        self.areas = {}          # number -> dict(base, pages, claimed{idx: pa}, mapped set)
        self.area_next = AREA_BASE
        self.phys_next = 0x10000                # low recommendations: from 256 MB
        self.high_next = GB >> 12               # (0.12) above 1 GB: from 1 GB
        self.high_limit = 0x100000              # a 4 GB Pi: nothing from 4 GB up
        self.honour_range = True                # RISC OS 5.29+ (R4-R7)
        self.mem12 = []                         # (flags, bytes, lo, hi) asked
        self.vector = None
        self.mailbox = []
        self.time = 1000
        self.risc3d = None
        self.risc3d_swi = 0x5AF81               # (0.16) RISC3D_Register, allocated chunk
        self.swi_lookups = []
        self.revision = 0xC03114                # (0.13) a Pi 4B 4GB: processor 3
        self.bcm = True                         # BCMSupport is there
        self.touched = 0                        # GPU/PM register accesses
        self.mem13 = 0                          # devices mapped

    # -- the GPU
    def _v3d_read(self, uc, off, size, ud):
        self.touched += 1
        if off < 0x11000 and getattr(self, "domain", 0) == 0 and getattr(self, "strict_domain", False):
            self.read_while_off = getattr(self, "read_while_off", 0) + 1   # (0.20) would abort
        if off >= 0x11000:
            return self.asb.get(off, 0)
        if off >= 0x4000:
            return self.core.get(off - 0x4000, 0)
        return self.hub.get(off, 0)

    def _v3d_write(self, uc, off, size, value, ud):
        self.touched += 1
        self.writes.append((off, value))
        if off >= 0x11000:
            v = value & 0xFFFFFF
            self.asb[off] = (v & 1) | ((v & 1) << 1)     # ACK follows REQ_STOP
            return
        if off >= 0x4000:
            o = off - 0x4000
            if o == R["CTL_INT_CLR"]:
                self.core[R["CTL_INT_STS"]] = self.core.get(R["CTL_INT_STS"], 0) & ~value
                return
            if o == R["L2TCACTL"]:
                value &= ~R["L2T_BUSY"]
            if o == R["CT0QEA"]:
                self.started.append(("bin", self.core.get(R["CT0QBA"]), value))
            if o == R["CT1QEA"]:
                self.started.append(("render", self.core.get(R["CT1QBA"]), value))
            self.core[o] = value
            return
        if off == R["HUB_INT_CLR"]:
            self.hub[R["HUB_INT_STS"]] = self.hub.get(R["HUB_INT_STS"], 0) & ~value
            return
        if off == R["MMUC_CONTROL"]:
            value &= ~R["MMUC_FLUSHING"]
        if off == R["MMU_CTL"]:
            value &= ~R["TLB_CLEARING"]
        self.hub[off] = value

    def _pm_read(self, uc, off, size, ud):
        self.touched += 1
        return self.grafx if off == 0x10C else 0

    def _pm_write(self, uc, off, size, value, ud):
        self.touched += 1
        if off == 0x10C:
            check((value >> 24) == 0x5A, "PM writes carry the password")
            self.grafx = value & 0xFFFFFF

    def _code(self, uc, address, size, ud):
        self.hal_clears += 1

    def _armop(self, uc, address, size, ud):
        self.armop_calls[(address - ARMOP) // 8] += 1

    def finish(self, bit):
        """the GPU finishes: the status bit, then the interrupt"""
        self.core[R["CTL_INT_STS"]] = self.core.get(R["CTL_INT_STS"], 0) | bit
        self.irq()

    def irq(self):
        uc = self.uc
        check(self.vector is not None, "the device vector is claimed")
        entry, r12 = self.vector
        saved_svc_sp = uc.reg_read(UC_ARM_REG_SP)
        uc.reg_write(UC_ARM_REG_CPSR, 0x92)            # IRQ mode, IRQs off
        uc.reg_write(UC_ARM_REG_SP, IRQ_STACK + 0xFFF4)  # not 8-aligned on purpose
        uc.reg_write(UC_ARM_REG_LR, RETURN)
        uc.reg_write(REGS[12], r12)
        uc.reg_write(REGS[0], 10)
        self.fault = None
        uc.emu_start(entry, RETURN, count=2_000_000)
        if self.fault:
            raise EmuError(self.fault)
        check(uc.reg_read(UC_ARM_REG_PC) == RETURN, "the interrupt returned")
        check(uc.reg_read(UC_ARM_REG_SP) == IRQ_STACK + 0xFFF4, "the IRQ stack is balanced")
        check(uc.reg_read(UC_ARM_REG_CPSR) & 0x1F == 0x12, "still in IRQ mode")
        uc.reg_write(UC_ARM_REG_CPSR, 0x93)
        uc.reg_write(UC_ARM_REG_SP, saved_svc_sp)

    # -- the OS
    def swi(self, number):
        uc = self.uc
        r = [uc.reg_read(x) for x in REGS[:13]]
        base = number & ~X_BIT
        out = lambda i, v: uc.reg_write(REGS[i], v & 0xFFFFFFFF)
        if base == 0x591C5 and not self.bcm:      # (0.13) not a Pi
            return self._fail(number, 0x1E6, "SWI &591C5 not known")
        if base == 0x591C5:                       # BCMSupport_SendTempPropertyBuffer
            req = [self.word(r[0] + 4 * i) for i in range(9)]
            tag, a0, a1 = req[2], req[5], req[6]
            self.mailbox.append(tag)
            resp = list(req)
            if tag == 0x00010002:
                resp[5] = self.revision
            elif tag in (0x00030002,):
                resp[6] = 500000000 if getattr(self, "clock_set", 0) else 0
            elif tag == 0x00030004:
                resp[6] = 500000000
            elif tag == 0x00038002:
                self.clock_set = a1
                resp[6] = a1
            elif tag == 0x00030001:
                resp[6] = 0
            elif tag == 0x00030030:               # GET_DOMAIN_STATE
                resp[6] = getattr(self, "domain", 0) if a0 == 10 else 0
            elif tag == 0x00038030:               # SET_DOMAIN_STATE
                if a0 == 10:
                    self.domain = a1
                    self.domain_sets = getattr(self, "domain_sets", []) + [a1]
                resp[6] = a1
            for i, v in enumerate(resp):
                self.set_word(r[1] + 4 * i, v)
            return
        if base == 0x6B and (r[0] & 0xFF) == 2:   # (0.15) OS_MMUControl 2: ARMops
            i = (r[0] >> 8) & 0xFF
            if i not in (0, 1, 3) or not getattr(self, "armops", True):
                return self._fail(number, 0x1E6, "ARMop %d" % i)
            out(0, ARMOP + 8 * i)
            return
        if base == 0x1E and r[0] == 18:           # OS_Module lookup: nothing loaded
            return self._fail(number, 0x102, "Module '%s' not found" % self.cstr(r[1]))
        if base == 0x68:                          # OS_Memory
            reason = r[0] & 0xFF
            if reason == 13:
                self.mem13 += 1
                out(3, {0xFEC00000: V3D_LOG, 0xFE100000: PM_LOG}[r[1]])
                return
            if reason == 12:
                pages = (r[1] + 4095) // 4096
                align = max(1, (1 << r[2]) >> 12)
                ranged = bool(r[0] & (1 << 9))
                lo = r[4] | (r[5] << 32) if ranged else 0
                hi = r[6] | (r[7] << 32) if ranged else 0xFFFFFFFF
                self.mem12.append((r[0], r[1], lo, hi))
                if ranged and self.honour_range:
                    check(lo >= GB and hi >= lo, "a range from 1 GB: &%X-&%X" % (lo, hi))
                    start = (self.high_next + align - 1) // align * align
                    if (start + pages) > min(self.high_limit, (hi + 1) >> 12):
                        return self._fail(number, 0x1E7, "No contiguous memory in that range")
                    self.high_next = start + pages
                    out(3, start)
                    return
                start = (self.phys_next + align - 1) // align * align
                self.phys_next = start + pages
                out(3, start)
                return
            if reason == 65:
                um = getattr(self, "user_mem", None)
                if um and um[0] <= r[1] < um[0] + um[1]:
                    pa = 0x500000000 + (r[1] - um[0])     # the program's memory
                    out(0, pa & 0xFFFFFFFF)
                    out(1, pa >> 32)
                    return
                for a in self.areas.values():
                    i = (r[1] - a["base"]) // 4096
                    if 0 <= i < a["pages"] and i in a["mapped"]:
                        pa = a["claimed"][a["mapped"][i]] + (r[1] & 4095)
                        out(0, pa)
                        out(1, pa >> 32)
                        return
                return self._fail(number, 0xFC, "bad address")
        if base == 0x24:                          # (0.24) OS_SetVarVal
            if not hasattr(self, "vars"):
                self.vars = {}
            name = self.cstr(r[0])
            self.vars[name] = bytes(self.uc.mem_read(r[1], r[2])).decode("latin-1")
            self.var_log = getattr(self, "var_log", []) + [(name, self.vars[name])]
            return
        if base == 0x23:                          # OS_ReadVarVal
            name = self.cstr(r[0])
            val = getattr(self, "vars", {}).get(name)
            if val is None:
                return self._fail(number, 0x124, "System variable not found")
            self.uc.mem_write(r[1], val.encode())
            out(2, len(val))
            return
        if base == 0x08 and r[0] == 10:           # OS_File 10: save
            name = bytes(self.uc.mem_read(r[1], 64)).split(b"\r")[0].split(b"\0")[0].decode()
            self.saved = (name, r[2], r[5] - r[4])
            return
        if base == 0x66:                          # OS_DynamicArea
            return self.dynamic_area(number, r, out)
        if base == 0x42:                          # OS_ReadMonotonicTime
            self.time += 1
            out(0, self.time)
            return
        if base == 0x400F2:                       # Wimp_ReadSysInfo 5: no task
            out(0, 0)
            return
        if base == 0x7A:                          # OS_Hardware
            if r[8] == 1:
                check(r[9] == 3, "OS_Hardware 1 looks up HAL_IRQClear")
                out(0, HAL_CLEAR)
                out(1, 0x5B5B5B5B)
            return
        if base == 0x4B:                          # OS_ClaimDeviceVector
            check(r[0] == 10, "device 10 (the V3D)")
            self.vector = (r[1], r[2])
            return
        if base == 0x4C:
            check(self.vector == (r[1], r[2]), "the same vector is released")
            self.vector = None
            return
        if base == 0x39:                          # (0.16) OS_SWINumberFromString
            name = self.cstr(r[1])
            self.swi_lookups.append(name)
            if name == "RISC3D_Register" and self.risc3d_swi:
                out(0, self.risc3d_swi)
                return
            return self._fail(number, 0x1E4, "SWI name not known")
        if self.risc3d_swi and base == self.risc3d_swi:   # RISC3D_Register (a stand-in RISC3D)
            if r[0] == 0:
                self.risc3d = dict(entry=r[1], r12=r[2], name=self.cstr(r[3]),
                                   flags=r[4], ioctl=r[5], drm=self.cstr(r[6]))
            else:
                check(self.risc3d and self.risc3d["entry"] == r[1], "deregisters what it registered")
                self.risc3d = None
            return
        if base in (0xD4, 0xD8):                  # OS_ConvertHex8 / Cardinal4
            s = ("%08X" % r[0]) if base == 0xD4 else str(r[0])
            uc.mem_write(r[1], s.encode() + b"\0")
            out(0, r[1])
            return
        # (0.17) the desktop's side of the 2D copies
        if base == 0x1F and r[0] == 42:           # OS_Claim GraphicsV
            check(getattr(self, "gfxv", None) is None, "GraphicsV claimed once")
            self.gfxv = (r[1], r[2])
            return
        if base == 0x20 and r[0] == 42:           # OS_Release GraphicsV
            check(getattr(self, "gfxv", None) == (r[1], r[2]), "GraphicsV released as claimed")
            self.gfxv = None
            return
        if base == 0x31:                          # OS_ReadVduVariables
            i = 0
            while True:
                v = self.word(r[0] + 4 * i)
                if v == 0xFFFFFFFF:
                    break
                self.set_word(r[1] + 4 * i, self.vdu[v])
                i += 1
            return
        if base == 0x34 and r[9] == 42:           # OS_CallAVector GraphicsV
            self.gv9 = getattr(self, "gv9", 0) + 1
            check(r[4] == 9, "only GraphicsV 9 is called")
            out(0, self.fs[0])
            out(1, self.fs[1])
            out(4, 0)
            return
        if base == 0x65 and r[0] == 4:            # OS_ScreenMode 4: read
            out(1, self.screen_flags)
            return
        return super().swi(number)

    def dynamic_area(self, number, r, out):
        reason = r[0]
        if reason == 2 and r[1] == 2:             # (0.17) the screen's area
            out(4, self.screen_da_flags)
            return
        if reason == 0 and not r[4] & (1 << 20):
            # an ordinary area (the hang dump): memory mapped at once
            n = 300 + len(self.areas)
            size = (r[5] + 0xFFFFF) & ~0xFFFFF
            self.uc.mem_map(self.area_next, size)
            self.areas[n] = dict(base=self.area_next, pages=0, claimed={}, mapped={},
                                 name=self.cstr(r[8]), size=r[2])
            out(1, n)
            out(3, self.area_next)
            self.area_next += size
            return
        if reason == 0:
            check(r[4] & (1 << 20) and r[4] & (1 << 8), "a PMP with specific pages")
            n = 300 + len(self.areas)
            size = (r[5] + 0xFFFFF) & ~0xFFFFF
            if size > 0x10000000:
                return self._fail(number, 0x1E5, "Not enough address space")  # 1 GB won't fit here
            if getattr(self, "pmp_fail", False):
                return self._fail(number, 0x1E5, "Not enough address space")  # (0.24) none at all
            self.areas[n] = dict(base=self.area_next, pages=r[9], claimed={}, mapped={},
                                 name=self.cstr(r[8]))
            out(1, n)
            out(3, self.area_next)
            self.area_next += size
            return
        if reason == 1:
            a = self.areas.pop(r[1])
            check(not a["mapped"] and not a["claimed"], "an area is empty when removed (%s)" % a["name"])
            return
        a = self.areas[r[1]]
        for i in range(r[3]):
            e = r[2] + 12 * i
            idx, page, flags = self.word(e), self.word(e + 4), self.word(e + 8)
            if reason == 21:
                if page == 0xFFFFFFFF:
                    check(idx not in a["mapped"].values(), "a mapped page isn't released")
                    a["claimed"].pop(idx, None)
                else:
                    check(flags & (1 << 15), "pages are locked")
                    if page == 0xFFFFFFFE:      # the kernel picks: low memory
                        pa = 0x08000000 + ((idx * 7919) % 50000) * 4096
                        a["any"] = a.get("any", 0) + 1
                    else:
                        pa = page * 4096
                    a["claimed"][idx] = pa
                    a["claims"] = a.get("claims", 0) + 1     # (0.25)
            elif reason == 22:
                addr = a["base"] + idx * 4096
                if page == 0xFFFFFFFF:
                    if idx in a["mapped"]:
                        del a["mapped"][idx]
                        self.uc.mem_unmap(addr, 4096)
                else:
                    check(page in a["claimed"], "a mapped page was claimed")
                    a.setdefault("flags", {})[idx] = flags
                    a["mapped"][idx] = page
                    self.uc.mem_map(addr, 4096)
                    self.uc.mem_write(addr, b"\xAA" * 4096)
            else:
                return self._fail(number, 0x1E6, "OS_DynamicArea %d" % reason)


def initialise(mod, tail):
    """RISC OS passes the command tail in r10 (emu.py's initialise is for
    CMunge modules, which take it from r0)"""
    m = mod.m
    t = SCRATCH + 0x1000
    m.uc.mem_write(t, tail.encode() + b"\0")
    keep = {7: 0x7007, 8: 0x8008, 9: 0x9009, 11: 0xB00B}
    out, v, _ = m.call(mod.base + mod.init, {10: t, 12: m.pw, **keep})
    for i, val in keep.items():
        check(out[i] == val, "initialisation keeps r%d" % i)
    return m.cstr(out[0] + 4) if v else None


def main():
    data = open(sys.argv[1], "rb").read()
    m = Pi4()
    mod = Module(m, data, 0x20010000)
    check(mod.title == "V3DRender", "title")

    err = initialise(mod, "NoWFI")
    check(err is None, "initialises: %s" % err)
    if err:
        return
    check(m.grafx & (1 << 6), "the V3D is out of reset")
    check(m.asb[0x11008] == 0 and m.asb[0x1100C] == 0, "both ASB gates open")
    check(0x00038002 in m.mailbox, "the clock was set (it read 0)")
    pt = [a for a in m.areas.values() if a["name"] == "V3DRender page table"][0]
    bo_area = [a for a in m.areas.values() if a["name"] == "V3DRender buffers"][0]
    check(len(pt["mapped"]) == 1025, "the page table is mapped")
    pt_pa = pt["claimed"][0]
    check(m.hub.get(R["MMU_PT_PA_BASE"]) == pt_pa >> 12, "the MMU points at it")
    check(m.hub.get(R["MMU_CTL"], 0) & 1, "the MMU is on")
    check(m.core.get(R["CTL_INT_MSK_CLR"]) not in (None, 0), "interrupts unmasked")
    print("  areas: buffers %d pages at &%08X, page table at physical &%X"
          % (bo_area["pages"], bo_area["base"], pt_pa))
    check(bo_area["pages"] == 65536, "the buffer area is 256 MB by default (0.09)")
    check(pt_pa >= GB and all(pt["claimed"][i] == pt_pa + i * 4096 for i in range(1024)),
          "the page table is above 1 GB and contiguous (0.12)")
    check(m.mem12 and m.mem12[0][0] == 12 | 1 << 9 and m.mem12[0][2] == GB,
          "asked OS_Memory 12 for memory from 1 GB: %s" % m.mem12[:1])

    out, e = mod.swi(0)
    check(e is None and out[1] == 42 and out[2] == 0x000E1124, "V3DRender_Info")
    check(out[0] == 25 and out[8] != 0, "version 0.25, statistics in r8")
    check(struct.unpack("<I", data[0x1C:0x20])[0] == 0x5AFC0, "(0.16) the allocated SWI chunk, &5AFC0")
    stats_addr = out[8]

    # RISC3D: registered as a DRM device; its calls open, describe, close
    d = m.risc3d
    check(d is not None and d["flags"] == 9 and d["ioctl"] == 0x5AFC3 and d["drm"] == "v3d"
          and "RISC3D_Register" in m.swi_lookups,
          "registered with RISC3D as a DRM device (%s)" % d)
    if d:
        o, v, _ = m.call(d["entry"], {0: 0, 12: d["r12"]})
        check(not v and o[0] >= 1 and o[1] == 0 and o[2] == 0, "RISC3D open -> a file")
        rf = o[0]
        o, v, _ = m.call(d["entry"], {0: 3, 12: d["r12"]})
        check(not v and o[0] == 0, "RISC3D describe -> 0, on a GPU")
        # (0.20) reason 4: the hardware, label TAB value LF, for RISC3DSetup
        B = SCRATCH + 0xB000
        o, v, _ = m.call(d["entry"], {0: 4, 1: B, 2: 0, 12: d["r12"]})
        need = o[2]
        check(not v and 100 < need < 600, "reason 4 with no buffer gives the size (%d)" % need)
        o, v, _ = m.call(d["entry"], {0: 4, 1: B, 2: need, 12: d["r12"]})
        txt = bytes(m.uc.mem_read(B, need))
        check(not v and o[2] == need and txt[-1] == 0, "reason 4 fills it, ended by 0")
        lines = txt[:-1].decode().split("\n")
        check(lines[-1] == "" and all("\t" in l for l in lines[:-1]) and len(lines) - 1 <= 8
              and lines[0] == "GPU\tV3D 4.2, revision 14 (the Pi 4's VideoCore VI)"
              and lines[1].startswith("Clock\t") and lines[2].startswith("GPU memory\t")
              and "Largest image\t4096 x 4096" in txt.decode(),
              "reason 4's lines: %s" % lines)
        print("  reason 4:\n    " + "\n    ".join(lines[:-1]))
        m.uc.mem_write(B, b"\xAA" * 64)
        o, v, _ = m.call(d["entry"], {0: 4, 1: B, 2: 20, 12: d["r12"]})
        cut = bytes(m.uc.mem_read(B, 21))
        check(not v and o[2] == need and cut[19] == 0 and cut[20] == 0xAA and cut[:19] == txt[:19],
              "reason 4 into 20 bytes: 19 and a 0, nothing past it")
        o, v, _ = m.call(d["entry"], {0: 2, 1: rf, 12: d["r12"]})
        check(v, "RISC3D sync refused (no command stream)")
        o, v, _ = m.call(d["entry"], {0: 1, 1: rf, 12: d["r12"]})
        check(not v, "RISC3D close")
    out, e = mod.swi(1, {0: 0})
    check(e is None and out[0] == 1, "V3DRender_Open -> file 1")
    f = out[0]
    out, e = mod.swi(2, {0: 9})
    check(e is not None and "Not a V3DRender file" in e, "a bad file is refused")

    A = SCRATCH + 0x8000

    def ioctl(req, payload):
        m.uc.mem_write(A, payload)
        out, e = mod.swi(3, {0: f, 1: req, 2: A})
        check(e is None, "ioctl raised no error: %s" % e)
        res = out[0] - (1 << 32) if out[0] & 0x80000000 else out[0]
        return res, bytes(m.uc.mem_read(A, len(payload)))

    ret, b = ioctl(R["IOCTL_GET_PARAM"], struct.pack("<IIQ", R["PARAM_HUB_IDENT1"], 0, 0))
    check(ret == 0 and struct.unpack("<IIQ", b)[2] == 0x000E1124, "GET_PARAM through the SWI")

    ret, b = ioctl(R["IOCTL_CREATE_BO"], struct.pack("<IIII", 20000, 0, 0, 0))
    _, _, handle, offset = struct.unpack("<IIII", b)
    check(ret == 0 and handle == 1 and offset and offset % 4096 == 0, "CREATE_BO")
    ret, b = ioctl(R["IOCTL_MMAP_BO"], struct.pack("<IIQ", handle, 0, 0))
    addr = struct.unpack("<IIQ", b)[2]
    check(ret == 0 and addr == bo_area["base"] + offset, "MMAP_BO gives the BO's address")
    check(bytes(m.uc.mem_read(addr, 16)) == b"\0" * 16, "a new BO is zeroed")
    # the PTE for its first page, read from the page table's memory
    pte = m.word(pt["base"] + (offset >> 12) * 4)
    pa = bo_area["claimed"][bo_area["mapped"][offset >> 12]]
    check(pte & (1 << 28) and (pte & 0xFFFFFF) == pa >> 12, "its PTE (&%08X)" % pte)
    pas = [bo_area["claimed"][bo_area["mapped"][(offset >> 12) + i]] for i in range(5)]
    check(all(x >= GB for x in pas) and pas == [pas[0] + 4096 * i for i in range(5)],
          "a small BO's pages are above 1 GB, in a run (0.12)")
    check(not bo_area.get("any"), "none was left to the kernel")

    ret, b = ioctl(R["IOCTL_SYNCOBJ_CREATE"], struct.pack("<II", 0, 0))
    sync = struct.unpack("<II", b)[0]
    check(ret == 0 and sync == 1, "SYNCOBJ_CREATE")
    hlist = SCRATCH + 0x9000
    m.uc.mem_write(hlist, struct.pack("<I", handle))
    cl = struct.pack("<9I4xQ4IQ", 0x1000, 0x1040, 0x2000, 0x2040, 0, 0, sync,
                     0x3000, 0x8000, hlist, 1, 0, 0, 0, 0)
    check(len(cl) == R["SIZEOF_SUBMIT_CL"], "submit_cl layout")
    ret, _ = ioctl(R["IOCTL_SUBMIT_CL"], cl)
    check(ret == 0, "SUBMIT_CL")
    check(m.started == [("bin", 0x1000, 0x1040)], "the bin job started: %s" % m.started)

    def wait(timeout_ns):
        m.uc.mem_write(SCRATCH + 0x9100, struct.pack("<I", sync))
        return ioctl(R["IOCTL_SYNCOBJ_WAIT"],
                     struct.pack("<QqIIII", SCRATCH + 0x9100, timeout_ns, 1, 1, 0, 0))[0]

    check(wait(0) == -62, "not done yet (ETIME)")
    m.finish(R["INT_FLDONE"])
    check(m.hal_clears == 1, "HAL_IRQClear called from the interrupt")
    check(m.started[-1] == ("render", 0x2000, 0x2040), "the render job started from the interrupt")
    m.finish(R["INT_FRDONE"])
    check(m.hal_clears == 2, "and again")
    check(wait(0) == 0, "done")
    ret, _ = ioctl(R["IOCTL_WAIT_BO"], struct.pack("<IIQ", handle, 0, 0))
    check(ret == 0, "WAIT_BO")
    ret, _ = ioctl(R["IOCTL_GEM_CLOSE"], struct.pack("<II", handle, 0))
    check(ret == 0 and (offset >> 12) in bo_area["mapped"] and m.word(pt["base"] + (offset >> 12) * 4) == 0,
          "(0.25) GEM_CLOSE keeps the pages for the next BO, but not in the GPU's MMU")
    m.uc.mem_write(addr, b"\x55" * 16)
    claims0 = bo_area.get("claims", 0)
    ret, b = ioctl(R["IOCTL_CREATE_BO"], struct.pack("<IIII", 20000, 0, 0, 0))
    _, _, handle, off2 = struct.unpack("<IIII", b)
    check(ret == 0 and off2 == offset and bo_area.get("claims", 0) == claims0
          and bytes(m.uc.mem_read(addr, 16)) == b"\0" * 16,
          "(0.25) the next BO of its size gets those pages, zeroed, with no claim")
    ret, _ = ioctl(R["IOCTL_GEM_CLOSE"], struct.pack("<II", handle, 0))
    check(m.core.get(R["CTL_INT_STS"], 0) == 0, "the interrupts were acknowledged")

    check(all(fl & (1 << 5) for fl in bo_area.get("flags", {}).values()),
          "ordinary BOs are mapped not cacheable")

    # (0.11) a cached BO, and the program's memory imported, through a job
    ret, b = ioctl(R["IOCTL_CREATE_BO"], struct.pack("<IIII", 8192, 0x80000000, 0, 0))
    ch, coff = struct.unpack("<IIII", b)[2], struct.unpack("<IIII", b)[3]
    check(ret == 0 and not bo_area["flags"][coff >> 12] & (1 << 5)
          and bo_area["flags"][coff >> 12] & (1 << 15), "a cached BO: mapped cacheable, locked")
    ret, _ = ioctl(R["IOCTL_CREATE_BO"], struct.pack("<IIII", 4096, 1, 0, 0))
    check(ret == -22, "an unknown CREATE_BO flag is refused")
    UMEM = 0x48000000
    m.uc.mem_map(UMEM, 0x10000)
    m.user_mem = (UMEM, 0x10000)
    ret, b = ioctl(R["IOCTL_IMPORT_MEMORY"], struct.pack("<QIIII", UMEM + 0x1000, 0x3000, 0, 0, 0))
    ih, ioff = struct.unpack("<QIIII", b)[2], struct.unpack("<QIIII", b)[3]
    check(ret == 0 and ih and ioff, "IMPORT_MEMORY: handle %d at GPU &%X" % (ih, ioff))
    pte = m.word(pt["base"] + (ioff >> 12) * 4)
    check((pte & 0xFFFFFF) == (0x500001000 >> 12) & 0xFFFFFF and pte & (1 << 28),
          "the GPU's MMU points at the program's memory: &%08X" % pte)
    ret, b = ioctl(R["IOCTL_MMAP_BO"], struct.pack("<IIQ", ih, 0, 0))
    check(ret == 0 and struct.unpack("<IIQ", b)[2] == UMEM + 0x1000, "MMAP_BO gives its own address")
    check((ioff >> 12) not in bo_area["mapped"], "and no buffer-area page was taken for it")
    # (0.23) the OS moves one of the imported pages: no job starts between
    # Service_PagesUnsafe64 and Service_PagesSafe64, then the GPU's MMU
    # points at the page's new place. The 32-bit pair inside is ignored.
    PB = SCRATCH + 0xC000
    old, new = 0x500002000, 0x600005000
    m.uc.mem_write(PB, struct.pack("<5I", 0x500002, UMEM + 0x2000, 0, old & 0xFFFFFFFF, old >> 32))
    m.uc.mem_write(PB + 20, struct.pack("<5I", 0x600005, UMEM + 0x2000, 0, new & 0xFFFFFFFF, new >> 32))
    m.call(mod.base + mod.service, {1: 0xE3, 2: PB, 3: 1, 12: m.pw})
    m.uc.mem_write(PB + 40, struct.pack("<3I", 0x1234, 0x9000, 0x1234000))
    m.call(mod.base + mod.service, {1: 0x8E, 2: PB + 40, 3: 1, 12: m.pw})
    n0 = len(m.started)
    m.uc.mem_write(hlist, struct.pack("<II", ch, ih))
    cl = struct.pack("<9I4xQ4IQ", 0x1000, 0x1040, 0x2000, 0x2040, 0, 0, sync,
                     0x3000, 0x8000, hlist, 2, 0, 0, 0, 0)
    ret, _ = ioctl(R["IOCTL_SUBMIT_CL"], cl)
    check(ret == 0 and len(m.started) == n0, "pages moving: the job waits (%d started)" % (len(m.started) - n0))
    m.call(mod.base + mod.service, {1: 0x8F, 2: 1, 3: PB + 40, 4: PB + 40, 12: m.pw})
    check(len(m.started) == n0, "the 32-bit PagesSafe inside the 64-bit pair changes nothing")
    m.call(mod.base + mod.service, {1: 0xE4, 2: 1, 3: PB, 4: PB + 20, 12: m.pw})
    pte = m.word(pt["base"] + ((ioff >> 12) + 1) * 4)
    check((pte & 0xFFFFFF) == (new >> 12) & 0xFFFFFF and pte & (1 << 28),
          "the moved page's PTE follows it (&%08X)" % pte)
    pte0 = m.word(pt["base"] + (ioff >> 12) * 4)
    check((pte0 & 0xFFFFFF) == (0x500001000 >> 12) & 0xFFFFFF, "the others stay")
    check(len(m.started) > n0, "PagesSafe64: the job starts")
    m.output = ""
    mod.command("V3DRender")
    check("Pages moved by the OS: 1 imported pages, 1 times" in m.output, "*V3DRender counts the move")
    # pages nobody imported: no hold at all
    m.uc.mem_write(PB + 60, struct.pack("<5I", 0x7000, 0x9000, 0, 0x7000000, 0))
    m.call(mod.base + mod.service, {1: 0xE3, 2: PB + 60, 3: 1, 12: m.pw})
    m.call(mod.base + mod.service, {1: 0xE4, 2: 1, 3: PB + 60, 4: PB + 60, 12: m.pw})
    check("Pages moved by the OS: 1 imported pages, 1 times" in (m.output if False else m.output),
          "pages not imported: nothing held")
    m.finish(R["INT_FLDONE"])
    m.finish(R["INT_FRDONE"])
    check(wait(0) == 0, "done (and invalidated before the wait returned)")
    for h in (ch, ih):
        ret, _ = ioctl(R["IOCTL_GEM_CLOSE"], struct.pack("<II", h, 0))
        check(ret == 0, "GEM_CLOSE %d" % h)
    check(not m.word(pt["base"] + (ioff >> 12) * 4) & (1 << 28), "the import is out of the MMU")
    check(m.armop_calls == [0, 0], "small BOs: by address, not the whole cache")

    # (0.15) a 1 MB cached BO: the kernel's whole-cache ARMops instead
    ret, b = ioctl(R["IOCTL_CREATE_BO"], struct.pack("<IIII", 1 << 20, 0x80000000, 0, 0))
    wh = struct.unpack("<IIII", b)[2]
    m.uc.mem_write(hlist, struct.pack("<I", wh))
    cl = struct.pack("<9I4xQ4IQ", 0x1000, 0x1040, 0x2000, 0x2040, 0, 0, sync,
                     0x3000, 0x8000, hlist, 1, 0, 0, 0, 0)
    ret, _ = ioctl(R["IOCTL_SUBMIT_CL"], cl)
    check(ret == 0 and m.armop_calls == [0, 2],
          "a 1 MB cached BO: Cache_CleanAll for its zeroes and at submit %s" % m.armop_calls)
    m.finish(R["INT_FLDONE"])
    m.finish(R["INT_FRDONE"])
    check(wait(0) == 0 and m.armop_calls == [1, 2],
          "and Cache_CleanInvalidateAll after the job %s" % m.armop_calls)
    ret, _ = ioctl(R["IOCTL_GEM_CLOSE"], struct.pack("<II", wh, 0))

    # (0.12) a 1 MB BO above 1 GB; then with nothing left there, the kernel's pages
    ret, b = ioctl(R["IOCTL_CREATE_BO"], struct.pack("<IIII", 1 << 20, 0, 0, 0))
    bh, boff = struct.unpack("<IIII", b)[2], struct.unpack("<IIII", b)[3]
    pas = [bo_area["claimed"][bo_area["mapped"][(boff >> 12) + i]] for i in range(256)]
    check(ret == 0 and all(x >= GB for x in pas) and pas == [pas[0] + 4096 * i for i in range(256)],
          "a 1 MB BO: above 1 GB, contiguous")
    big_pte = m.word(pt["base"] + (boff >> 12) * 4)
    check(boff & 0xFFFFF == 0 and pas[0] & 0xFFFFF == 0 and big_pte & (1 << 31),
          "(0.13) and on a 1 MB GPU address and 1 MB physical: a 1 MB entry (&%08X)" % big_pte)
    ret, b = ioctl(R["IOCTL_CREATE_BO"], struct.pack("<IIII", 64 * 1024, 0, 0, 0))
    kh, koff = struct.unpack("<IIII", b)[2], struct.unpack("<IIII", b)[3]
    kpte = m.word(pt["base"] + (koff >> 12) * 4)
    check(ret == 0 and koff & 0xFFFF == 0 and kpte & (1 << 30) and not kpte & (1 << 31),
          "a 64 KB BO: a 64 KB entry (&%08X at &%X)" % (kpte, koff))
    ret, _ = ioctl(R["IOCTL_GEM_CLOSE"], struct.pack("<II", kh, 0))
    out, e = mod.swi(5, {0: 2, 1: 0x00777000})      # (0.13) Debug 2: place the next BO
    ret, b = ioctl(R["IOCTL_CREATE_BO"], struct.pack("<IIII", 66 * 4096, 0, 0, 0))
    ph, poff = struct.unpack("<IIII", b)[2], struct.unpack("<IIII", b)[3]
    check(e is None and ret == 0 and poff == 0x00777000, "V3DRender_Debug 2 places a BO (&%X)" % poff)
    ret, _ = ioctl(R["IOCTL_GEM_CLOSE"], struct.pack("<II", ph, 0))
    m.high_limit = m.high_next + 40                 # room for 40 pages only
    ret, b = ioctl(R["IOCTL_CREATE_BO"], struct.pack("<IIII", 100 * 4096, 0, 0, 0))
    lh, loff = struct.unpack("<IIII", b)[2], struct.unpack("<IIII", b)[3]
    pas = [bo_area["claimed"][bo_area["mapped"][(loff >> 12) + i]] for i in range(100)]
    hi_n = sum(x >= GB for x in pas)
    check(ret == 0 and 16 <= hi_n <= 40 and bo_area.get("any") == 100 - hi_n
          and all(x >= GB for x in pas[:hi_n]),
          "high memory short: %d pages above 1 GB, the rest from the kernel" % hi_n)
    check(bytes(m.uc.mem_read(bo_area["base"] + loff + 99 * 4096, 16)) == b"\0" * 16,
          "and the BO is still whole and zeroed")
    m.output = ""
    err = mod.command("V3DRender")
    used = re.search(r"in use \(KB\): +(\d+)", m.output)
    above = re.search(r"above 1 GB \(KB\): +(\d+)", m.output)
    check(err is None and used and above
          and int(above.group(1)) + (100 - hi_n) * 4 == int(used.group(1))
          and "below 1 GB (KB):     %d" % ((100 - hi_n) * 4) in m.output
          and "Page table at:         &%08X (above 1 GB)" % pt_pa in m.output
          and "in 1 MB entries (KB):1024" in m.output and "GPU time (ms):" in m.output
          and "jobs run again after:0" in m.output
          and "ARM clock (MHz):" in m.output
          and "whole cache 3 times (from 1024 KB)" in m.output,
          "*V3DRender says where the buffers are: %d high\n%s" % (hi_n, m.output))
    for h in (bh, lh):
        ret, _ = ioctl(R["IOCTL_GEM_CLOSE"], struct.pack("<II", h, 0))
        check(ret == 0, "GEM_CLOSE %d" % h)
    m.high_limit = 0x100000

    m.output = ""
    err = mod.command("V3DRender")
    check(err is None and "Render jobs:           3" in m.output, "*V3DRender")
    check(m.word(stats_addr + 4 * 1) == 3 and m.word(stats_addr + 4 * 5) == 6,
          "the statistics block: 3 render jobs, 6 interrupts")
    print("  " + m.output.replace("\n", "\n  ").rstrip())

    check(getattr(m, "domain_sets", None) == [1], "the firmware's V3D domain turned on at load (0.08)")
    m.output = ""
    err = mod.command("V3DPower")
    check(err is None and "power domain: on" in m.output and "PM_GRAFX" in m.output,
          "*V3DPower: %s" % m.output.strip().replace("\n", " / "))

    # a bin job that hangs on a list in a BO: *V3DRender shows where (0.05)
    ret, b = ioctl(R["IOCTL_CREATE_BO"], struct.pack("<IIII", 4096, 0, 0, 0))
    h2, off2 = struct.unpack("<IIII", b)[2], struct.unpack("<IIII", b)[3]
    m.uc.mem_write(bo_area["base"] + off2, struct.pack("<8I", *range(0x11, 0x19)))
    m.uc.mem_write(hlist, struct.pack("<I", h2))
    cl = struct.pack("<9I4xQ4IQ", off2, off2 + 0x20, off2, off2 + 0x20, 0, 0, sync,
                     0x3000, 0x8000, hlist, 1, 0, 0, 0, 0)
    ret, _ = ioctl(R["IOCTL_SUBMIT_CL"], cl)
    check(ret == 0 and m.started[-1] == ("bin", off2, off2 + 0x20), "a bin job that won't end")
    m.core[R["CT0CA"]] = off2 + 0x10
    for i in range(4):
        m.time += 60
        wait(0)
    check(m.word(stats_addr + 4 * 13) >= 1, "found hung and reset")
    m.output = ""
    err = mod.command("V3DRender")
    check(err is None and "First hang: bin job, list &%08X" % off2 in m.output
          and "at &%08X: &00000011 &00000012" % off2 in m.output, "*V3DRender shows the hang")
    print("  " + m.output.replace("\n", "\n  ").rstrip())
    dump = [a for a in m.areas.values() if a["name"] == "V3DRender hang dump"]
    check(len(dump) == 1, "the hang dump area")
    if dump:
        d = dump[0]
        w = struct.unpack("<12I", bytes(m.uc.mem_read(d["base"], 48)))
        check(w[0] == 0x44443356 and w[1] == 0x31504d55 and w[3] == 0 and w[6] == off2
              and w[11] == 1, "the dump's header: %s" % [hex(x) for x in w])
        # after the hang words: the BO's address, size, then its contents
        hw = 12 + 32
        bo = struct.unpack("<4I", bytes(m.uc.mem_read(d["base"] + hw * 4, 16)))
        check(bo[0] == off2 and bo[1] == 4096 and bo[2] == 0x11 and bo[3] == 0x12,
              "the dump holds the BO: %s" % [hex(x) for x in bo])
        m.files = {}
        err = mod.command("V3DDump", "RAM::0.$.Dump", 1)
        check(err is None and m.saved and m.saved[0] == "RAM::0.$.Dump" and m.saved[1] == 0xFFD
              and m.saved[2] == d["size"], "*V3DDump saves it: %s" % (m.saved,))

    m.output = ""
    mod.command("V3DRender")
    check("Buffers made:" in m.output and " all from kept pages" in m.output,
          "(0.25) *V3DRender shows buffers made from the pool")
    out, e = mod.swi(2, {0: f})
    check(e is None, "V3DRender_Close")
    m.output = ""
    mod.command("V3DRender")
    check("kept for reuse (KB): 0 (up to 16384," in m.output,
          "(0.25) the last program closed: nothing kept (%s)"
          % [l for l in m.output.splitlines() if "kept" in l])
    err = mod.command("V3DRender", "pool 4", 1)
    m.output = ""
    mod.command("V3DRender")
    check(err is None and "(up to 4096," in m.output, "(0.25) *V3DRender pool 4")
    err = mod.command("V3DRender", "pool", 1)
    check(err is not None, "(0.25) *V3DRender pool needs a number")
    mod.command("V3DRender", "pool 16", 1)
    # (0.10) *V3DReset: the GPU reset; "all": every file closed
    out, e = mod.swi(1, {0: 0})
    check(e is None, "a file left open by a program that stopped")
    m.output = ""
    err = mod.command("V3DReset")
    check(err is None and "reset" in m.output and "Files closed" not in m.output,
          "*V3DReset: %s" % m.output.strip())
    out, e = mod.swi(0)
    check(out[6] == 1, "the file is still open")
    m.output = ""
    err = mod.command("V3DReset", "all", 1)
    check(err is None and "Files closed: 1" in m.output, "*V3DReset all: %s" % m.output.strip())
    out, e = mod.swi(0)
    check(out[6] == 0, "no files open after *V3DReset all")
    out, e = mod.swi(4, {0: 1})
    check(e is None, "V3DRender_Reset with r0 = 1")

    m.output = ""
    err = mod.command("V3DPower", "cycle", 1)
    check(err is None and "off and on again" in m.output and m.domain_sets[-2:] == [0, 1],
          "*V3DPower cycle: %s" % m.output.strip().replace("\n", " / "))
    err = mod.finalise()
    check(err is None, "finalises: %s" % err)
    check(m.vector is None, "the device vector is released")
    check(m.risc3d is None, "deregistered from RISC3D")
    check(not m.areas, "both areas removed")
    check(m.hub.get(R["MMU_CTL"]) == 0, "the MMU left off")
    print("  most SVC stack used: %d bytes" % m.stack_used())

    # loaded again on a running V3D: no reset pulse
    m2 = Pi4()
    m2.grafx = 1 << 6
    m2.asb = {0x11008: 0, 0x1100C: 0}
    mod2 = Module(m2, data, 0x20010000)
    check(initialise(mod2, "NoWFI") is None, "loads on a running V3D")
    check(not any(o == 0x11008 or o == 0x1100C for o, _ in m2.writes), "and doesn't touch the gates")
    check(mod2.finalise() is None, "finalises")

    # the buffer area's size (0.09): the Size option, V3DRender$Size, fallback
    def area_after(tail, var=None):
        m3 = Pi4()
        m3.grafx = 1 << 6
        m3.asb = {0x11008: 0, 0x1100C: 0}
        if var is not None:
            m3.vars = {"V3DRender$Size": var}
        mod3 = Module(m3, data, 0x20010000)
        err = initialise(mod3, tail)
        pages = [a["pages"] for a in m3.areas.values() if a["name"] == "V3DRender buffers"]
        mod3.finalise()
        return err, pages[0] if pages else None
    check(area_after("NoWFI Size 64") == (None, 16384), "Size 64: a 64 MB area")
    check(area_after("Size=128 NoWFI") == (None, 32768), "Size=128: a 128 MB area")
    check(area_after("", "32") == (None, 8192), "V3DRender$Size 32: a 32 MB area")
    check(area_after("Size 64", "32") == (None, 16384), "the option over the variable")
    check(area_after("Size 1024") == (None, 65536), "Size 1024 where only 256 MB fits: 256 MB")

    # (0.12) a 1 GB Pi (nothing above 1 GB), and a RISC OS older than 5.29
    for what, setup in (("a 1 GB Pi", lambda m: setattr(m, "high_limit", GB >> 12)),
                        ("RISC OS before 5.29", lambda m: setattr(m, "honour_range", False))):
        m4 = Pi4()
        setup(m4)
        mod4 = Module(m4, data, 0x20010000)
        err = initialise(mod4, "NoWFI")
        pt4 = [a for a in m4.areas.values() if a["name"] == "V3DRender page table"]
        ok = err is None and pt4 and pt4[0]["claimed"][0] < GB
        if ok:
            out, e = mod4.swi(1, {0: 0})
            A4 = SCRATCH + 0x8000
            m4.uc.mem_write(A4, struct.pack("<IIII", 64 * 4096, 0, 0, 0))
            o2, e2 = mod4.swi(3, {0: out[0], 1: R["IOCTL_CREATE_BO"], 2: A4})
            ok = e2 is None and o2[0] == 0
            mod4.swi(2, {0: out[0]})
        check(ok, "%s: loads, page table below 1 GB, a BO still made" % what)
        mod4.finalise()

    # (0.14) after a job, imported and cached BOs are cleaned and
    # invalidated (DCCIMVAC), never just invalidated (DCIMVAC): the CPU
    # may have written elsewhere in a cached screen meanwhile
    words = struct.unpack("<%dI" % (len(data) // 4), data[:len(data) // 4 * 4])
    check(0xEE070F3E in words and 0xEE070F36 not in words,
          "cache maintenance after jobs is clean+invalidate, not invalidate")

    # (0.13) RISC3D's package puts V3DRender on every machine: it must
    # refuse to start wherever there's no V3D 4.2, touching nothing
    def refuses(what, setup, errnum, hardware_ok=False):
        m5 = Pi4()
        setup(m5)
        mod5 = Module(m5, data, 0x20010000)
        t = SCRATCH + 0x1000
        m5.uc.mem_write(t, b"\0")
        out, v, _ = m5.call(mod5.base + mod5.init, {10: t, 12: m5.pw})
        num = m5.word(out[0]) if v else 0
        ok = v and num == errnum and not m5.areas and m5.vector is None
        if not hardware_ok:
            ok = ok and m5.touched == 0 and m5.mem13 == 0 \
                and all(t == 0x00010002 for t in m5.mailbox)
        check(ok, "%s: refused with &%X (%s), nothing claimed%s" % (
            what, num, m5.cstr(out[0] + 4) if v else "no error",
            "" if hardware_ok else ", no hardware touched"))
    refuses("a Pi 3B+", lambda m: setattr(m, "revision", 0xA020D3), 0x823E01)
    refuses("a Pi 1 (old-style revision)", lambda m: setattr(m, "revision", 0x000E), 0x823E01)
    refuses("not a Pi (no BCMSupport)", lambda m: setattr(m, "bcm", False), 0x823E01)
    def no_v3d(m):
        m.hub, m.core = {}, {}
    refuses("a BCM2711 whose V3D doesn't answer", no_v3d, 0x823E03, hardware_ok=True)


def gfxv(m, r):
    """GraphicsV as the kernel calls a claimant: SVC mode, IRQs on, the
    return address for a claim stacked, lr to pass the call on. Returns
    ("claimed" | "passed", r0-r12)."""
    from emu import STACK_BASE, STACK_SIZE
    uc = m.uc
    entry, r12 = m.gfxv
    CLAIMED = RETURN + 0x300
    sp = STACK_BASE + STACK_SIZE - 4
    m.set_word(sp, CLAIMED)
    for i, reg in enumerate(REGS[:13]):
        uc.reg_write(reg, r.get(i, 0x66660000 + i))
    uc.reg_write(REGS[12], r12)
    uc.reg_write(UC_ARM_REG_SP, sp)
    uc.reg_write(UC_ARM_REG_LR, RETURN)
    uc.reg_write(UC_ARM_REG_CPSR, 0x13)
    stop = uc.hook_add(1 << 2, lambda u, a, s, d: u.emu_stop(), begin=RETURN, end=RETURN + 0x3FF)
    m.fault = None
    try:
        uc.emu_start(entry, 0, count=5_000_000)
    finally:
        uc.hook_del(stop)
    if m.fault:
        raise EmuError(m.fault)
    pc, nsp = uc.reg_read(UC_ARM_REG_PC), uc.reg_read(UC_ARM_REG_SP)
    o = [uc.reg_read(x) for x in REGS[:13]]
    if pc == CLAIMED and nsp == sp + 4:
        return "claimed", o
    if pc == RETURN and nsp == sp:
        return "passed", o
    raise EmuError("GraphicsV returned to &%08X, sp &%08X" % (pc, nsp))


def test_2d(data):
    """(0.17) the GPU's desktop copies: GraphicsV claimed, copies taken or
    left to the CPU, the framestore mapped, the commands and services"""
    FS = 0x3E000000
    m = Pi4()
    m.vdu = {9: 5, 6: 7680, 11: 1919, 12: 1079, 0x94: 0xFE000000, 0x95: 0xFE000000, 0x96: 7680 * 1080}
    m.fs = (FS, 16 << 20)
    m.screen_flags = 0
    m.screen_da_flags = 1 << 5 | 1 << 4                 # not cacheable, not bufferable
    m.vars = {"V3DRender$2D": "async"}
    mod = Module(m, data, 0x20010000)
    check(initialise(mod, "NoWFI") is None, "loads with V3DRender$2D async")
    check(getattr(m, "gfxv", None) is not None, "GraphicsV claimed")
    if not getattr(m, "gfxv", None):
        return
    bo_area = [a for a in m.areas.values() if a["name"] == "V3DRender buffers"][0]
    pt = [a for a in m.areas.values() if a["name"] == "V3DRender page table"][0]
    P = SCRATCH + 0xA000

    def copy(sx, sy, dx, dy, w, h, flags=0, head=0):
        m.uc.mem_write(P, struct.pack("<6I", sx, sy, dx, dy, w - 1, h - 1))
        return gfxv(m, {0: flags, 1: 1, 2: P, 4: 13 | head << 16})

    how, o = gfxv(m, {4: 8})
    check(how == "passed" and o[4] == 8, "other reasons are passed on untouched")
    how, o = copy(0, 0, 100, 0, 10, 10)
    check(how == "passed" and o[4] == 13, "a small copy is left to the CPU")
    how, _ = copy(0, 0, 400, 0, 200, 100, head=1)
    check(how == "passed", "head 1 is left alone")
    n = len(m.started)
    how, o = copy(40, 1079 - 109, 800, 1079 - 399, 200, 100)
    check(how == "claimed" and o[4] == 0, "a 200x100 copy is claimed, r4 = 0")
    check(len(m.started) == n + 1 and m.started[-1][0] == "render", "as a render job (no binning)")
    if len(m.started) > n:
        rcl = m.started[-1][1]
        # the list in the 2D file's buffer: header, then the first tile's load
        i = (rcl >> 12)
        page = bo_area["mapped"].get(i)
        check(page is not None, "the render list is in a buffer")
        if page is not None:
            va = bo_area["base"] + (rcl & ~0xFFF) + (rcl & 0xFFF)
            b = bytes(m.uc.mem_read(va, 96))
            w, h = struct.unpack_from("<HH", b, 2)
            check(b[0] == 121 and (w, h) == (200, 100), "TRMC 200x100 (%d, %d)" % (w, h))
            ld = 52 + 4                             # after the header and the first coordinates
            check(b[52] == 124 and b[ld] == 30, "a tile, then its load")
            addr = struct.unpack_from("<I", b, ld + 9)[0]
            pte = m.word(pt["base"] + (addr >> 12) * 4)
            want = FS + 10 * 7680 + 40 * 4
            check(pte & (1 << 28) and ((pte & 0xFFFFFF) << 12) + (addr & 0xFFF) == want,
                  "the load reads the framestore at &%X (PTE &%08X)" % (want, pte))
    m.finish(R["INT_FRDONE"])
    how, _ = gfxv(m, {0: 1, 1: 0, 4: 13})
    check(how == "passed", "a no-op with synchronise: waited, passed on")
    how, _ = copy(0, 0, 0, 1, 200, 100)
    check(how == "claimed", "a scroll by one row is claimed")
    m.finish(R["INT_FRDONE"])
    # left to the CPU: hardware acceleration disabled, a cached screen
    m.screen_flags = 4
    how, _ = copy(0, 0, 400, 0, 200, 100)
    check(how == "passed", "OS_ScreenMode 4 bit 2: left to the CPU")
    m.screen_flags = 0
    m.screen_da_flags = 0                             # (checked again at a bank change)
    gfxv(m, {0: 3, 1: FS, 4: 6})                      # GraphicsV 6: VDU writes at the start
    how, _ = copy(0, 0, 400, 0, 200, 100)
    check(how == "passed", "a cached screen: left to the CPU")
    m.screen_da_flags = 1 << 5
    gfxv(m, {0: 3, 1: FS + 7680 * 1080, 4: 6})        # the second bank
    n = len(m.started)
    how, _ = copy(0, 0, 400, 0, 200, 100)
    check(how == "claimed", "the second bank is used")
    m.finish(R["INT_FRDONE"])
    # (0.18) flipping banks: no GraphicsV 9 or new mapping, just the bank
    gv9 = m.gv9
    for i in range(3):
        gfxv(m, {0: 3, 1: FS + (i & 1) * 7680 * 1080, 4: 6})
        how, _ = copy(0, 0, 400, 0, 200, 100)
        check(how == "claimed", "bank flip %d: claimed" % i)
        if how == "claimed":
            va = bo_area["base"] + m.started[-1][1]
            addr = struct.unpack_from("<I", bytes(m.uc.mem_read(va + 52 + 4 + 9, 4)))[0]
            pte = m.word(pt["base"] + (addr >> 12) * 4)
            want = FS + (i & 1) * 7680 * 1080 + 980 * 7680
            check(((pte & 0xFFFFFF) << 12) + (addr & 0xFFF) == want, "bank flip %d reads its bank" % i)
        m.finish(R["INT_FRDONE"])
    check(m.gv9 == gv9, "no GraphicsV 9 for a bank flip (%d)" % (m.gv9 - gv9))
    gfxv(m, {0: 3, 1: FS + (16 << 20) - 100, 4: 6})  # off the end
    how, _ = copy(0, 0, 400, 0, 200, 100)
    check(how == "passed", "a bank past the framestore: left to the CPU")
    # a mode change forgets the framestore's mapping
    mapped = sum(1 for i in range(1024 * 1024 // 4) if m.word(pt["base"] + 4 * i) & (1 << 28)
                 and ((m.word(pt["base"] + 4 * i) & 0xFFFFFF) << 12) == FS)
    check(mapped == 1, "the framestore is in the GPU's MMU")
    m.call(mod.base + mod.service, {1: 0x4D, 12: m.pw})
    mapped = sum(1 for i in range(1024 * 1024 // 4) if m.word(pt["base"] + 4 * i) & (1 << 28)
                 and ((m.word(pt["base"] + 4 * i) & 0xFFFFFF) << 12) == FS)
    check(mapped == 0, "Service_PreModeChange: the framestore unmapped")
    m.call(mod.base + mod.service, {1: 0x46, 12: m.pw})
    how, _ = copy(0, 0, 400, 0, 200, 100)
    check(how == "claimed", "after the mode change: mapped again, claimed")
    m.finish(R["INT_FRDONE"])
    out, e = mod.swi(0)
    check(out[6] == 0, "the 2D file isn't counted as a program's")
    m.output = ""
    err = mod.command("V3DRender")
    # fills: solid ones taken, patterned and small ones left
    ECF = SCRATCH + 0xA100

    def fill(l, t, r, b, ecf):
        m.uc.mem_write(ECF, struct.pack("<16I", *ecf))
        m.uc.mem_write(P, struct.pack("<5I", l, t, r, b, ECF))
        return gfxv(m, {0: 0, 1: 2, 2: P, 4: 13})
    solid = [0xFFFFFFFF, 0xFF00FF00] * 8
    n = len(m.started)
    how, o = fill(100, 999, 499, 700, solid)
    check(how == "claimed" and o[4] == 0 and len(m.started) == n + 1, "a 400x300 solid fill is claimed")
    if len(m.started) > n:
        va = bo_area["base"] + m.started[-1][1]
        b = bytes(m.uc.mem_read(va, 96))
        check(b[9] == 121 and b[10] & 15 == 3 and struct.unpack_from("<I", b, 11)[0] == 0x00FF00FF,
              "the clear colour is the pixel, ~EOR (&%08X)" % struct.unpack_from("<I", b, 11)[0])
        st = 52 + 4 + 1
        check(b[52] == 124 and b[st] == 29 and b[st + 1] & 15 == 8 and b[st + 13] == 25,
              "the first tile clears the buffer")
    m.finish(R["INT_FRDONE"])
    how, _ = fill(100, 999, 499, 700, [0xFFFFFFFF, 0xFF00FF00, 0xFFFFFFFF, 0xFF00FF01] * 4)
    check(how == "passed", "a patterned fill is left to the CPU")
    how, _ = fill(100, 999, 499, 700, [0x0000FFFF, 0xFF00FF00] * 8)
    check(how == "passed", "a fill that keeps some of the old pixels is left to the CPU")
    how, _ = fill(100, 120, 140, 100, solid)
    check(how == "passed", "a small fill is left to the CPU")
    how, _ = fill(0, 1079, 1919, 1072, solid)
    check(how == "passed", "a wide, short fill (8 rows) is left to the CPU")
    # (0.18) the limits: lowered, a small copy goes to the GPU; reset
    err = mod.command("V3DRender", "2D limits 0 1 0 1 0", 7)
    how, _ = copy(0, 0, 100, 0, 10, 10)
    check(err is None and how == "claimed", "with the limits lowered a 10x10 copy is claimed")
    m.finish(R["INT_FRDONE"])
    m.output = ""
    mod.command("V3DRender")
    check("limits (changed):    copies 0 bytes, 1 rows" in m.output, "*V3DRender shows changed limits")
    check(mod.command("V3DRender", "2D limits 1 2 3", 5) is not None, "three limits refused")
    err = mod.command("V3DRender", "2D limits", 2)
    how, _ = copy(0, 0, 100, 0, 10, 10)
    check(err is None and how == "passed", "*V3DRender 2D limits: the defaults again")
    # (0.19) measured limits: a 32x32 copy is the GPU's, a 16x16 one the CPU's
    how, _ = copy(0, 0, 100, 0, 32, 32)
    check(how == "claimed", "0.19's limits: a 32x32 copy is claimed")
    m.finish(R["INT_FRDONE"])
    how, _ = copy(0, 0, 100, 0, 15, 15)
    check(how == "passed", "0.19's limits: a 15x15 copy is left to the CPU")
    m.output = ""
    mod.command("V3DRender")
    check(re.search(r"us an op \(claimed\): +\d+ in GraphicsV: list \d+, submit \d+, wait \d+", m.output),
          "*V3DRender shows where an op's time goes")
    # (0.21) not while a program has the GPU open, unless "alongside on"
    out, e = mod.swi(1, {0: 0})                       # V3DRender_Open: a program's file
    pf = out[0] if e is None else 0
    how, _ = fill(100, 999, 499, 700, solid)
    check(e is None and how == "passed", "a program has the GPU open: the fill is left to the CPU")
    err = mod.command("V3DRender", "2D alongside on", 3)
    how, _ = fill(100, 999, 499, 700, solid)
    check(err is None and how == "claimed", "*V3DRender 2D alongside on: claimed beside the program")
    m.finish(R["INT_FRDONE"])
    err = mod.command("V3DRender", "2D alongside off", 3)
    if pf:
        mod.swi(2, {0: pf})                           # V3DRender_Close
    how, _ = fill(100, 999, 499, 700, solid)
    check(how == "claimed", "the program closed: claimed again")
    m.finish(R["INT_FRDONE"])
    m.output = ""
    mod.command("V3DRender")
    check("1 a program using the GPU" in m.output, "*V3DRender counts it")
    err = mod.command("V3DRender", "2D fills off", 3)
    how, _ = fill(100, 999, 499, 700, solid)
    check(err is None and how == "passed", "*V3DRender 2D fills off: fills passed on")
    err = mod.command("V3DRender", "2D fills on", 3)
    m.output = ""
    err = mod.command("V3DRender")
    check(err is None and "Desktop copies (2D):   on, not waited for" in m.output
          and "copies by the GPU:   9 of 15 (1 scrolls; 3 small)" in m.output
          and "fills by the GPU:    3 of 8 (2 small, 2 patterned)" in m.output
          and "1 acceleration off" in m.output, "*V3DRender shows them:\n%s" % m.output)
    print("  " + "\n  ".join(l for l in m.output.splitlines() if "2D" in l or "by the GPU" in l
                                or "left to" in l.lower() or "GPU work" in l))
    err = mod.command("V3DRender", "2D off", 2)
    check(err is None and m.gfxv is None, "*V3DRender 2D off releases GraphicsV")
    err = mod.command("V3DRender", "2D on", 2)
    check(err is None and m.gfxv is not None, "*V3DRender 2D on claims it again")
    err = mod.command("V3DRender", "2D zero", 2)
    m.output = ""
    mod.command("V3DRender")
    check(err is None and "copies by the GPU" not in m.output, "*V3DRender 2D zero: counters from 0")
    # the GPU hangs on a waited-for copy: reset, 2D stops itself, but
    # GraphicsV stays claimed (not released inside its own call)
    m.core[R["CT1CA"]] = 0x1234
    how, _ = copy(0, 0, 400, 0, 200, 100)
    check(how == "passed" and m.gfxv is not None, "a copy the GPU hung on: left to the CPU")
    how, _ = copy(0, 0, 400, 0, 200, 100)
    check(how == "passed", "and 2D stays off")
    m.output = ""
    mod.command("V3DRender")
    check("off (stopped after the GPU failed)" in m.output and "1 errors" in m.output,
          "*V3DRender says why: %s" % [l for l in m.output.splitlines() if "2D" in l or "errors" in l])
    err = mod.command("V3DRender", "2D off", 2)
    check(err is None and m.gfxv is None, "*V3DRender 2D off releases GraphicsV after a stop")
    err = mod.command("V3DRender", "2D on", 2)
    check(err is None and m.gfxv is not None, "and 2D on claims it again")
    err = mod.command("V3DRender", "2D sideways", 2)
    check(err is not None, "*V3DRender 2D sideways is refused: %s" % err)
    err = mod.finalise()
    check(err is None and m.gfxv is None and not m.areas, "finalised: GraphicsV released, areas gone")


def test_domain_first(data):
    """(0.20) the firmware's V3D power domain is turned on before any V3D
    register is read: a CM4 aborted at start-up, most likely from the
    IDENT0 read on a V3D whose domain was off (reset released, gates open)"""
    m = Pi4()
    m.strict_domain = True
    m.domain = 0
    m.grafx = 0x40                                   # PM_V3DRSTN: reset released
    m.asb = {0x11008: 0, 0x1100C: 0}                 # gates open
    mod = Module(m, data, 0x20010000)
    err = initialise(mod, "NoWFI")
    check(getattr(m, "read_while_off", 0) == 0, "no V3D register read while its domain was off (%d)"
          % getattr(m, "read_while_off", 0))
    check(getattr(m, "domain_sets", [None])[0] == 1, "the domain turned on first (%s)" % getattr(m, "domain_sets", None))
    check(err is None, "loads (%s)" % err)
    steps = [v for n, v in getattr(m, "var_log", []) if n == "V3DRender$Step"]
    check(steps == ["1 checks", "2 map", "3 power", "4 probe", "5 memory", "6 init", "7 interrupt",
                    "8 RISC3D", "ready"], "(0.24) V3DRender$Step follows start-up: %s" % steps)
    if err is None:
        # (0.22) an MMU error's client is named (0.21 printed through an
        # unrelocated pointer table, and aborted in OS_Write0 on the Pi)
        m.hub[R["HUB_INT_STS"]] = R["HUB_INT_MMU_PTI"]
        m.hub[R["MMU_VIO_ID"]] = 3 << 5
        m.hub[R["MMU_VIO_ADDR"]] = 0x1234
        m.irq()
        m.hub[R["HUB_INT_STS"]] = 0
        m.output = ""
        mod.command("V3DRender")
        check("MMU errors:            1" in m.output and "last: client TLB, page address &00001234" in m.output,
              "the MMU error's client is named: %s" % [l for l in m.output.splitlines() if "client" in l])
        mod.finalise()


def test_start_fails_part_way(data):
    """(0.24) start-up failing after the device is set up (no buffer area):
    an error, no v3d_fini on a half-made device, nothing claimed, and
    V3DRender$Step says where"""
    m = Pi4()
    m.pmp_fail = True
    mod = Module(m, data, 0x20010000)
    err = initialise(mod, "NoWFI")
    steps = [v for n, v in getattr(m, "var_log", []) if n == "V3DRender$Step"]
    check(err is not None and steps[-1:] == ["5 memory"], "fails at step 5: %s (%s)" % (steps[-1:], err))
    check(getattr(m, "vector", None) is None and not m.areas, "nothing left claimed")


try:
    main()
    test_2d(open(sys.argv[1], "rb").read())
    test_start_fails_part_way(open(sys.argv[1], "rb").read())
    test_domain_first(open(sys.argv[1], "rb").read())
except (EmuError, UcError) as e:
    failures += 1
    print("FAIL:", e)
print("%d checks, %d failed" % (checks, failures))
sys.exit(1 if failures else 0)
