# Changelog

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
