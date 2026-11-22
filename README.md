# V3DRender

A native RISC OS driver for the Raspberry Pi 4's GPU (Broadcom V3D 4.2),
following Linux's v3d driver. Programs draw with Mesa, whose v3d driver
runs in the program and sends its work to V3DRender, as it sends it to
Linux's v3d driver through a DRM device. RISC3D lists V3DRender as the
machine's 3D driver.

**Version 0.26, 22 November 2026.** Tested on a Raspberry Pi 4 under native
RISC OS 5:

- Mesa's v3d driver (Mesa 26.1.6, through riscos-mesa) draws with it:
  OpenGL 2.1 and GLES 2, 100 frames of 1920x1080 in 37 cs; the web
  browser's GPU compositing, 404,818 jobs with no errors (0.23).
- The desktop's copies and fills on the GPU (0.17, off by default):
  RISCOSmark's Rectangle Copy 7.1 times as fast at 1920x1200 (519 to
  3,679), pixel-identical to the CPU's.
- The kit's own jobs: cleared 1080p frames at 540 a second, the TFU
  (texture formatting), compute on every QPU, the GPU's MMU, its
  interrupt, and the reset after a hang.
- Buffers of 64 KB and 1 MB or more in the GPU MMU's big entries, as
  Linux; GPU time per program; a hang ends only the job that hung.
- Imported memory follows pages RISC OS moves (0.23); freed buffers'
  pages kept for the next (0.25).
- Frames read back by the CPU at about 2.6 to 3.9 GB/s from cached
  buffers or the program's own memory (151 MB/s from ordinary buffers).
- It works alongside ReelHWAccel: Reel decodes on the VideoCore while
  V3DRender is loaded, because V3DRender keeps its memory above 1 GB.

The SWI chunk (&5AFC0) and error block (&823E00) are allocated by RISC OS
Open (since 0.16; earlier versions used provisional &C3D40 and &81D340).

**Programs never call V3DRender themselves: they reach it through RISC3D**
(see [`docs/INTERFACE.md`](docs/INTERFACE.md)). Its own SWIs are for RISC3D
and its test tools.

## Installing

V3DRender comes inside RISC3D (`!RISC3D.GPU`), which loads it at start-up.
On its own: RISC OS 5.30 or later on a Pi 4, Pi 400 or CM4, with Rob
Heaton's V3DDriver not loaded. `*RMLoad V3DRender` before RISC3D (0.05,
with the allocated numbers; 0.03 and 0.04 work too), which then lists it as the machine's GPU driver. The test kit
(V3DRenderTest) checks a machine: run Check and read Results.

## How it fits

```
program -> Mesa (EGL, OpenGL, GLES)
             -> Mesa's v3d driver (compiles shaders, builds control lists)
                  -> libdrm (drmIoctl)  ->  V3DRender_Ioctl
                                               V3DRender: buffers, the GPU's
                                               MMU, queues, interrupts, resets
                                                 -> the V3D
RISC3D: says which driver a program's contexts get (RISC3D_Info: a "DRM
device", ioctl SWI V3DRender_Ioctl, DRM driver "v3d").
```

On RISC OS on Linux the same programs get Linux's own v3d driver through
RISC3D's Linux3D. Native and Linux therefore run the same Mesa driver on
the same GPU.

## The Pi 5

32-bit RISC OS can't run natively on a Pi 5: its Cortex-A76 has no 32-bit
privileged modes. On a Pi 5, 3D comes through RISC OS on Linux (RISC3D and
Linux3D). The core here keeps the V3D 7.1 (Pi 5) register differences
from Linux (TFU, CSD, GMP, interrupts, SMS power), and is plain C with
its machine behind `core/v3d_plat.h`, so that it can go to a 64-bit RISC
OS when there is one; the RISC OS layer here is the Pi 4's.

## The parts

| Part | What it is |
|---|---|
| `core/` | The driver, after Linux's `drivers/gpu/drm/v3d` (rpi-6.12.y): hardware sequences (`v3d_hw.c`), queues, fences and hang checks (`v3d_sched.c`), buffers (`v3d_bo.c`), the request interface (`v3d_ioctl.c`). `v3d_regs.h` is Linux's, unchanged. |
| `include/drm/` | `v3d_drm.h` is Linux's request interface (MIT), unchanged, as current Mesa carries it; `drm.h` the generic calls Mesa uses (GEM_CLOSE, sync objects) and RISC OS's sync files. |
| `riscos/` | The module: `module.c` (SWIs, files, RISC3D), `plat_pi4.c` (registers, power, memory, the interrupt), `header.s`. |
| `test/host/` | The core on the build machine with a simulated V3D: jobs, dependencies, binner overflow, MMU entries, sync objects, timeouts and reset, old structure sizes, V3D 7.1. |
| `test/riscos/` | The built module in Unicorn with a stand-in Pi 4: power-up, the page table, every SWI, a job finished by the real interrupt path, unload, reload on a running GPU. |
| `kit/` | The Pi tests: V3DCheck (buffers), V3DJob (cleared frames, built as Mesa builds them, in `kit/src/`) and HangTest (`V3DRenderTest-2.zip`). |

## What the driver does

- **Buffers (BOs)** live in one physical memory pool, "V3DRender buffers"
  (up to 1 GB of address space). A BO's GPU address is its offset in the
  pool, and programs reach it at the same offset, so `MMAP_BO` gives its
  address directly. Pages are claimed from RISC OS as needed, locked, and
  mapped not cached but bufferable (Linux maps v3d BOs write-combined).
  They need not be contiguous: the GPU's MMU joins them up, with 64 KB
  and 1 MB entries where pages happen to be contiguous.
- **Memory above 1 GB first** (0.12): the VideoCore reaches only the first
  1 GB of RAM, so its users (VCHIQ, ReelHWAccel's H.264 decoder and
  encoder) need contiguous memory there; the V3D's MMU reaches it all.
  V3DRender asks OS_Memory 12 for runs of pages between 1 GB and 4 GB
  (RISC OS 5.29+) and lets the kernel pick only when there are none (a
  1 GB Pi, or memory above 1 GB used up). `*V3DRender` shows how much is
  above and below 1 GB, and where the page table is.
- **The GPU's MMU**: a 4 MB page table (contiguous, from OS_Memory 12,
  above 1 GB where there is room) and a scratch page for illegal accesses,
  as Linux sets it up.
- **Jobs**: bin, render, TFU (texture formatting) and CSD (compute)
  queues, one job at a time each, started by the interrupt handler when
  the last one finishes. A render job waits for its bin job; a job waits
  for the earlier jobs on its buffers and for its sync objects; a cache
  clean follows compute jobs (and CL jobs that ask for it). As Linux.
- **Binner overflow**: a pool of 256 KB buffers given to the binner from
  the interrupt when it runs out, returned when the render job is done.
- **Hangs**: a job that makes no progress for 500 ms (Linux's timeout)
  resets the GPU, at the next call into the driver; the job that hung is
  ended and its fence signalled. Since 0.13 the jobs running on the other
  queues run again, as Linux resubmits them.
- **Big pages** (0.13): BOs of 64 KB and 1 MB or more are aligned as
  Linux aligns them, so the GPU's MMU uses 64 KB and 1 MB entries.
- **GPU time** (0.13): per queue, per file and for everyone
  (`DRM_IOCTL_RISCOS_FILE_STATS`, `*V3DRender`).
- **Refusing to start** where there's no V3D 4.2: anything but a Pi 4,
  400 or CM4 (asked through BCMSupport's board revision, so nothing else
  is touched), or a V3D that doesn't answer. RISC3D's package carries
  V3DRender to every machine and loads the first GPU driver that starts.
- **Clock**: raised to the firmware's maximum while files are open, put
  back when the last closes. `*V3DClock` shows or sets it.
- **Power**: the sequence is Rob Heaton's (V3DDriver), proven on Pi 4s.
  A V3D that is already running when the module loads is never reset,
  so the module can be reloaded.

Not yet: performance counters, ioctl extensions (multisync), the CPU
queue, sharing BOs between files (PRIME).

### Freed GPU memory kept for reuse (0.25)

A closed buffer's pages stay claimed, up to 16 MB, while a program has the
GPU open. The next buffer that fits uses them without asking RISC OS for
memory. This makes the browser's constant texture making and dropping
cheaper, and RISC OS moves fewer other programs' pages. The pages go back
when the last program closes. `*V3DRender pool <MB>` sets the limit (0 keeps
none).

## SWIs (provisional)

See [`docs/INTERFACE.md`](docs/INTERFACE.md) for the requests, their
structures and the libdrm layer.

| SWI | In | Out |
|---|---|---|
| `V3DRender_Info` (&5AFC0) | | r0 = version * 100, r1 = V3D version (42), r2 = HUB_IDENT1, r3 = clock (Hz), r4 = buffer memory, r5 = in use, r6 = files open, r7 -> "v3d", r8 -> statistics |
| `V3DRender_Open` (&5AFC1) | r0 = flags (0) | r0 = file |
| `V3DRender_Close` (&5AFC2) | r0 = file | |
| `V3DRender_Ioctl` (&5AFC3) | r0 = file, r1 = request (Linux's number), r2 -> structure | r0 = 0 or -errno |
| `V3DRender_Reset` (&5AFC4) | r0 bit 0: also close every file | |
| `V3DRender_Debug` (&5AFC5) | r0 = 0, r1 = GPU address; r0 = 1; or r0 = 2, r1 = GPU address | r0 = page table entry; r0, r1 = the MMU's address widths; or (2, 0.13) the next BO starts at r1 if free (the kit's replay) |

`*V3DRender [2D on|async|off|fills on|off]` shows the GPU and what it has done (0.17: and turns the desktop's 2D work on or off); `*V3DClock [MHz]`;
`*V3DPower [cycle]`; `*V3DDump <file>`; `*V3DReset [all]`.

**If something goes wrong (0.10):** `*V3DReset` ends every GPU job, running or
waiting, and resets the GPU. Programs waiting for those jobs are told they
failed, and the GPU is ready for the next program. `*V3DReset all` also
closes every V3DRender file, with its buffers. Use it when programs stopped
without closing theirs, for example after Alt-Break or an error in a
command-line program; a program still using the GPU must start again.
`V3DRender_Reset` does the same (r0 bit 0 = all).

For frames the CPU reads back (0.11): CREATE_BO's `DRM_RISCOS_BO_CPU_CACHED`
flag maps a BO cacheable for the CPU, and `DRM_IOCTL_RISCOS_IMPORT_MEMORY`
lets the GPU draw into the program's own memory (a sprite, the screen).
V3DRender does the cache maintenance either way. See docs/INTERFACE.md. On its own, V3DRender
resets the GPU when a job makes no progress for half a second (as Linux),
and since 0.10 also when one has run for 5 seconds, even if it still moves. `V3DRender_Debug` (&5AFC5) reads
the GPU's page table (r0 = 0, r1 = GPU address) and the MMU's widths
(r0 = 1).

Load options (0.09), in any order:

- `Size <MB>` (or `Size=<MB>`): the buffer area, 16 to 2048 MB. Without
  it, the system variable `V3DRender$Size` (MB), else **256 MB**. If the
  address space can't take that, it is halved until it fits (down to 16
  MB). The area is address space only: memory is taken as buffers are
  made and given back when they are freed. All the GPU's buffers must
  fit in it at once. To change it: `*RMKill V3DRender`, then
  `*RMLoad V3DRender Size 512`, or `*Set V3DRender$Size 512` in !Boot
  before V3DRender loads.
- `NoWFI`: waits spin instead of sleeping the CPU until the next
  interrupt.
- `2D` or `2DAsync` (0.17): the desktop's copies and fills on the GPU,
  waited for or not (above). Without it, `V3DRender$2D` (`on`, `async`).

## The desktop's copies and fills (0.17)

V3DRender can do the desktop's rectangle copies (window moves, scrolls,
`OS_Plot` block copies) and solid fills on the GPU, through its own
queues: GraphicsV 13 ops 1 and 2. This is Rob Heaton's V3DDriver 2D
acceleration, written again for V3DRender (V3DDriver itself can't be
loaded with V3DRender, as both would drive the GPU). It is **off by
default**:

- `*V3DRender 2D on`: each copy and fill waited for.
- `*V3DRender 2D async`: not waited for, up to four at once, honouring
  GraphicsV's "synchronise" flags (experimental: a program reading the
  screen directly could see it unfinished).
- `*V3DRender 2D off`; `*V3DRender 2D fills on|off` for fills alone.
- `*V3DRender 2D limits [5 numbers]`: the copy bytes, copy rows, fill
  bytes, fill rows and fill bytes a tile below which the CPU does the
  work. No numbers gives the defaults measured on a Pi 4 in 0.19: 1024,
  8, 4096, 16, 0 (Rob's V3DDriver used 2048, 16, 16384, 32, 4500).
- `*V3DRender 2D zero` (0.19): the 2D counters and times back to 0.
- While any program has the GPU open, the desktop's work stays on the
  CPU (0.21; a web browser's GPU drawing went wrong beside it).
  `*V3DRender 2D alongside on` allows it anyway, for testing.
- At load: the `2D` or `2DAsync` option, or `*Set V3DRender$2D on`
  (or `async`) before V3DRender loads.

Small copies and fills (Rob's measured limits), patterned fills, a
cached screen, `OS_ScreenMode 4`'s "no hardware acceleration", and
anything while a program has 3D work on the GPU are left to the CPU.
`*V3DRender` shows what was done and why the rest wasn't. The kit's
Desk2D checks the GPU's result against the CPU's on the screen.

## Building

`RISCOS_TOOLCHAIN=<riscos-crossdev 1.11 toolchain directory> ./build.sh` builds the
module and the kit into `out/` and runs both tests (the emulator test
needs `pip install unicorn`).

## Licence and credits

GNU General Public License, version 2 or later (see `LICENSE` and
`NOTICE`), as the Linux v3d driver it follows.
- Linux's v3d driver: Copyright (C) 2014-2018 Broadcom, (C) 2023
  Raspberry Pi. `core/v3d_regs.h` is Linux's file; `core/v3d_hw.c`
  follows Linux's functions one for one; `include/drm/v3d_drm.h` is
  Linux's (MIT).
- The Pi 4 power-up, already-running check and reset sequence: Rob
  Heaton's V3DDriver (GPL-2.0-or-later), which took them from
  rpi4-gpu-bare-metal-examples (MIT, Copyright (c) 2022 Random06457).
- The desktop's copies and fills (`core/v3d_2d.c`, `riscos/twod.c`,
  0.17): after Rob Heaton's V3DDriver (GPL-2.0-or-later): its GraphicsV
  13 handling, tile load/store and clear lists, tile order for
  overlapping copies, formats and measured limits.
- `common/`: Andrew Youll's C module framework (MIT).

Copyright (c) 2026 Andrew Youll.
