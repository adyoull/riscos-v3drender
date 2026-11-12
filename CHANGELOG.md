# Changelog

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
