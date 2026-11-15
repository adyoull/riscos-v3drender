# Changelog

## Documentation (15 November 2026)

- docs/INTERFACE.md now states the owner's rule: programs use V3DRender only through RISC3D (`RISC3D_Info`, `RISC3D_Open`, and the ioctl SWI that RISC3D names). The direct SWIs are for V3DRender's own tools and kits, and for RISC3D.
- riscos-mesa's libdrm layer works that way since 15 November. On the Pi, ES 2.0 at 1920x1080 ran 100 frames in 37 cs through RISC3D.

## 0.09: the buffer area's size can be set (15 November 2026)

Until now the buffer area was 1 GB, or the biggest of 512, 256 or 128 MB that fitted. It is address space only, but 1 GB of a 32-bit RISC OS's address space is a lot to keep for a whole session. Since 0.09:

- The size is the load option `Size <MB>` (or `Size=<MB>`, `-size`), else the system variable `V3DRender$Size` (MB), else **256 MB**. It is limited to 16-2048 MB, and halved down to 16 MB while the address space can't take it.
- `NoWFI` is now read as one of several words in any order.
- `*V3DRender` and `V3DRender_Info` r4 show the size in use.
- The emulator test checks the default (256 MB), `Size 64`, `Size=128`, the variable, the option over the variable, and the fallback when 1 GB doesn't fit.

A size that follows the RAM (BOs not all mapped for the CPU, the GPU's own 4 GB address space) is a later, bigger change: the owner chose a settable size for now.

## Kit 10 (V3DRender 0.08, unchanged) (15 November 2026)

Kit 9 on the Pi:
- Loading colour, or Z+stencil, into the tile (TLB reads) finishes.
- Storing Z (9), Z+stencil (11) or stencil (10) hangs, every time.

The Z store's Output Image Format is 0, srgb8_alpha8, a colour format. Mesa's v3d format table gives a depth surface d24s8 (43) for S8_UINT_Z24_UNORM, and gives 0 only for a format it has no row for. So V3DTri's depth buffer is probably a format the v3d driver doesn't render to (Z24_UNORM_S8_UINT, say). Replay adds two runs: the Z store in format 43, and Z+stencil in format 43.

## Kit 9 (V3DRender 0.08, unchanged) (15 November 2026)

Kit 8 on the Pi:
- The firmware's V3D domain read "off" at first. PM_GRAFX was &1040 (RSTN, ENAB; on the 2711 the firmware owns the power bits).
- `*V3DPower cycle` worked (the domain then read "on"), but the replay was unchanged: the Z store still hangs into any memory.

Mesa stores Z only (Buffer to Store 9) into a Z24S8 UIF buffer, which may need a read of memory to keep the stencil bits. V3DJob's tests have never made the TLB read memory. Replay adds four runs:
- the Z store as Z+stencil (11);
- the Z store as stencil (10);
- a Load Tile Buffer General of colour before End of Loads, with no Z store;
- a Load Tile Buffer General of Z+stencil before End of Loads, with no Z store.

The loads come from a new generic tile list, 512 bytes on in the same BO, which the RCL is re-pointed to. `V3DJob replay FILE show` prints each run's generic tile list without running it; the build machine checked them against the Mesa dump. Load general is opcode 30 (V3DJob had 0x1C).

## 0.08 and kit 8: the firmware's V3D power domain (15 November 2026)

Kit 7's replay on the Pi:
- the Z store hangs into the Z buffer's memory, into the colour buffer's, and into a new BO;
- the colour store into the Z buffer's memory works;
- every page is below 2^35, the MMU's physical width.

So it isn't the memory: storing the Z/stencil tile buffer never finishes.

Linux has the firmware power the V3D's domain (raspberrypi-power, RPI_POWER_DOMAIN_V3D = 10) before v3d probes. V3DRender, after Rob's driver, only turned on the clock and pulsed the reset.

- **At load**, if the GPU isn't already running, V3DRender asks the firmware for domain 10 on: GET_DOMAIN_STATE (&30030), then SET_DOMAIN_STATE (&38030) if it is off.
- **`*V3DPower`** shows the firmware's domain state and PM_GRAFX with its bits named.
- **`*V3DPower cycle`** works only while nothing uses the GPU. The firmware turns the domain off and on, then V3DRender turns the clock on and resets the GPU (gates, MMU, interrupts).
- **Replay (kit 8)** runs the job set, then `*V3DPower cycle`, then the set again.
- **Tests:** the emulator test answers the domain tags, and checks the domain request at load, `*V3DPower` and `*V3DPower cycle`.

## 0.07 and kit 7: is it the Z buffer's memory? (15 November 2026)

Kit 6's replay on the Pi: Mesa's first job finished at once **without
its Z store**, and hung in every other form:
- as sent;
- without the colour store;
- without primitives (no shading at all);
- with the FS not 4-way threadable;
- with the scoreboard off;
- with no stores at all (a tile with no store at all may not be valid).

So it isn't the shaders or the threads: the GPU's store of the Z tile buffer to &244000 (UIF, Z/stencil) never completes.

- **V3DRender_Debug (&C3D45)**, new: r0 = 0 returns the GPU page-table entry for the GPU address in r1. r0 = 1 returns the MMU's virtual and physical address widths.
- **CT1QCFG** is set to 0 (its reset value: no empty-tile filtering) at load and after every reset, for a V3D that was already running.
- **Replay (kit 7):**
  - It now prints each BO's physical range and its 64 KB / 1 MB page entries from the GPU's page table, and flags any page beyond the MMU's physical width.
  - It adds three runs: the Z store into the colour buffer's memory; the colour store into the Z buffer's memory (with no Z store); and the Z store into a new BO.
  - These separate "the memory at &244000" from "storing Z".

## Kit 6 (V3DRender 0.06, unchanged) (15 November 2026)

Kit 5 passed on the Pi: 64 and 4096 workgroups, every one wrote its number. So all QPUs run threads, and the idea that some QPUs never finish is ruled out.

The owner's *V3DDump of V3DTri's first hang, decoded with tools/v3ddump.py, matches what riscos-mesa's OutDebug showed:
- the BCL, the RCL, the generic tile list, the shader and attribute records, and the binner's tile lists (16 tiles, 128-byte first blocks, PTB chunks to &38B000);
- the render stops in tile (1,0), after row 0.

Kit 6 replays that job without Mesa (`V3DJob replay <dump>`, the Obey file Replay, the dump as MesaJob):
- BOs are made in address order, with fillers for the gaps, so they get the same GPU addresses (first fit).
- Every BO is copied back before each run.
- The job runs as sent, then once each without the Z store, without the colour store, without either, without its primitives, with the FS not 4-way threadable, and with the scoreboard off.
- Each run reports whether it finished, or the render CA and RA at the reset.

The simulated GPU runs replay's mechanics on its own hang dump.

## Kit 5 (V3DRender 0.06, unchanged) (15 November 2026)

riscos-mesa's test 4 on the Pi: Mesa's first job decodes as it should.
- The RCL has the four Tile Rendering Mode Cfg sub-ids and 4x4 supertiles; the generic tile list stores colour (UIF) and Z.
- The GL shader state record has the fragment shader 4-way threadable and not single segment, and the clear shader is `thrsw; thrsw; tlbu; thrsw`, as on Linux.
- With every shader limited to 2 threads it still hangs, so thread switching is not the cause.
- The render job stops at supertile (1,0): RA is &38D075, after the fifth Supertile Coordinates.

Kit 4's compute job used one QPU. Kit 5's test 8 sends 64, then 4096 workgroups of a program (`csd_wide`, from Mesa's packer in `tools/qpu/mkcsd`) that writes each workgroup's number (rf0, Mesa's cs_payload[0] on 4.2) at base + 4 * number, so that every QPU runs threads. The simulated GPU in jobhost runs it too.

`tools/v3ddump.py` now decodes the GL Shader State record and attribute records that the bin list names, and reads Mesa's field starts given in bytes ("3b").

## 0.06: *V3DDump, the first hung job's buffers (15 November 2026)

0.05's registers from V3DTri: every render job stops in Mesa's generic
tile list, just after End of Loads, with the pixel stage holding quads
for the shader processors. About 2.5 tiles of the first frame are drawn.
To see the lists, shaders and tile lists as the GPU had them:

- When the first job hangs after loading, before the reset, V3DRender
  copies every BO the job held into a "V3DRender hang dump" dynamic area,
  along with the registers and the job's lists, tile alloc and tile
  state. That covers the shaders, uniforms, vertex data, the
  binner-written tile lists, the tile state and any overflow BOs. A bin
  job's BOs are the ones its render job holds. BOs bigger than 16 MB
  are only listed.
- `*V3DDump <file>` saves it (type &FFD). `tools/v3ddump.py` prints it:
  - which BO each of CA, RA and BPCA is in;
  - the render list, the generic tile list it names, and the bin list,
    decoded with Mesa's v3d_packet.xml (addresses unpacked as Mesa
    does);
  - a hex dump around CA, and each tile's first block in tile alloc.
  `-x DIR` writes out each BO.
- Render jobs keep their bin list, qma, qms and qts for the dump.
- Tests:
  - The emulator test hangs a bin job and checks the dump's header and
    the BO's contents, and that `*V3DDump` saves the right size.
  - build.sh runs v3ddump.py on the simulated hang's dump.

## 0.05: what the GPU was doing when a job hung (15 November 2026)

riscos-mesa's first Mesa program on V3DRender (V3DTri, Mesa 26.1.6's v3d
driver) had every request accepted, but each of its 102 frames, even the
first one, which only clears, hung on the GPU and was reset, with one
interrupt a frame. 0.04 only logged ERR_STAT.

- When a job is found hung, the registers are kept before the reset
  (`struct v3d_hang`, at the end of the statistics, so also through
  `V3DRender_Info` r8): which queue, the job's list, CT0/CT1 CS/CA/EA/RA,
  PCS, BFC, RFC, ERR_STAT, FDBGO/B/R/S, the binner's memory registers
  (BPCA/BPCS/BPOA/BPOS), and 8 words of the list around the hung
  queue's current address. The first and the last hang since loading
  are kept, and `*V3DRender` shows them.
- Check and HangTest load 0.05.

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
