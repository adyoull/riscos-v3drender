# Changelog

## 0.33: the start-up log and diagnostic options at boot (23 November 2026)

Chris Hughes's CM4 Ne.X.T: V3DRender works when turned on from Configure's RISC3D window, and turns off cleanly, but the machine still stops ("trap while in trap handler") when V3DRender loads at boot. No log from it yet.

- **`V3DRender$Options`:** read when V3DRender is loaded with no options, as RISC3D's !Boot loads it. So NoIRQ, NoRISC3D, LowMem, NoWFI, Size and Log can be given at boot. Options in the command tail still win.
- **The log goes on past "ready":** after start-up, each of these is logged the first time it happens:
  - registering with RISC3D, and its answer;
  - RISC3D loaded again;
  - RISC3D's first call for each reason;
  - the first program opening the GPU;
  - the first mode change (starting and done);
  - the first Service_WimpCloseDown;
  - the first GPU interrupt (as in 0.27).

  Nothing is written from page-moving service calls. A machine that stops later in its boot now shows how far it got.
- **Tests:** emulator 101832 (the variable used only with no command tail; the later events logged once each). Host 5271.
- **Kit 36:** as kit 35, needing 0.33.

## 0.32: PRESENT's red and blue in 64K-colour modes (23 November 2026)

From MESA's first Pi test of 0.31 (riscos-mesa `tests/presenttest.c`, owner's Pi 4):
- **What worked:** 16M colours and 32K colours were right. The menu stayed on top. The rate was 565 frames a second against 66 with the CPU plot. 16,225 presents, no MMU errors, timeouts or resets.
- **What didn't:** in `C64K` (5:6:5, ModeFlags bit 7, bit 14 clear) red and blue were swapped. Blue came out as &001F, where it should be &F800.

- **Cause:** the GPU doesn't swap R and B for a 3-channel format.
  - 0.31 asked for the swap on the store to the 5:6:5 screen (BGR565), and the hardware ignored it.
  - Mesa's v3dv says the same ("we can't swap R/B on 3-channel images", commit 7f3e79ef9f1).
  - The format itself was right: BGR565 has red at the top, as Vulkan's R5G6B5_PACK16.
- **Fix:** the source's swap and the screen's are combined and done on a load in RGBA8 (4 channels), never on a store.
  - An RGB565 source that needs a swap is first copied whole into the staging buffer as RGBA8, as a UIF source is.
  - No store asks for a swap any more, for any screen.
- **Tests:**
  - The host model now ignores R/B swap on format 7, as the hardware does.
  - The 5:6:5 TBGR case is checked with an ABGR source, and with MESA's exact pixel (blue gives &F800). 0.31's code fails these.
  - cl2d's 1:5:5:5 TRGB list now expects no swap on the load or the store, as the two cancel.
  - Host 5271; emulator 92557.
- **Kit 35:** as kit 34, needing 0.32.

## 0.31: DRM_IOCTL_RISCOS_PRESENT, EGL's swap on the GPU (23 November 2026)

Asked for by MESA (EGL's window swap) and agreed by RISC3D: the GPU copies a program's finished frame to the screen, instead of EGL plotting a sprite with the CPU.

- **The ioctl:** `DRM_IOCTL_RISCOS_PRESENT`, `'R'` nr 4, `_IOC_WRITE`, 48 bytes. The struct is MESA's suggestion, with the padding before `rects` made explicit (`pad`, must be 0), and a third flag, `SRC_UIF_XOR`. In `include/drm/drm.h`; described in docs/INTERFACE.md.
- **Rectangles:**
  - Up to 256 a call, in screen pixels from the top left, each with its source pixel.
  - Each is clipped to the screen and to the source.
  - Each becomes one render job of 64x64 tiles: each tile is loaded from the source and stored to the screen, with no shaders and no binning, as the desktop's copies are. A frame too big for one list is split into bands of tile rows, each a job with the whole frame's header.
- **Formats:**
  - Sources: ABGR/XBGR8888 and ARGB/XRGB8888, raster or UIF (Mesa's tiled layout, plain or XOR); RGB565.
  - Screens: 32bpp TBGR/TRGB (alpha or not); 16bpp 5:6:5 (ModeFlags bit 7) and 1:5:5:5, each TBGR/TRGB.
  - The conversion is done by the load's and the store's own formats and R/B swaps. Alpha is forced to 1.
  - Other screens: -EINVAL.
- **UIF sources** can only be loaded from their start. So a UIF frame is first untiled whole into a raster staging buffer (kept, grown in MB), and the rectangles are copied from there.
- **The screen is read each call:**
  - the mode from the VDU variables;
  - the framestore from GraphicsV 9;
  - where the VDU writes from VDU variable 148, turned into a physical address with OS_Memory 65, so the bank OS_Byte 112 chose is followed.
  - A screen the CPU caches gives -EBUSY.
- **Ordering:**
  - The jobs go on the calling program's render queue, after its earlier jobs on the source (implicit) and after `in_sync`. `out_sync` is signalled when the last one finishes.
  - All rectangles clipped away: `out_sync` is signalled at once.
- **Other answers:**
  - `rect_count` 0 checks the screen (and the source, given a handle) without drawing.
  - `VSYNC`: -EOPNOTSUPP. It couldn't be done without waiting; EGL uses OS_Byte 19 itself.
  - -ENOTTY where it isn't offered.
- **Independent of desktop 2D:**
  - PRESENT has its own internal file: its lists (256 KB in four parts, each part reused once its last job has finished), its own mapping of the framestore (remapped when the framestore moves) and the staging buffer.
  - It's closed with the last program.
  - Internal files are no longer counted as programs anywhere: open files, `*V3DPower`, 2D's "program open" rule, and the pool.
- **The core:** `v3d_submit_render` (a render job from the core, with BOs by index) and `v3d_present_rcl` (v3d_2d.c, the per-tile list with separate load and store words).
- **`*V3DRender`** shows a "Presents:" line: rectangles, jobs, UIF frames untiled, checks, rectangles off the screen, and refusals with the last error.
- **Tests:**
  - Host 5268. The lists are carried out as the GPU would (formats, raster and UIF layouts, swaps, alpha) into a stand-in screen, and compared with one drawn pixel by pixel. Covered:
    - partial, off-screen and over-the-edge rectangles;
    - a second bank;
    - ARGB onto TRGB, XBGR onto TRGB, RGB565 onto 5:6:5 both orders, and onto 1:5:5:5 both orders;
    - a 100x70 UIF frame;
    - the errors, `out_sync` and `in_sync` ordering, the parts of the list buffer, a framestore move, and the file closed with the last program.
  - cl2d.py checks a 1:5:5:5 present list and a UIF untiling list against Mesa's packet XML (formats 45 and 27, memory format 4, height 9 UIF blocks, R/B swap, alpha 1).
  - Emulator 92557, through the module: the screen read from the VDU variables, GraphicsV 9 and OS_Memory 65, with the jobs started. Covered:
    - 32bpp, 5:6:5 and 1:5:5:5 TRGB;
    - 4K colours and 8bpp refused;
    - VSYNC, a cached screen, the VDU outside the framestore and a bad handle;
    - the status line;
    - no file left open.
- **Not checked on the Pi yet.** 16bpp UIF follows the same rule as 32bpp but has no model in the tests.
- **Kit 34:** as kit 33, needing 0.31.

## 0.30: the 128 MB area clamp; BO and handle tables that grow; refused buffers counted (23 November 2026)

From RISC3D, passing on David Sharp's ports (ioquake3, Mupen64Plus, dhewm3) on a Pi 4 with RISC OS 5.30 and V3DRender 0.26. Both claims checked against the code first.

- **The buffer area's real size.**
  - *Claim:* on RISC OS 5.30, unless DAClamp has run first, the area stops at 128 MB, and BOs past it fail with "claim any failed: OS error &1D1"; `*V3DRender` still shows 262,144 KB.
  - *Confirmed:* `pmp_create` took the size it asked for as the size it got. OS_DynamicArea 0 returns the clamped maximum in R5, which was ignored, so `area_pages` covered address space the area didn't have.
  - *Fixed:*
    - The area's size is now R5.
    - The clamp for areas with a given maximum (OS_DynamicArea 8, R2) is lifted just while V3DRender makes its area, then put back to what it was. Nothing else in the system changes.
    - If it can't be lifted (an older OS), V3DRender uses what it got.
    - `*V3DRender`'s "Buffer memory" says when RISC OS gave less than was asked for. V3DRender_Info r4 is the real size.
- **4,096 handles a file and 8,192 BOs.**
  - *Claim:* Doom 3 makes about 5,300 BOs; CREATE_BO returns -ENOSPC past 4,096 handles, and the game crashes.
  - *Confirmed:* fixed tables, `V3D_MAX_HANDLES` 4096, `V3D_MAX_BOS` 8192.
  - *Fixed:* both are now tables that start at 1,024 and double as needed, up to 65,535 (indices are 16 bits).
    - Growing is done in SVC: a new table, the old copied in, then the pointer swapped with interrupts off (the interrupt handler reads BOs) and the old freed.
    - A free-slot hint keeps finding a BO cheap with thousands in use.
- **Refused CREATE_BOs are counted and logged.**
  - Each is counted by reason: no handle, BO table full, no room in the area, or no memory from RISC OS. The first of each kind goes in the log, and the size of the last refusal is kept.
  - `*V3DRender` shows the BO table ("buffers (BOs): N of M (grows to 65535)") and a "refused" line when there are any.
  - Statistics gain `bo_refused, refused_handles, refused_table, refused_space, refused_memory, refused_last_bytes` at the end.
- **Tests:**
  - Host 5142: 1,500 BOs in one file, so both tables grow from 1,024 to 2,048 with every BO and its contents still where they were; refusals counted for a BO too big for the area, for RISC OS giving no memory, and for a file with no handle left.
  - Emulator 86752: RISC OS 5.30's 128 MB clamp emulated; the area gets its 256 MB and the clamp is put back; with a 64 MB clamp V3DRender can't lift, it uses 64 MB and says so in `*V3DRender` and Info r4.
- **Kit 33:** as kit 32, needing 0.30.

## 0.29: "Z-layer complex overlays" in Configure's RISC3D window (23 November 2026)

The owner asked for overlays layered as on Linux, in a way that stays compatible with VideoOverlay. On the native Pi this was already in the OS: BCMVideo (`s/GVOverlay`) creates VideoOverlay's overlays as firmware (dispmanx) layers.
- In a 32bpp alpha desktop mode they are Z-Order: at layer -192 and below, under the desktop (which the firmware puts at -127), seen through VideoOverlay's transparent hole. GraphicsV 28 orders several.
- Otherwise they are Basic, at layer 1024 over everything.
- BCMVideo offers alpha modes only with `framebuffer_ignore_alpha=0` in config.txt.
- Tested on the owner's Pi 4 through TestAgent, with config.txt backed up first:
  - before: Basic;
  - with the line, after a restart, in `C16M LABGR`: colour bars in a window, covered properly by a menu and a Filer window, and the desktop drawn correctly.
- The owner named it "Enable z-layer complex overlays".

- **Reason 4's hardware text gains a line**, so Configure's RISC3D window shows the real state:
  - `Z-layer complex overlays`, with one of:
    - "on: video overlays behind the desktop, under its windows and menus" (the desktop mode has the alpha flag);
    - "off: available in this mode's alpha form" (OS_CheckModeValid accepts the current mode with ModeFlags bit 15);
    - "off: needs framebuffer_ignore_alpha=0 in config.txt and a restart";
    - "off: needs a 16 million colour desktop".
  - From OS_ReadModeVariable and OS_CheckModeValid; nothing is changed.
- Turning it on and off is RISC3D's part (a handoff): the config.txt line and the alpha mode. V3DRender has no part in display overlays.
- **Tests:** emulator 77477 (all four states; the alpha form asked of OS_CheckModeValid is the current mode with &8000 added). Host 2129 (unchanged).
- **Kit 32:** as kit 31, needing 0.29.

## 0.28: desktop 2D switched from Configure's RISC3D window (23 November 2026)

From RISC3D, at the owner's request: turn the desktop's 2D on and off in Configure's RISC3D window rather than with a command (RISC3D 0.10, !RISC3DSetup 1.07, DRIVER-GUIDE "Desktop 2D").

- **Registers with flags bit 4** (`1 | 8 | 16`): "can do the desktop's 2D on the GPU and switch it while running". RISC3D 0.09 and older ignore the bit.
- **Driver reason 5, desktop 2D:**
  - In: r1 = -1 to read, 0 off, 1 on, 2 on without waiting.
  - Out: r0 = the state now (0, 1 or 2).
  - On failure, V set with an error: &823E0C if 2D was asked for and didn't start; &823E0D for a state it doesn't know, which changes nothing.
  - RISC3D calls it from RISC3D_Desktop2D in the window's task, and from a transient callback after V3DRender registers, with the saved `RISC3D$2D`.
- **One path:** `*V3DRender 2D off|on|async` and reason 5 now share `desktop_2d`, so `*V3DRender` and the window always agree.
  - `*V3DRender 2D on` (or `async`) now gives an error if 2D couldn't be started. Before, it silently stayed off.
  - Asking for the state already in force does nothing, except that `off` always releases GraphicsV, as after a GPU failure.
- `V3DRender$2D` and the `2D`/`2DAsync` load options are unchanged. A saved `RISC3D$2D` arrives later, so it wins.
- **Tests:** emulator 77472: registration flags 25; reason 5 reads off; on and async are claimed and agree with `*V3DRender`; `*V3DRender 2D off` is read back as off by reason 5; an unknown state gives an error and changes nothing; off after a GPU stop releases GraphicsV. Host 2129 (unchanged).
- **Kit 31:** as kit 30, needing 0.28.

## 0.27: a start-up log that survives a crash; diagnostic load options (23 November 2026)

From RISC3D: Chris Hughes's CM4 (R-Comp Ne.X.T) still gives "Internal error, trap while in trap handler: abort on instruction fetch at &00000000" at start-up with V3DRender 0.24. The earlier test build gave the same. RISC3D asked for a log that survives the crash and an option to skip the interrupt.

- **`Log <file>`, or `V3DRender$Log`:**
  - Each start-up step goes into the file as it begins: 1 checks, 2 map, 3 power, 4 probe, 5 memory, 6 init, 7 interrupt, 8 RISC3D, ready.
  - The file is opened, written and closed for each line, so it is complete up to the moment of a crash, and can be read after restarting without !RISC3D.
  - Each line has the monotonic time and the version. The file is created as Text and added to at every start-up.
  - It also records the load options and the RAM (OS_Memory 8), HAL_IRQClear's address and static base, and a failure's error number and text, before the clean-up. It records the first GPU interrupt, from a callback, since files can't be touched in the handler, and "final" when the module is killed.
- **`NoIRQ`:** the device vector isn't claimed and HAL_IRQEnable isn't called. Waits, and every V3DRender_Ioctl, poll the GPU's interrupt status instead, so programs still work. This separates an interrupt problem from a power or memory one.
- **`NoRISC3D`:** V3DRender doesn't register with RISC3D.
- **`LowMem`:** no OS_Memory 12 requests for pages above 1 GB; buffers and the page table come from anywhere.
- **Options are whole words.** 0.09-0.26 took any word starting "no" as NoWFI.
- **Tests:** emulator 77366:
  - every step, the options and RAM, and HAL_IRQClear in a Text file, appended;
  - the first interrupt noted once, and "final";
  - `V3DRender$Log`; a start-up failing at step 5 logs its error before the clean-up's "final (fatal)";
  - no file unless asked for;
  - NoIRQ: no vector, and a job that finishes with no interrupt is seen by polling;
  - NoRISC3D: RISC3D not called;
  - LowMem: no range request above 1 GB.
  - Host 2129 (unchanged).
- **Kit 30:** as kit 29, needing 0.27.

## 0.26: no memory calls inside GraphicsV; GFXH-1897 (22 November 2026)

From Rob Heaton's V3DDriver development log, read against V3DRender.

- **Nothing inside a GraphicsV call claims or gives back RISC OS memory.**
  - Each `OS_DynamicArea` 21/22 call tells the Wimp memory has moved (Service_MemoryMoved). Inside a GraphicsV call, the Wimp is still in the middle of its redraw. Rob found this was what froze V3DDriver whenever the Task Manager was open.
  - V3DRender's 2D buffers were always claimed at `*V3DRender 2D on`. But the desktop's copies and fills go through the driver's ordinary submit path, and its housekeeping could still claim or release memory: topping up the binner's overflow memory, giving back a finished program's buffers, and (0.25) giving back kept pages to make room for a new screen mapping.
  - Now, while V3DRender is inside GraphicsV (`no_page_ops`):
    - freed pages stay kept even past the pool's limit;
    - a new screen mapping goes only where no kept pages are, and if there is no such place the operation is left to the CPU;
    - nothing is claimed;
    - the binner's memory isn't topped up.
  - The postponed work runs from a transient callback (`OS_AddCallBack`) as RISC OS leaves SVC mode, or at a program's next call into V3DRender, whichever comes first. The callback is removed on `*RMKill`.
  - 0.21's rule (no desktop 2D while a program has the GPU open) hid most of this. Without this change, relaxing that rule would have exposed it.
- **GFXH-1897:** on V3D before 7.1, writing L2TCACTL while an L2T flush is still pending clobbers the flush. Cleaning the GPU's caches (after a compute job, and for the cache-clean queue) now waits for any pending L2T flush first. This is the same change as Linux's "drm/v3d: Wait for pending L2T flush before cleaning caches" (Igalia, May 2026, in a series to fix Pi 4 freezes). Rob made it in V3DDriver too. The series' other patches are for Linux's runtime power management, which V3DRender doesn't have.
- **Checked against Rob's log and already right:**
  - core and hub interrupts both acknowledged on the one level-triggered line, and the device vector always claimed;
  - a running V3D recognised from the hardware, not the firmware's clock flag (which always reads off);
  - the VDU write address forgotten at GraphicsV 2 and Service_PreModeChange;
  - one-pass scrolls in his per-axis order;
  - buffer pages mapped Normal non-cacheable (bit 5 alone);
  - a DSB before each job starts, and only SLCACTL's documented bits written.
  - Not needed: his 510-tile bands (a V3DRender list holds about 4,000 tiles; a 4K screen is 2,040).
- **Tests:**
  - Host 2129: inside GraphicsV a close past the limit keeps its pages; a BO needing claims is refused with nothing claimed or released, while one that fits kept pages is made; an import goes clear of kept pages; the overflow memory isn't topped up and trimming waits; outside, it is all done; the last program closing inside leaves its pages until outside. Also a simulated pending L2T flush that the clean must wait for. Removing either fix fails the tests.
  - Emulator 54154: a closed program's finished job, released by the desktop's next fill, makes no OS_DynamicArea call during the fill. One callback is asked for, not repeated while pending; it preserves every register and gives the pages back. Without the GraphicsV guard, 4 checks fail.
- **Kit 29:** as kit 28, needing 0.26.
- **Release notes** (`release-notes/v0.26.md`, after the tag; the module is unchanged): everything since 0.16, the last published release, with the owner's RISCOSmark results (Rectangle Copy 519 with 2D off, 3,679 with it on, at 1920x1200). The README's status is brought up to 0.26.
- **Credit to Rob Heaton** (after the tag; the module builds byte-for-byte the same): `riscos/module.c`'s header now says its interrupt handling (device 10 with the private word, HAL_IRQClear called directly) and the refusal to load beside V3DDriver follow V3DDriver, and that 0.26's GraphicsV rule comes from his log. NOTICE item 3 says the same. The other files taken from his work already said so: `riscos/plat_pi4.c`, `core/v3d_2d.c` and `riscos/twod.c`.

## 0.25: freed GPU memory kept for the next buffer (22 November 2026)

From riscos-mesa and the web browser (Strada's profile on the Pi, 22 Nov). About 5% of the web page task's busy time was in `DRM_IOCTL_V3D_CREATE_BO`, the largest item in the GPU library. WebKit and Skia make and drop textures and render targets all the time while a page scrolls.

- **Why each one cost so much:** every CREATE_BO claimed its pages from RISC OS (`OS_Memory 12` for runs above 1 GB, then `OS_DynamicArea` 21/22 claim and map), and every GEM_CLOSE gave them back. Those specific-page claims are also what makes RISC OS move other memory's pages: the "Pages moved by the OS" that 0.23 follows, each of which makes the GPU wait.
- **The page pool:**
  - A freed BO's pages now stay claimed and mapped where they are, up to 16 MB, while a program other than the desktop's 2D has the GPU open. Their GPU page-table entries are still removed.
  - The next BO of the same kind (cached for the CPU or not) that fits in kept pages uses them, with no OS call. The search tries 1 MB- and 64 KB-aligned places first for big BOs, then any.
  - Kept pages of the other kind in a BO's place are given back and claimed again with the right mapping.
  - New places and imported memory go clear of the kept pages where there is room.
  - If RISC OS can't supply a BO's pages, the kept pages go back and the claim is tried once more.
  - When the last program closes, everything kept goes back. `*V3DRender pool <MB>` sets the limit (0 keeps none).
- **Zeroing:** a new BO still starts all zeroes, as on Linux. The pages are now written in 64-byte STM bursts, which suit the uncached (bufferable) mapping better than memset's single words.
- **A BO still used by a queued or running job** already kept its pages and PTEs until that job was released (since 0.01: jobs hold references). Mesa's third suggestion needed no change.
- **`*V3DRender` shows:**
  - "kept for reuse (KB)" and its limit;
  - "Buffers made": how many, how many came wholly from kept pages, pages reused and claimed;
  - the average CREATE_BO time and its zeroing part.
  - Configure's RISC3D window (reason 4) adds the kept amount to "GPU memory".
  - The statistics block gains `bo_creates, bo_from_pool, pages_reused, pages_claimed, create_ns, zero_ns` at its end.
- **Tests:**
  - Host 2103: reuse in place with no claim and zeroed; a smaller BO in a bigger freed one; cached and uncached kept apart; the limit; a failed claim gives the pool back and succeeds on the retry; two failures give ENOMEM with nothing kept; nothing kept with only the 2D file open; everything back at close.
  - Emulator 54122: GEM_CLOSE keeps the pages, and the next BO gets them zeroed with no claim; nothing kept after the last close; `*V3DRender pool`.
- **Kit 28:** as kit 27, needing 0.25.

## 0.24: start-up that says how far it got; safer when it fails (22 November 2026)

For the Compute Module 4 in an R-Comp Ne.X.T (Chris Hughes, via RISC3D). At start-up with !RISC3D it reports "Internal error, trap while in trap handler: ... abort on instruction fetch at &00000000". It probably has PackMan's V3DRender 0.15 or 0.16, from before 0.20's power-domain fix.

- **`V3DRender$Step`:** start-up sets it as it goes: `1 checks`, `2 map`, `3 power`, `4 probe`, `5 memory`, `6 init`, `7 interrupt`, `8 RISC3D`, `ready`. If a machine stops during start-up, `*Show V3DRender$Step` afterwards says which step it was.
- **A start that fails part way no longer tears down a GPU it never set up.** If the buffer area or page table couldn't be made, older versions ran the full shutdown (v3d_fini) on a device that was never initialised. Now only the V3D's interrupts are turned off.
- **The interrupt handler** runs V3DRender's code only once the device is set up, and never calls a null HAL routine. If `OS_Hardware` gives no HAL_IRQClear, start-up stops with an error rather than jumping to 0 at the first interrupt.
- **Tests:** emulator 54119:
  - V3DRender$Step follows every step to "ready";
  - with no buffer area at all, start-up fails at "5 memory", returns the error, and leaves nothing claimed (no device vector, no areas).

## 0.23: imported memory follows pages the OS moves (21 November 2026)

- **The likely cause of the web browser's broken GPU drawing** (render jobs hanging, frames lost, MMU errors, and with 2D off too):
  - RISC OS can move a physical page of any memory that isn't locked. It copies the page and remaps its logical address, for example to give a dynamic area the specific pages it asked for. V3DRender's own `OS_Memory 12` claims for buffers above 1 GB ask for exactly that.
  - Memory a program imports (`DRM_IOCTL_RISCOS_IMPORT_MEMORY`, the browser's EGL_RISCOS_image_memory) is used by the GPU through the physical pages it had at import. After a move, the GPU kept writing the old pages, which now belonged to someone else.
  - On the owner's Pi, a hung render list was found overwritten with pixel words (&FFFFFEFF ...): a later V3DRender buffer had been given a page the browser's frames still mapped.
- **The fix: Service_PagesUnsafe / PagesSafe**, the OS's own notice for anything doing DMA, and their 64-bit forms &E3 / &E4.
  - On *Unsafe*, if any imported BO uses one of the pages, no new job starts. The jobs running are waited for (the interrupt polled, up to 1 s).
  - On *Safe*, those BOs' pages take their new physical addresses, their GPU page table entries are rewritten, and jobs start again.
  - Inside a 64-bit pair, the 32-bit calls are ignored: they list only some of the same pages.
  - `*V3DRender` shows "Pages moved by the OS" when it has happened.
- **Tests:** emulator 54105, with an imported page moved by Unsafe64 and Safe64:
  - a job submitted in between doesn't start until Safe64;
  - the 32-bit Safe inside the pair changes nothing;
  - the moved page's PTE points at its new place and the others stay;
  - the counter.
- **Kit 26:** as kit 25, needing 0.23.

## 0.22: `*V3DRender` could abort when showing an MMU error (21 November 2026)

- **The fault:** the MMU error line's client name ("L2T", "TLB", ...) and the 2D line's reason why the screen can't be used came from tables of string pointers. The module isn't relocated, so those pointers were link-time addresses.
  - Printing one read whatever memory that was: on the owner's Pi the browser report showed "client d ãitã=".
  - Once the browser had closed and that memory was unmapped, `*V3DRender` aborted in OS_Write0: "Internal error: abort on data transfer at &FC033AD8".
- **The fix:** both are switches now, and no other tables of pointers are left in the module.
- **Tests:** emulator 54099, including an MMU error (PTI, client 3, page &1234) shown as "client TLB". The old module, given the same test, reads unmapped memory.
- **Kit 25:** as kit 24, needing 0.22.

## 0.21: no desktop 2D on the GPU while a program has it open (21 November 2026)

- The web browser (Strada, WebKit compositing on the GPU into imported memory) reported that with `*V3DRender 2D on`, its GPU drawing went wrong:
  - render jobs hung and were reset (8 timeouts, 7 resets);
  - there were MMU errors at GPU page 0;
  - frames were lost: missing and stale regions, tiles of noise.
  - With 2D off, the same build drew right.
  - The 2D work was 407 fills on the GPU, never declined for 3D work ("0 GPU busy with 3D"). In waited-for mode a 2D job runs alone between the browser's jobs, so the cause isn't simply two jobs on the GPU at once, and it isn't found yet.
- **Until it is found, the desktop's copies and fills stay on the CPU while any program has a V3DRender file open.** V3DRender's own 2D file doesn't count. `*V3DRender` counts these as "a program using the GPU".
- `*V3DRender 2D alongside on|off` (default off) allows 2D beside programs again, for finding the cause.
- **Tests:** emulator 54094: with a program's file open a fill is left to the CPU; alongside on, it's claimed; after the program closes, it's claimed; the counter.

## 0.20: hardware facts for Configure's RISC3D window; V3D power domain first (21 November 2026)

- **RISC3D reason 4, "hardware"** (RISC3D 0.09, DRIVER-GUIDE §5). The owner asked that Configure's RISC3D window report what the GPU supports without a program running.
  - The GL figures come from riscos-mesa's GPUProbe.
  - V3DRender registers with flags bit 3 and answers reason 4 with lines of `label TAB value LF`:
    ```
    GPU            V3D 4.2, revision 14 (the Pi 4's VideoCore VI)
    Clock          250 MHz now (up to 500 MHz, raised while programs draw)
    GPU memory     2048 KB in use, all above 1 GB, of 256 MB
    Largest image  4096 x 4096 (textures and render targets)
    Desktop 2D     off (*V3DRender 2D on)
    ```
  - r1 points to a buffer and r2 is its size; r2 comes back as the size needed. The text is cut to fit and always ends with a 0. RISC3D 0.08 and older ignore flags bit 3.
- **The firmware's V3D power domain is turned on before any V3D register is read.**
  - RISC3D passed on a report (Chris Hughes, Compute Module 4): "Internal abort" at various locations at start-up with !RISC3D, and no V3DDriver installed.
  - 0.19 and older first read the core's IDENT0 to see whether the V3D was already running. They turned the domain on only after that, and only if it wasn't. If the firmware leaves the reset released and the bus gates open but the domain off, that read can end in an asynchronous external abort, reported later wherever the CPU happens to be.
  - Now a domain the firmware reports off is turned on first. V3DRender leaves it on, so on a reload it is already on and nothing changes. A firmware without the domain interface answers ~0, and nothing changes there either.
  - This is a likely cause of the CM4 abort, but it isn't confirmed.
- **Tests:** emulator 54082 checks:
  - registration flags 9;
  - reason 4's size query, its full text, and a 20-byte cut with nothing written past it;
  - a V3D whose domain is off, with the reset released and the gates open: 0.19 read its registers 34 times before turning the domain on; 0.20 reads none.
- **Kit 23:** as kit 22, needing 0.20.

## 0.19: 2D measured on the Pi; limits from the measurements (20 November 2026)

Measured on the owner's Pi 4 through TestAgent: native RISC OS 5, 1920x1200 at 32bpp, ARM at 1800 MHz.

- **Where an op's time goes.** V3DRender's own cost is about 5 us an op: building the list under 1 us, submitting 1 us, the checks the rest. A GPU job costs about 5 us plus about 4 us a 64x64 tile; the clock (250 or 500 MHz) makes no difference. Large copies and fills are memory-bound, and the GPU is 1.3 to 2 times the CPU, which copies the screen at about 4 GB/s itself.
- **Not waited for (async):** a claimed op costs the caller 4 to 7 us, whatever its size.
- **New default limits, from the measurements.** The GPU is level with the CPU at 16x16 and ahead from there, so:
  - copies from 1,024 bytes and 8 rows (Rob's V3DDriver used 2,048 and 16);
  - fills from 4,096 bytes and 16 rows, with no rule per tile (Rob's used 16,384, 32, and `bytes + 15 x rows >= 4,500 x tiles`).
  - `*V3DRender 2D limits` with no numbers now goes back to these.
- **`*V3DRender`** shows the time per claimed op: in GraphicsV, list, submit and wait, in microseconds. It also shows the time per op left to the CPU. The core keeps `us_build`, `us_submit` and `us_wait`.
- **`*V3DRender 2D zero`** sets the 2D counters and times back to 0.
- **Kit 22:**
  - Time2D (Obey, runs Time2DProg) times 15 sizes of copy and fill on the CPU and on the GPU, with the limits off, plus three not waited for. It writes the wall time per op and V3DRender's split to Time2DOut.
  - Draw2D's labels no longer say which sizes the CPU takes.
  - Check, Desk2D and Time2D need 0.19.
- **Tests:** emulator 49443 checks:
  - a 32x32 copy is claimed under the new limits and a 15x15 one isn't;
  - the timing line;
  - `2D zero`.

## 0.18: faster desktop 2D (20 November 2026)

The owner asked for the 2D work to be optimised wherever possible.

- **Command lists written a word at a time.** Each tile's commands are built once and copied for every tile, changing only its position. The list buffer isn't cached by the CPU, so this means a quarter as many memory writes. A copy's tile is 32 bytes, eight word writes. A fill's tile is padded with a NOP to 20 bytes, and its clearing tile to 24, so every tile stays word-aligned. A list built at an unaligned address is still built byte by byte, and the tests check both give the same list.
- **Bank flips are cheap.** A program that flips between two screen banks each frame tells the driver through GraphicsV 6. Before, each flip made the next copy read the VDU variables, ask GraphicsV 9 and check the screen mapping again. Now only the write position moves; the cached check on dynamic area 2 is still made. The framestore is remembered from the last full check, and a flip to the same bank costs nothing. `*V3DRender` counts bank changes.
- **`*V3DRender 2D limits [copy-bytes copy-rows fill-bytes fill-rows fill-bytes-a-tile]`** sets the sizes below which the CPU does the work; with no numbers it goes back to Rob's. `*V3DRender` shows the limits when they aren't Rob's. This is for finding where the GPU starts to win on this driver, whose fills cost half the GPU commands of Rob's.
- **Kit 21:** Draw2D adds part 4, which turns Rob's limits off and times small copies and fills (32x16 up to 256x128 and thin strips) 500 times each. It shows microseconds per operation for the CPU and the GPU; those figures will set new limits. Times now show microseconds each. Check and Desk2D need 0.18.
- **Tests:**
  - sim 2063: the padded lists are the same built aligned and unaligned, and too small a buffer gives 0;
  - cl2d.py: the fill's NOP padding;
  - emulator 49435: three bank flips with no GraphicsV 9 call, each reading its own bank; a cached screen noticed at a bank change; lowered limits taking a 10x10 copy, three numbers refused, and the reset.

## 0.17: the desktop's copies and fills on the GPU (20 November 2026)

The owner asked for Rob Heaton's V3DDriver 2D acceleration inside V3DRender, since the two can't share the GPU. V3DRender now does it itself, written again after V3DDriver (GPL-2.0-or-later; see NOTICE 3), through V3DRender's own queues. **Off by default.**

- **GraphicsV 13 (Render), op 1, copies:** window moves, scrolls and `OS_Plot` block copies. Each one is a single render job: for each 64x64 tile, load the tile buffer from the source and store it to the destination. There's no binning and no shaders. Overlapping copies (scrolls) take one pass, visiting the tiles ahead of the move first (Rob's order).
- **Op 2, solid fills:** taken when every ECF line's OR is all ones and every EOR is the same, which is how `GCOL 0` fills look. The tile buffer is cleared to the colour once, on the first tile (Rob's clear packets); every tile then only stores it. With nothing loaded, the buffer keeps the colour, so this needs half the packets of V3DDriver's clear-and-store for each tile. Patterned fills and other GCOL actions are left to the CPU.
- **Rob's measured limits:**
  - copies need 2,048 bytes and 16 rows;
  - fills need 16,384 bytes, 32 rows, and `bytes + 15 x rows >= 4,500 x tiles`;
  - anything smaller is left to the CPU, which is quicker there.
  - 32bpp is RGBA8 (format 27); the Pi's 16bpp is BGR565 (format 7). At 16bpp a fill colour is widened to 8 bits a channel the way Rob does it, which narrows back exactly.
- **Through V3DRender's scheduler:** a copy or fill never meets a program's 3D job on the hardware. While a program has GPU work queued or running, the desktop work is left to the CPU rather than waiting behind it. The framestore (GraphicsV 9) is mapped into the GPU's MMU once a mode, from its 1 MB boundary, so it uses 1 MB entries. The lists go into one 128 KB buffer kept for good, so nothing is claimed or zeroed per copy.
- **Left to the CPU** when:
  - hardware acceleration is turned off (`OS_ScreenMode 4` bit 2);
  - the screen is cached by the CPU (dynamic area 2 is cacheable): the GPU would miss what is still in the cache;
  - the VDU draws to a bank the display isn't showing, unless GraphicsV 6 said where;
  - interrupts are off, or the screen isn't 16 or 32bpp.
  The mapping is forgotten at GraphicsV 2 and `Service_PreModeChange`. The bank is followed through GraphicsV 6 (R0 = 3). Driver 0, head 0 only. A GPU failure turns 2D off, as V3DDriver does: everything is passed on from then. GraphicsV is released only by `*V3DRender 2D off` or finalisation, not inside its own call while the kernel walks its claimants.
- **Turning it on:**
  - `*V3DRender 2D on` waits for each copy;
  - `*V3DRender 2D async` doesn't wait, and honours GraphicsV's "synchronise" flags. Experimental: a program that reads the screen directly without asking could see it before the GPU is done.
  - `*V3DRender 2D off`; `*V3DRender 2D fills on|off`.
  - At load: the option `2D` or `2DAsync`, or `V3DRender$2D` set to `on` or `async`.
- **Not waited for, four lists in flight:** the buffer is split in four, so up to four copies and fills queue on the GPU while the CPU goes on. A fifth waits for the first four. A list too big for a quarter (a screen of more than about 1,000 tiles) waits for all of them and uses the whole buffer.
- **`*V3DRender`** shows the 2D state and the reason the screen can't be used, if any. It shows the copies and fills by the GPU out of those offered (scrolls, small, patterned), the GPU's tiles, time and waits, and what was left to the CPU and why.
- **Faster submits for every program:** a job's list of BOs is kept in the job itself when there are up to 48 of them, so most submits no longer claim and free RMA (Mesa's jobs usually have fewer than 48 BOs).
- The 2D file belongs to V3DRender: it isn't counted in `V3DRender_Info` r6, `*V3DPower cycle`'s check or the "Files open" line. `*V3DReset all` closes it with the rest, and it is opened again.
- **Tests:**
  - **sim 2060 checks:**
    - mapping (1 MB entries) and bounds;
    - the job is render-only;
    - 800 random copies at both depths and 600 random fills, carried out tile by tile and compared with the CPU's result, so overlapping copies must equal memmove in every direction;
    - every 16bpp colour survives the widening;
    - the four-list ring, a fifth waiting, and a list bigger than a quarter;
    - declined while a 3D job runs; a reset fails the wait.
  - **`test/host/cl2d.py`** decodes the copy, scroll, 16bpp and fill lists with Mesa's `v3d_packet.xml` and checks every packet, tile order and address.
  - **Emulator 49411:**
    - GraphicsV claimed and released;
    - small copies, head 1 and other reasons passed on, and claims return with r4 = 0;
    - the list in the buffer reads the framestore through the GPU's MMU;
    - both banks, a cached screen and acceleration off;
    - `Service_PreModeChange` unmaps;
    - solid, patterned, partial and small fills;
    - a copy the GPU hangs on: reset, 2D stops itself and passes everything on, with GraphicsV still claimed until `*V3DRender 2D off`;
    - the commands and the status lines.
- **Kit 20:** `Desk2D` (which runs the BASIC `Draw2D`) draws each copy and fill on part of the screen twice, with acceleration off (the CPU) and with 2D on. It checks the two are the same, times both 100 times, and does the same not waited for. It writes `Results2D`. Check loads 0.17.

## 0.16: allocated SWI chunk and error block (18 November 2026)

RISC OS Open allocated V3DRender's numbers. This is the only change.

- **SWI chunk &5AFC0** (was the provisional &C3D40): `V3DRender_Info` &5AFC0, `_Open` &5AFC1, `_Close` &5AFC2, `_Ioctl` &5AFC3, `_Reset` &5AFC4, `_Debug` &5AFC5. The names are unchanged.
- **Error block &823E00** (was &81D340): each error keeps its offset (&823E00 "Not a V3DRender file" … &823E0A).
- **RISC3D_Register is found by name** (OS_SWINumberFromString). RISC3D's own chunk moves from the provisional &C3D00 to its allocated &5AF80, and V3DRender registers with either. The number it registered with is kept for deregistering.
- Reserved names (module, `*V3DRender`, `*V3DClock`, `*V3DDump`, `*V3DPower`, `*V3DReset`) are the ones V3DRender already uses.
- Programs are unaffected: they take the ioctl SWI from `RISC3D_Info` r5.
- **Kit 18:** V3DJob calls V3DRender's and RISC3D's SWIs by name, so it works with old and new chunks of either. Test 7 checks the ioctl SWI RISC3D reports is `V3DRender_Ioctl`'s number, whatever it is. BASIC V3DCheck already used names.
- **Tests:** emulator 44412 (the module's chunk word; registration through the name; new error numbers in the refusal tests).
- **Kit 19** (same module): the kit's RISC3D is now 0.05, the one with the allocated numbers (&5AF80; built unchanged from RISC3D's 204bca7), and Check needs RISC3D 0.05. RISC3D asked for this (`2026-11-18-risc3d-allocated-chunk-5AFC0-register-5AF81.md`): RISC3D 0.05 answers only its new numbers. V3DRender 0.16 already registers at &5AF81 through the name.

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
