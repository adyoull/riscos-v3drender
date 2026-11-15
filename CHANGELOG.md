# Changelog

## Kit 4 (V3DRender 0.04, unchanged) (15 November 2026)

Kit 3 on the owner's Pi 4: the TFU (lineartile, UIF, UIF with XOR) and
RISC3D (listed as a DRM device, a frame drawn on a file from
RISC3D_Open) passed. Two tests failed, both because of the test:

- Compute: the job never ended and was reset after about 1 s. csd_prog's
  only thrsw is its end, so the job must be started in the final segment
  (`V3D_CSD_CFG5_SINGLE_SEG`, bit 1 of cfg[5], as Mesa sets for programs
  with no earlier thrsw). V3DJob sets it now. The simulated GPU in
  `test/host/jobhost.c` now hangs a compute job without it (checked: the
  old V3DJob fails there).
- Binner overflow: a cleared 1080p frame given 8 KB of binner memory
  didn't run out. A frame with no primitives never grows its tile lists,
  so overflow needs drawn triangles (shaders). The test was removed; the
  simulated GPU no longer runs such a frame out of memory.

## 0.04: kit 3, the rest of the GPU's job types (15 November 2026)

Kit 2 passed on the owner's Pi 4 (0.02): cleared frames through the
queues, the GPU's MMU and its interrupt, 1080p at 555 frames a second,
404 interrupts for 404 jobs, and the reset after a hung job.

- RISC3D's describe call (reason 3, RISC3D 0.03) is answered "on a GPU"
  (r0 = 0). RISC3D only asks drivers registered with flags bit 1, which
  V3DRender isn't, so this is for safety.
- `V3DRender_Info` r8 points to the statistics (`struct v3d_stats`,
  read only), so test programs can check the counters.
- Kit 3 (`V3DRenderTest-3.zip`), V3DJob tests 4-7:
  - TFU: a raster image converted into lineartile, UIF and UIF with XOR,
    as Mesa's v3dX(tfu) sets the job up, every pixel checked against
    Mesa's tiling functions (`kit/src/tiling.h`).
  - Compute: one workgroup of 16 runs a program made with Mesa's own QPU
    packer (`tools/qpu`, `kit/src/csd_prog.h`) that stores a value from
    its uniforms at an address from its uniforms.
  - Binner overflow: a 1080p frame given 8 KB of binner memory.
  - Through RISC3D: RISC3D 0.03 lists V3DRender as a DRM device, and a
    frame drawn on the file from RISC3D_Open is right.
- Tests on the build machine: Mesa's 45 disassembler test vectors pass
  through the copy of its packer used here. The emulator test calls the
  module's RISC3D entry (open, describe, sync refused, close) and reads
  the statistics block. V3DJob's new tests run against the core with a
  simulated GPU that carries out TFU, compute and overflow.

## 0.03: the reset counters current Mesa asks for (13 November 2026)

- GET_PARAM answers `DRM_V3D_PARAM_GLOBAL_RESET_COUNTER` (every reset)
  and `DRM_V3D_PARAM_CONTEXT_RESET_COUNTER` (resets caused by this file's
  jobs, counted against the job that timed out), as newer Linux kernels
  do; current Mesa's v3d driver reads them (robustness: whether a reset
  was the program's doing). `include/drm/v3d_drm.h` is now the copy
  current Mesa carries, which adds those two names; nothing else in it
  differs.
- For RISC3D's two-Mesa plan (riscos-mesa adds a newer Mesa with only
  the v3d driver, on V3DRender): checked against current Mesa's v3d
  driver, which needs nothing else from V3DRender for a first version.
  Kit 2 (0.02) is unchanged; this changes nothing it tests.

## 0.02: the GPU's first jobs (12 November 2026)

0.01 passed on the owner's Pi 4: power-up, V3D 4.2 rev 14, a 1 GB buffer
area, buffers made, used from a program and freed, the clock raised and
restored, and a reload onto the running GPU ("already running at load")
with no freeze. No GPU work had been sent.

- A render job whose bin job failed (a reset) is ended without being run,
  as Linux does: its tile lists aren't there. (Found writing the hang
  test; the host test has the case.)
- The second Pi kit (`V3DRenderTest-2.zip`): V3DJob, the GPU's first
  jobs. Frames that are only cleared, built as Mesa's v3d driver builds
  them (`kit/src/clbuild.c`, packets from Mesa's v3d_packet.xml): a
  128x128 frame with every pixel and the memory either side checked, a
  1920x1080 frame, 200 more timed; HangTest: a bin job that waits for a
  semaphore for ever, for the first reset on hardware.
- Tests on the build machine: the control lists decoded against Mesa's
  v3d_packet.xml (`test/host/cldecode.py`); V3DJob itself run against the
  core with a simulated GPU that carries out the frames it is sent
  (`test/host/jobhost.c`), the hang test included.
- V3DCheck: the version printed as 0.02 (not 9.99999999984E-3), and line
  numbers (no "Program renumbered").

## 0.01: first build (12 November 2026)

Not yet run on a Pi.

- The core, after Linux's v3d driver (rpi-6.12.y): power-up checks, MMU
  and page table, cache flush and clean, interrupts with binner overflow,
  bin, render, TFU and CSD jobs, the 500 ms hang check and reset; V3D 4.2
  and 7.1. Linux's v3d request interface, its generic sync objects, and
  RISC OS sync files.
- The scheduler, for one CPU and no threads: fences as (queue, number),
  jobs started from the interrupt handler, buffers let go and hangs
  handled at the next call.
- The Pi 4 under RISC OS: Rob Heaton's power-up and reset sequences,
  device 10, BOs in a physical memory pool (locked pages, not cached but
  bufferable, user read/write), a 4 MB contiguous page table.
- RISC3D: registers as a driver of the new "DRM device" kind (RISC3D
  0.02).
- Refuses to load beside V3DDriver.
- Tests: the core with a simulated V3D (1,896 checks, with the address
  and undefined-behaviour checkers); the built module in Unicorn with a
  stand-in Pi 4 (12,399 checks, deepest SVC stack 1,772 bytes).
- The first Pi test: `V3DRenderTest.zip` (Check, V3DCheck, ReadMe).
