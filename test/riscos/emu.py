#!/usr/bin/env python3
"""emu.py - load and call a RISC OS module in Unicorn, with just enough of
the OS for module/gcc's runtime: the module is loaded at a chosen RMA
address and its entries are called in SVC mode on a 1 MB-aligned SVC
stack, as RISC OS does. SWIs: OS_Module 6/7, OS_SynchroniseCodeAreas,
OS_CallASWIR12, OS_Write0, OS_NewLine, OS_WriteC, OS_ValidateAddress;
anything else returns "SWI not known" (X form) or stops the test.

As on RISC OS, a SWI issued in SVC mode corrupts r14_svc (here set to a
poison value), so code that forgets to save it fails.

From riscos-mesa-v3d (module/gcc/test/emu.py, MIT licence, Copyright (c)
2026 Andrew Youll); used here unchanged. The machine V3DRender needs
(memory pools, the GPU's registers, interrupts) is test_module.py's.
"""
import struct

from unicorn import Uc, UC_ARCH_ARM, UC_MODE_ARM, UC_HOOK_INTR, UC_HOOK_MEM_WRITE, UcError
from unicorn.arm_const import (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2,
                               UC_ARM_REG_R3, UC_ARM_REG_R4, UC_ARM_REG_R5,
                               UC_ARM_REG_R6, UC_ARM_REG_R7, UC_ARM_REG_R8,
                               UC_ARM_REG_R9, UC_ARM_REG_R10, UC_ARM_REG_R11,
                               UC_ARM_REG_R12, UC_ARM_REG_SP, UC_ARM_REG_LR,
                               UC_ARM_REG_PC, UC_ARM_REG_CPSR)
try:
    from unicorn.arm_const import UC_CPU_ARM_CORTEX_A15
except ImportError:
    UC_CPU_ARM_CORTEX_A15 = None

REGS = [UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3,
        UC_ARM_REG_R4, UC_ARM_REG_R5, UC_ARM_REG_R6, UC_ARM_REG_R7,
        UC_ARM_REG_R8, UC_ARM_REG_R9, UC_ARM_REG_R10, UC_ARM_REG_R11,
        UC_ARM_REG_R12]

RMA = 0x20000000
RMA_SIZE = 0x01000000
STACK_BASE = 0x30000000             # 1 MB aligned, as RISC OS's SVC stack
STACK_SIZE = 0x2000                 # 8 KB
SCRATCH = 0x40000000                # caller's buffers
RETURN = 0x50000000                 # entries return here
POISON_LR = 0x0BADC0DE
V_FLAG = 1 << 28
C_FLAG = 1 << 29
X_BIT = 0x20000


class EmuError(Exception):
    pass


class Machine:
    def __init__(self):
        self.uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
        if UC_CPU_ARM_CORTEX_A15 is not None:
            try:
                self.uc.ctl_set_cpu_model(UC_CPU_ARM_CORTEX_A15)
            except (AttributeError, UcError):
                pass
        self.uc.mem_map(RMA, RMA_SIZE)
        self.uc.mem_map(STACK_BASE, 0x100000)
        self.uc.mem_map(SCRATCH, 0x100000)
        self.uc.mem_map(RETURN, 0x1000)
        self.uc.mem_write(RETURN, struct.pack("<I", 0xE1A00000))   # MOV r0,r0
        self.uc.hook_add(UC_HOOK_INTR, self._intr)
        self.uc.hook_add(UC_HOOK_MEM_WRITE, self._stack_write,
                         begin=STACK_BASE, end=STACK_BASE + 0xFFFFF)
        self.heap = {}              # address -> size
        self.heap_next = RMA + 0x00800000
        self.errors_next = RMA + 0x00F00000     # the OS's error blocks
        self.output = ""
        self.swis = []
        self.synced = []
        self.error_blocks = {}
        self.min_sp = STACK_BASE + STACK_SIZE
        self.fault = None
        self.pw = SCRATCH + 0xFF000   # the module's private word

    # -- memory helpers
    def word(self, a):
        return struct.unpack("<I", self.uc.mem_read(a, 4))[0]

    def set_word(self, a, v):
        self.uc.mem_write(a, struct.pack("<I", v & 0xFFFFFFFF))

    def cstr(self, a, limit=4096):
        b = bytes(self.uc.mem_read(a, limit))
        return b[:b.index(b"\0")].decode("latin-1") if b"\0" in b else b.decode("latin-1")

    def ctrl_str(self, a, limit=4096):
        b = bytes(self.uc.mem_read(a, limit))
        n = 0
        while n < len(b) and b[n] >= 32:
            n += 1
        return b[:n].decode("latin-1")

    def error(self, num, text):
        a = self.error_blocks.get(text)
        if a is None:
            a = self.errors_next
            self.errors_next += 256
            self.uc.mem_write(a, struct.pack("<I", num) + text.encode() + b"\0")
            self.error_blocks[text] = a
        return a

    def claim(self, size):
        size = (size + 7) & ~7
        a = self.heap_next
        self.heap_next += size + 8
        self.heap[a] = size
        return a

    def _stack_write(self, uc, access, address, size, value, data):
        # The words at the stack base are the C library's relocation
        # words, which CMunge's veneers save and restore: not stack use.
        if address >= STACK_BASE + 0x20:
            self.min_sp = min(self.min_sp, address)

    def stack_used(self):
        return STACK_BASE + STACK_SIZE - self.min_sp

    # -- SWIs
    def _set_flags(self, v=None, c=None):
        cpsr = self.uc.reg_read(UC_ARM_REG_CPSR)
        if v is not None:
            cpsr = (cpsr | V_FLAG) if v else (cpsr & ~V_FLAG)
        if c is not None:
            cpsr = (cpsr | C_FLAG) if c else (cpsr & ~C_FLAG)
        self.uc.reg_write(UC_ARM_REG_CPSR, cpsr)

    def _intr(self, uc, intno, data):
        if intno != 2:
            self.fault = "exception %d at &%08x" % (intno, uc.reg_read(UC_ARM_REG_PC))
            uc.emu_stop()
            return
        pc = uc.reg_read(UC_ARM_REG_PC)
        number = self.word(pc - 4) & 0xFFFFFF
        self.min_sp = min(self.min_sp, uc.reg_read(UC_ARM_REG_SP))
        self.swi(number)
        uc.reg_write(UC_ARM_REG_LR, POISON_LR)     # r14_svc is corrupted

    def swi(self, number):
        uc = self.uc
        r = [uc.reg_read(x) for x in REGS[:13]]
        base = number & ~X_BIT
        self.swis.append(base)
        self._set_flags(v=False)
        if base == 0x71:                            # OS_CallASWIR12
            self.swis.pop()
            return self.swi(r[12])
        if base == 0x1E and r[0] == 6:              # OS_Module claim
            uc.reg_write(UC_ARM_REG_R2, self.claim(r[3]))
        elif base == 0x1E and r[0] == 7:            # OS_Module free
            if r[2] not in self.heap:
                return self._fail(number, 0x1E1, "Not a heap block (&%08x)" % r[2])
            del self.heap[r[2]]
        elif base == 0x6E:                          # OS_SynchroniseCodeAreas
            self.synced.append((r[0], r[1], r[2]))
        elif base == 0x02:                          # OS_Write0
            s = self.cstr(r[0])
            self.output += s
            uc.reg_write(UC_ARM_REG_R0, r[0] + len(s) + 1)
        elif base == 0x03:                          # OS_NewLine
            self.output += "\n"
        elif base == 0x00:                          # OS_WriteC
            self.output += chr(r[0] & 0xFF)
        elif base == 0x3A:                          # OS_ValidateAddress
            ok = RMA <= r[0] <= r[1] <= RMA + RMA_SIZE
            self._set_flags(c=not ok)
        else:
            return self._fail(number, 0x1E6, "SWI &%X not known" % base)

    def _fail(self, number, num, text):
        if number & X_BIT:
            self.uc.reg_write(UC_ARM_REG_R0, self.error(num, text))
            self._set_flags(v=True)
        else:
            self.fault = "non-X SWI failed: " + text
            self.uc.emu_stop()

    # -- calling code
    def call(self, addr, regs=None, max_steps=20_000_000):
        """Call addr in SVC mode with r0-r12 from regs (dict); return
        (registers r0-r12, V flag, C flag)."""
        regs = dict(regs or {})
        sp = STACK_BASE + STACK_SIZE
        for i, reg in enumerate(REGS):
            self.uc.reg_write(reg, regs.get(i, 0x77770000 + i))
        self.uc.reg_write(UC_ARM_REG_SP, sp)
        self.uc.reg_write(UC_ARM_REG_LR, RETURN)
        self.uc.reg_write(UC_ARM_REG_CPSR, 0x13 | 0x80)   # SVC, IRQs off
        self.fault = None
        try:
            self.uc.emu_start(addr, RETURN, count=max_steps)
        except UcError as e:
            raise EmuError("%s at pc &%08x" % (e, self.uc.reg_read(UC_ARM_REG_PC)))
        if self.fault:
            raise EmuError(self.fault)
        if self.uc.reg_read(UC_ARM_REG_PC) != RETURN:
            raise EmuError("did not return (pc &%08x)" % self.uc.reg_read(UC_ARM_REG_PC))
        if self.uc.reg_read(UC_ARM_REG_SP) != sp:
            raise EmuError("stack not balanced: sp &%08x" % self.uc.reg_read(UC_ARM_REG_SP))
        if self.min_sp < STACK_BASE + 0x400:
            raise EmuError("SVC stack nearly overflowed (sp &%08x)" % self.min_sp)
        cpsr = self.uc.reg_read(UC_ARM_REG_CPSR)
        if cpsr & 0x1F != 0x13:
            raise EmuError("returned in mode &%x" % (cpsr & 0x1F))
        out = [self.uc.reg_read(x) for x in REGS]
        return out, bool(cpsr & V_FLAG), bool(cpsr & C_FLAG)


class Module:
    """A module file loaded into a Machine's RMA at address."""

    def __init__(self, machine, data, address):
        self.m = machine
        self.data = data
        self.base = address
        machine.uc.mem_write(address, data)
        h = struct.unpack_from("<13I", data, 0)
        (self.start, self.init, self.final, self.service, self.title_off,
         self.help_off, self.cmd_off, self.swi_chunk, self.swi_off,
         self.swi_table, self.swi_decode, self.messages, self.flags_off) = h
        self.title = machine.cstr(address + self.title_off)

    def initialise(self, tail=""):
        m = self.m
        t = SCRATCH + 0x1000
        m.uc.mem_write(t, tail.encode() + b"\0")
        keep = {7: 0x7007, 8: 0x8008, 9: 0x9009, 10: 0xA00A, 11: 0xB00B}
        out, v, _ = m.call(self.base + self.init, {0: t, 11: 0, 12: m.pw, **keep})
        for i, val in keep.items():
            if out[i] != val:
                raise EmuError("initialisation corrupted r%d" % i)
        return m.cstr(out[0] + 4) if v else None

    def finalise(self, fatal=1):
        out, v, _ = self.m.call(self.base + self.final, {10: fatal, 12: self.m.pw})
        return self.m.cstr(out[0] + 4) if v else None

    def swi(self, offset, regs=None):
        r = dict(regs or {})
        r[11] = offset
        r[12] = self.m.pw
        out, v, _ = self.m.call(self.base + self.swi_off, r)
        return out, (self.m.cstr(out[0] + 4) if v else None)

    def commands(self):
        """{name: (code address, info word)} from the command table."""
        cmds = {}
        a = self.base + self.cmd_off
        while self.m.uc.mem_read(a, 1)[0]:
            name = self.m.cstr(a)
            a = (a + len(name) + 1 + 3) & ~3
            code, info, _, _ = struct.unpack("<4I", self.m.uc.mem_read(a, 16))
            cmds[name] = (self.base + code, info)
            a += 16
        return cmds

    def command(self, name, tail="", argc=0):
        code, _ = self.commands()[name]
        t = SCRATCH + 0x2000
        self.m.uc.mem_write(t, tail.encode() + b"\r")
        out, v, _ = self.m.call(code, {0: t, 1: argc, 12: self.m.pw})
        return self.m.cstr(out[0] + 4) if v else None
