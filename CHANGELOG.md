# Changelog

## 0.15: cache work that no longer costs YSFlight its frames (16 November 2026)

YSFlight's log with `V3DM$Stats` (owner, MesaLibs gpu7, V3DRender 0.13/0.14) showed where its frames went. In each 5 s:

- the GPU was busy for about 350 ms (bin about 12 ms, render about 340 ms, for about 140 jobs);
- but `SUBMIT_CL` took 2.5–3.2 s (about 20 ms each), and `WAIT_BO` 1.3–2.2 s (about 2000 calls).

So about 4.5 s of every 5 s were spent inside V3DRender, and nearly all of it was cache maintenance. YSFlight draws into an imported, cached colour buffer (its window's sprite). That buffer is in every one of its roughly 9 jobs a frame, and was cleaned by address at each submit and invalidated by address after each job. A big window is hundreds of thousands of cache lines each time, and each line costs a trip through L2.

- **The whole data cache, above the kernel's threshold:** `plat_cache_clean` and `plat_cache_inval` now use the kernel's own ARMops (OS_MMUControl 2): `Cache_CleanAll` and `Cache_CleanInvalidateAll`. They are used when the range is at least the kernel's `Cache_RangeThreshold` (1 MB if it gives none), working by set/way through the Pi 4's L1 and L2 (about 17,000 lines) instead of line by line. Smaller ranges stay by address. Without OS_MMUControl 2 (before RISC OS 5.23), it's line by line as before.
- **An invalidate waits for the BO's last job:** a stale BO that another queued or running job will write again isn't invalidated yet. That happens once that job is done, at the file's next request. YSFlight's 9 invalidates a frame become about 1. The CPU can't be reading it meanwhile, because it waits for the job first, and the wait's request does the invalidate.
- **`*V3DRender`** shows the cache work: BOs cleaned and invalidated, invalidates put off, CPU time spent, and how many times the whole cache was done (and from what size).
- **Statistics block** (appended): `cache_ns`, `cache_cleans`, `cache_invals`, `cache_deferred`.
- **Tests:** sim 2012 checks (a cached BO in two queued jobs: cleaned twice, the invalidate put off until the second finishes, then once). Emulator 44411 (the kernel's ARMops stubbed: by address under 1 MB; a 1 MB cached BO cleaned with Cache_CleanAll, at create and at submit, and Cache_CleanInvalidateAll after the job; the status line). Mutations caught.
- **Kit 17:** as kit 16. Test 11's 8 MB cached and imported frames now go through the whole-cache path, and still check every pixel and guard.

## 0.14: clean and invalidate after jobs; the ARM's clock; kit 16 (16 November 2026)

- **Cache maintenance after a job is now clean and invalidate (DCCIMVAC), not invalidate (DCIMVAC).** riscos-mesa asked whether a cached screen imported without `DRM_RISCOS_IMPORT_UNCACHED` is safe. It wasn't quite: while the GPU draws, other tasks can write elsewhere on that screen through the cache, and an invalidate would throw away those writes if they were still in the cache. Clean and invalidate keeps them; lines the GPU wrote are clean since the submit, so they are only invalidated, as before. The same applies to cached BOs and imported sprites. Costs the same per line.
- **`*V3DRender` shows the ARM's clock** and the firmware's most for it, since frame rates on one core depend on it (Linux on a Pi 4 usually runs at 1.8 GHz).
- **Kit 16:** test 11 also times the CPU writing each kind of frame memory (memset, 10 times over 1080p): Mesa writes its vertex data, uniforms and lists into ordinary BOs with the CPU.
- Tests: emulator 43627 (version 0.14; the module has DCCIMVAC and no DCIMVAC; the ARM clock line).

## 0.13: riscos-mesa's four requests; refusing to start, tested (16 November 2026)

riscos-mesa compared V3DRender with Linux's v3d (`MESA/handoffs/2026-11-16-mesa-v3drender-linux-comparison.md`) and asked for four things, in order of value. RISC3D asked that V3DRender refuse to start where there's no V3D, before it goes in RISC3D's one package for every machine.

- **Uncached imports:** `drm_riscos_import_memory.pad` is now `flags`. `DRM_RISCOS_IMPORT_UNCACHED` (1) marks memory the CPU doesn't cache (screen memory, screen banks), and V3DRender then skips the clean at submit and the invalidate after the job. At 1920x1080 that was about 131,000 cache-line operations each way per frame. Other bits are refused with EINVAL; 0.11 and 0.12 already refuse any nonzero `pad`, so a program can try the flag and drop it.
- **Big pages, as Linux:** BOs of 1 MB or more get a 1 MB-aligned GPU address, and BOs of 64 KB or more a 64 KB-aligned one (Linux's `v3d_bo_create_finish`), placed from the top of the area down, so small BOs (first fit from the bottom) don't break up the aligned space. The Pi 4 layer asks OS_Memory 12 for physical runs aligned the same (2^20 or 2^16), falling back to 4 KB. The GPU's MMU then uses 1 MB and 64 KB entries (`v3d_mmu_insert_ptes`, unchanged). Imported BOs keep first fit.
- **GPU time:** each job's time on the GPU (put on it to its interrupt, from the ARM's generic timer, CNTVCT/CNTFRQ; centiseconds if CNTFRQ is unset), per queue, per file and for everyone, as Linux's fdinfo engine statistics. `DRM_IOCTL_RISCOS_FILE_STATS` ('R', 3) returns the file's and everyone's. `*V3DRender` shows everyone's in ms. Cache-clean jobs are timed on the CPU, where they run.
- **After a hang, the innocent jobs run again** (Linux's `drm_sched_resubmit_jobs`): the job that hung is ended as failed; jobs then running on other queues go back to the front of their queues and start again. A render job's hang no longer loses the next frame's bin job too. `*V3DReset` still ends everything. `*V3DRender`: "jobs run again after" resets. Statistics block: `requeued` and `busy_ns[5]` appended.
- **Refusing to start** (RISC3D's ask) was already there: the board revision through BCMSupport's mailbox (an error from the SWI where there's no BCMSupport), and the V3D's identity registers. Now tested: on a Pi 3B+, a Pi 1, a machine without BCMSupport, and a BCM2711 whose V3D doesn't answer, initialisation returns an error, claims nothing, and (but for the last) maps and touches no hardware and asks the firmware nothing but the board revision.
- **`*V3DRender`** also shows how much buffer memory is in 1 MB and 64 KB entries.
- **`V3DRender_Debug` 2** (r1 = GPU address): the next BO made starts there, if that space is free. The kit's replay uses it to put a *V3DDump's BOs back at their addresses whatever the layout (older modules: fillers, as before, with BOs over 64 KB made of smaller parts).
- **Tests:** sim 1999 checks (alignment and big/super entries; per-file and global stats; uncached import with no cache work, flag 2 refused; a render hang with the next bin job running: the bin job rerun, its render job then runs, only the guilty file's reset counter moves; `v3d_cancel_all` still ends both). Emulator 43424 (version; 1 MB BO in a 1 MB entry, 64 KB BO in 64 KB entries; the status lines; four refusals). Debug 2. Seven core mutations and two module mutations caught. Replay of the owner's MesaJob dump still places all 9 BOs at their addresses. build.sh's dump check follows the new layout (tile alloc at &1000).
- **Kit 15:** V3DJob test 12 also counts 1 MB and 64 KB entries and checks the 8 MB buffer is all in 1 MB entries; test 13 prints this program's and everyone's GPU time per queue, and checks the uncached import flag is accepted and an unknown one refused.

## 0.12: memory above 1 GB first, leaving the VideoCore's memory free (16 November 2026)

RISC3D now loads V3DRender at start-up, so it is always there when ReelHWAccel runs. The VideoCore can reach only the first 1 GB of RAM, and ReelHWAccel's H.264 decoder and encoder (and VCHIQ) need contiguous memory there. The V3D's MMU reaches all of it, so V3DRender now keeps out of that 1 GB where it can (the owner's request).

- **Buffers:** pages are claimed as runs of contiguous pages between 1 GB and 4 GB (OS_Memory 12 with a range, RISC OS 5.29+). A run that can't be had is halved, down to 64 KB (16 pages; smaller BOs ask for their own size); only what is left after that comes from the kernel's choice, as before. Contiguous runs also give the GPU's MMU more 64 KB and 1 MB entries.
- **The page table** (4 MB, contiguous) comes from above 1 GB too, else from anywhere as before.
- **No change** on a 1 GB Pi (nothing above 1 GB) or a RISC OS before 5.29 (which ignores the range), except one extra OS_Memory 12 call per BO.
- **`*V3DRender`** shows the buffer memory in use above and below 1 GB, and the page table's physical address.
- **Kit 14:** V3DJob test 12 (Check) makes an 8 MB buffer, reads every page's entry from the GPU's page table, and checks all of it is above 1 GB when the Pi has more than 1 GB (OS_Memory 8).
- **Tests (emulator, now with physical page numbers as on a Pi):** the page table above 1 GB and contiguous, with OS_Memory 12 asked from 1 GB; a 5-page and a 1 MB BO above 1 GB in one run; with room for only 40 pages above 1 GB, a 100-page BO gets 25 there and the rest from the kernel, whole and zeroed; `*V3DRender`'s figures; a 1 GB Pi and a RISC OS before 5.29 both still load and make BOs. Three mutations (no range asked, no high runs, the page table not high) caught.

## 0.11: cached BOs and imported memory, for frames read back (15 November 2026)

This answers riscos-mesa's `2026-11-15-mesa-v3drender-cached-reads-and-import.md`. Their EGL/OSMesa draw on the GPU in about 1 ms a frame at 640x480, then spend about 5.5 ms reading it from an uncached BO, at about 215 MB/s.

- **`DRM_RISCOS_BO_CPU_CACHED`** (CREATE_BO flag `0x80000000`): the CPU's mapping is cacheable.
  - The BO is cleaned when a job lists it.
  - It is invalidated, after the job, before the owning file's next request returns.
  - BOs remember their file; a reaped job marks its cached BOs stale, and the file's next request invalidates them in its own context.
- **`DRM_IOCTL_RISCOS_IMPORT_MEMORY`**: the program's own page-aligned memory as a BO.
  - Area pages are taken for the GPU addresses only.
  - The MMU points at the memory's physical pages (OS_Memory 65).
  - MMAP_BO returns the program's address.
  - Cache maintenance as above.
  - GEM_CLOSE unmaps it after its jobs.
  - Hang records and dumps never read an imported BO.
- **Cache maintenance** by address to the point of coherency (DCCMVAC/DCIMVAC, the line size from CTR), in `riscos/header.s`.
- **Tests:**
  - sim (1954 checks): the cached flag, clean at submit, one invalidate at the next request, no work for ordinary BOs, an unknown flag refused; an import's PTEs, MMAP_BO, unaligned/zero sizes refused, and an import kept while a job uses it, then unmapped, using no area memory.
  - Emulator: a cached BO's pages mapped cacheable and locked (ordinary ones still not cacheable); an import's PTE and MMAP_BO, with no area page taken; a job using both, and GEM_CLOSE unmapping the import.
  - V3DJob test 11 (Check): a 1080p frame drawn into an uncached BO, a cached BO and imported memory, after the CPU has read the old contents into its cache. Every pixel is checked, a second frame is drawn on the same memory, and the CPU's read-back speed is printed.

## 0.10: recovering after a failed run (15 November 2026)

The owner found V3DRender stuck after a failed run, and asked for a way to reset the GPU and recover.

- **`*V3DReset [all]`** (and `V3DRender_Reset`, with r0 bit 0 = all):
  - Every job waiting in a queue is ended unrun.
  - Every running job is ended by a GPU reset.
  - Their fences are signalled as failed, so waiting programs carry on, and their buffers are let go.
  - `all` also closes every V3DRender file, with its buffers and sync objects. It is meant for command-line programs that stop without closing (Alt-Break, an error); Wimp tasks are already closed on Service_WimpCloseDown.
  - Before 0.10, `V3DRender_Reset` reset the GPU but left queued jobs, and nothing closed a file whose program had gone.
- **A hard limit:** a job is reset after 5 s on the GPU even if its list keeps moving. Like Linux, the 0.5 s hang check skips the reset while CA/RA change, so a looping list would otherwise run for ever.
- **The log** says "Resetting GPU on request" for a reset that was asked for.
- **Tests:**
  - The simulated GPU checks the hard limit, `v3d_cancel_all` with a hung job and two queued behind it (all fences done, the BO freed after GEM_CLOSE), the same with nothing to do, and that the GPU works afterwards (1929 checks).
  - The emulator test checks `*V3DReset`, `*V3DReset all` closing a file left open, and `V3DRender_Reset` r0 = 1.
  - V3DJob's hang run (HangTest) adds test 10: a job that never ends and a frame queued behind it, ended at once by V3DRender_Reset; the queued frame isn't run, and the next frame is right.

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
