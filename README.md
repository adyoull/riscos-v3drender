# V3DRender

A native RISC OS driver for the Raspberry Pi 4's GPU (Broadcom V3D 4.2),
following Linux's v3d driver. Programs draw with Mesa, whose v3d driver
runs in the program and sends its work to V3DRender, as it sends it to
Linux's v3d driver through a DRM device. RISC3D lists V3DRender as the
machine's 3D driver.

**Version 0.02, 12 November 2026.** 0.01 passed its first test on a
Pi 4 (loading, power-up, buffers, reload); 0.02 brings the GPU's first
jobs (`V3DRenderTest-2.zip`), not yet run on the Pi. The SWI chunk (&C3D40) and error block (&81D340) are
provisional.

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
| `include/drm/` | `v3d_drm.h` is Linux's request interface (MIT), unchanged; `drm.h` the generic calls Mesa uses (GEM_CLOSE, sync objects) and RISC OS's sync files. |
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
- **The GPU's MMU**: a 4 MB page table (contiguous, from OS_Memory 12)
  and a scratch page for illegal accesses, as Linux sets it up.
- **Jobs**: bin, render, TFU (texture formatting) and CSD (compute)
  queues, one job at a time each, started by the interrupt handler when
  the last one finishes. A render job waits for its bin job; a job waits
  for the earlier jobs on its buffers and for its sync objects; a cache
  clean follows compute jobs (and CL jobs that ask for it). As Linux.
- **Binner overflow**: a pool of 256 KB buffers given to the binner from
  the interrupt when it runs out, returned when the render job is done.
- **Hangs**: a job that makes no progress for 500 ms (Linux's timeout)
  resets the GPU, at the next call into the driver; the jobs then running
  are ended, and their fences signalled.
- **Clock**: raised to the firmware's maximum while files are open, put
  back when the last closes. `*V3DClock` shows or sets it.
- **Power**: the sequence is Rob Heaton's (V3DDriver), proven on Pi 4s.
  A V3D that is already running when the module loads is never reset,
  so the module can be reloaded.

Not yet: performance counters, ioctl extensions (multisync), the CPU
queue, sharing BOs between files (PRIME), super pages as a request,
cached BO memory with cache maintenance (faster CPU reads).

## SWIs (provisional)

See [`docs/INTERFACE.md`](docs/INTERFACE.md) for the requests, their
structures and the libdrm layer.

| SWI | In | Out |
|---|---|---|
| `V3DRender_Info` (&C3D40) | | r0 = version * 100, r1 = V3D version (42), r2 = HUB_IDENT1, r3 = clock (Hz), r4 = buffer memory, r5 = in use, r6 = files open, r7 -> "v3d" |
| `V3DRender_Open` (&C3D41) | r0 = flags (0) | r0 = file |
| `V3DRender_Close` (&C3D42) | r0 = file | |
| `V3DRender_Ioctl` (&C3D43) | r0 = file, r1 = request (Linux's number), r2 -> structure | r0 = 0 or -errno |
| `V3DRender_Reset` (&C3D44) | | |

`*V3DRender` shows the GPU and what it has done; `*V3DClock [MHz]`.
Loading with the parameter `NoWFI` makes waits spin instead of sleeping
the CPU until the next interrupt.

## V3DDriver

Rob Heaton's V3DDriver accelerates the desktop's fills and copies with
the same GPU. The two can't drive the GPU at once (both would take
device 10 and the job registers), so V3DRender won't load while
V3DDriver is loaded. The plan is that V3DDriver becomes a client of
V3DRender (its jobs through V3DRender's queues), once V3DRender is
proven.

## Building

`RISCOS_TOOLCHAIN=<riscos-crossdev 1.11>/toolchain ./build.sh` builds the
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
- `common/`: Andrew Youll's C module framework (MIT).

Copyright (c) 2026 Andrew Youll.
