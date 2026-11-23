# V3DRender's request interface

`V3DRender_Ioctl` (r0 = file, r1 = request, r2 -> structure) takes
Linux's request numbers and structures, so that Mesa's v3d driver and
libdrm work as they do on Linux with a small layer:

```c
int drmIoctl (int fd, unsigned long request, void *arg)
{
  int r;
  _swix (V3DRender_Ioctl, _INR (0, 2) | _OUT (0), fd, request, arg, &r);
  if (r < 0) { errno = -r; return -1; }
  return 0;
}
```

where `fd` is the file from `V3DRender_Open` (or `RISC3D_Open` r1). The
request number carries the structure's size: a shorter structure (older
Mesa) has the rest taken as zero; only the caller's length is written
back. The result is 0 or a Linux errno, negated; no RISC OS error is
raised for those.

## Requests

| Request | As Linux? | Notes |
|---|---|---|
| `DRM_IOCTL_V3D_GET_PARAM` | yes | identity registers; TFU, CSD, cache flush 1; perfmon, multisync, CPU queue, super pages 0; `GLOBAL_RESET_COUNTER` (resets so far) and `CONTEXT_RESET_COUNTER` (resets this file's jobs caused), which current Mesa uses for robustness |
| `DRM_IOCTL_V3D_CREATE_BO` | yes | `offset` = GPU address. Zeroed. |
| `DRM_IOCTL_V3D_MMAP_BO` | differs | `offset` is the BO's address: already mapped, readable and writable by the program. libdrm's `mmap (0, size, ..., fd, offset)` returns it as it is; `munmap` does nothing. |
| `DRM_IOCTL_V3D_GET_BO_OFFSET` | yes | |
| `DRM_IOCTL_V3D_WAIT_BO` | yes | `timeout_ns` is relative and counted down |
| `DRM_IOCTL_V3D_SUBMIT_CL` | yes | flag `FLUSH_CACHE`; extensions and `perfmon_id` refused |
| `DRM_IOCTL_V3D_SUBMIT_TFU` | yes | V3D 7.1's `v71.ioc` |
| `DRM_IOCTL_V3D_SUBMIT_CSD` | yes | followed by a cache clean |
| `DRM_IOCTL_GEM_CLOSE` | yes | the BO goes when the GPU has finished with it |
| `DRM_IOCTL_GET_CAP` | `DRM_CAP_SYNCOBJ` only | |
| `DRM_IOCTL_SYNCOBJ_CREATE`, `_DESTROY`, `_RESET`, `_SIGNAL` | yes | |
| `DRM_IOCTL_SYNCOBJ_WAIT` | yes | `timeout_nsec` is absolute on V3DRender's clock: `OS_ReadMonotonicTime * 10,000,000` ns; `INT64_MAX` for ever. `WAIT_FOR_SUBMIT` on a sync object with no fence waits until the timeout (jobs are submitted at once, so none will come). |
| `DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD` | sync files only | `EXPORT_SYNC_FILE`: the "fd" is a number from 1 |
| `DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE` | sync files only | `IMPORT_SYNC_FILE` |
| `DRM_IOCTL_RISCOS_SYNC_FILE_WAIT` | RISC OS | for libsync's `sync_wait (fd, ms)`; ms -1 for ever |
| `DRM_IOCTL_RISCOS_SYNC_FILE_CLOSE` | RISC OS | for `close (fd)` on a sync file |
| performance monitors, `SUBMIT_CPU` | not yet | `-EINVAL` |

`V3DRender_Info` r4 is the buffer area's size in bytes (load option
`Size <MB>` or `V3DRender$Size`, 256 MB by default since 0.09); every BO
must fit in it. r8 points to the statistics, `struct v3d_stats` in
`core/v3d.h` (words, read only, added to at the end only), for tests.

The structures for the RISC OS requests are in `include/drm/drm.h`.

## Files

A file is a client: its BO handles and sync objects. `V3DRender_Close`
closes one; files opened by a Wimp task are closed when it ends
(Service_WimpCloseDown), and RISC3D closes its contexts' files.

## Through RISC3D

**The owner's rule: programs reach V3DRender only through RISC3D.** They
find it with `RISC3D_Info`, open it with `RISC3D_Open`, and send every
request to the ioctl SWI that RISC3D names. They never call V3DRender's
SWIs by number. riscos-mesa's libdrm layer does exactly this since
15 November 2026, and it has been checked on the Pi with no measurable
cost: 100 1080p ES 2.0 frames in 37 cs. V3DRender's own SWIs
(`V3DRender_Info`, `_Open`, `_Close`, `_Reset`, `_Debug`) are for its
own tools and test kits (V3DCheck, V3DJob, Replay) and for RISC3D.

RISC3D (0.02) lists V3DRender as a driver of the "DRM device" kind
(`RISC3D_Register` r4 bit 0, r5 = `V3DRender_Ioctl`, r6 -> "v3d").
Since 0.16, V3DRender's SWIs are in the allocated chunk &5AFC0
(`V3DRender_Ioctl` = &5AFC3), and it finds `RISC3D_Register` by name, so it
registers with RISC3D in either its provisional (&C3D00) or allocated
(&5AF80) chunk. Programs never see the change: they take the ioctl SWI
from `RISC3D_Info` r5.
Mesa's EGL can then:

1. `RISC3D_Info`: r3 non-zero, r4 bit 0 set: the driver is a DRM device,
   its ioctl SWI is r5, and Mesa's driver for it is named by r6 ("v3d").
2. `RISC3D_Open`: r1 is the file to give libdrm.
3. Run Mesa's v3d gallium driver on that file (as riscos-mesa-v3d does on
   the older module), with the layer above in place of libdrm's.
4. `RISC3D_Close` when the display is terminated.

## Frames the CPU reads back (0.11)

riscos-mesa reads every finished frame back from its BO into a sprite or
screen memory. Normal BOs are mapped non-cacheable for the CPU, so those
reads run at about 210-220 MB/s. Since 0.11 there are two ways round that.
Both are RISC OS only, in `include/drm/drm.h`.

**`DRM_RISCOS_BO_CPU_CACHED`** (`0x80000000`) in `drm_v3d_create_bo.flags`:

- The BO is mapped cacheable (write-back) for the CPU.
- V3DRender cleans its lines from the CPU's caches when a job that lists
  it is submitted.
- After such a job, it cleans and invalidates them (0.11-0.13: invalidated
  only) before the file's next request returns. So once a wait has seen the job finish, the CPU reads what the
  GPU wrote.
- Put the BO in the job's `bo_handles`, as Mesa does for every BO a job
  uses.

**`DRM_IOCTL_RISCOS_IMPORT_MEMORY`** (`'R'`, nr 2, `struct
drm_riscos_import_memory { __u64 address; __u32 size, handle, offset, flags;
}`; `flags` was `pad` in 0.11 and 0.12):

- Memory the program already has (a sprite, screen memory) becomes a BO.
  `address` and `size` must be multiples of 4 KB. It returns `handle` and
  `offset` (the GPU address).
- V3DRender looks up each page's physical address (OS_Memory 65) and maps
  them in the GPU's MMU. Nothing is copied or zeroed.
- `MMAP_BO` gives back the program's own address.
- The caches are kept as for a cached BO, unless `flags` has
  **`DRM_RISCOS_IMPORT_UNCACHED`** (1, since 0.13): the CPU doesn't cache
  this memory (screen memory, screen banks), so V3DRender does no cache
  maintenance for it. That saves a clean and an invalidate of every line
  of the frame on every job (8 MB at 1920x1080). Don't set it for memory
  the CPU caches (a sprite in the program's heap): the CPU would then read
  stale lines.
- Other `flags` bits are refused with `EINVAL`. 0.11 and 0.12 refuse any
  `flags` (as `pad`) with `EINVAL`, so a program can try the flag and,
  if refused, import again without it.
- `GEM_CLOSE` takes it out of the GPU's address space once no job uses it.
- **The memory must stay where it is until then:** the same logical and
  physical pages. It must also be mapped when the program calls V3DRender,
  because the cache maintenance is done by address in the program's
  context (application space included).
- V3DRender doesn't follow pages that RISC OS moves (Service_PagesUnsafe).
  Sprite areas, application space and screen memory don't move while in
  use.
- A `*V3DDump` lists imported BOs without their contents.

Mesa's v3d driver makes a render target from a GEM handle
(`resource_from_handle`, raster with a stride), so an imported sprite or
the screen can be drawn into directly.

## GPU time (0.13)

**`DRM_IOCTL_RISCOS_FILE_STATS`** (`'R'`, nr 3, read only, `struct
drm_riscos_file_stats { struct drm_riscos_queue_stats file[5], all[5]; }`,
each `{ __u64 busy_ns; __u32 jobs, pad; }`), as Linux's fdinfo engine
statistics:

- Per queue, in v3d_drm.h's order: bin, render, TFU, CSD, cache clean.
- `file`: the calling file's jobs that finished and their time on the GPU
  (from being put on it to its interrupt). `all`: everyone's since
  V3DRender started.
- The time comes from the ARM's generic timer (54 MHz on a Pi 4). Bin and
  render jobs of one frame overlap, so their times add up to more than the
  wall-clock time.
- `*V3DRender` shows everyone's, in ms.

A program that compares its GPU time with its frame time sees whether it
is waiting for the GPU or the GPU for it.

## Memory layout (0.13)

As Linux, BOs of 1 MB or more get a 1 MB-aligned GPU address and BOs of
64 KB or more a 64 KB-aligned one (placed from the top of the area down;
smaller ones first fit from the bottom), and V3DRender asks for physical runs
aligned the same way. The GPU's MMU then uses 1 MB and 64 KB entries for
them (fewer TLB misses on big textures and frames). `*V3DRender` shows how
much is in each. Nothing changes for programs: `DRM_V3D_PARAM_SUPPORTS_SUPER_PAGES`
isn't read by Mesa.

## Freed pages kept for reuse (0.25)

Nothing changes in the requests. When a program closes a BO
(`DRM_IOCTL_GEM_CLOSE`, or the last job using it is released), its pages stay
claimed and mapped where they were, up to 16 MB. They stay only while a
program other than the desktop's 2D has the GPU open. The next
`DRM_IOCTL_V3D_CREATE_BO` of the same kind (cached for the CPU or not) that
fits in them uses them with no `OS_Memory` or `OS_DynamicArea` call. It is
zeroed as before, so a new BO still starts all zeroes, as on Linux.

- New places and imported memory are put clear of the kept pages where there
  is room, so the pool stays usable.
- If RISC OS can't give a CREATE_BO its pages, the kept pages are given back
  and the claim is tried once more.
- When the last program closes its file, the kept pages go back to RISC OS.
- `*V3DRender pool <MB>` sets the limit (0 keeps none). `*V3DRender` shows
  how much is kept, how many BOs were made and how many came wholly from kept
  pages, and the average time of a CREATE_BO (and of its zeroing).
- The statistics (`struct v3d_stats`, r8 of `V3DRender_Info`) gain
  `bo_creates, bo_from_pool, pages_reused, pages_claimed` (`__u32`) and
  `create_ns, zero_ns` (`__u64`) at the end.

## Desktop 2D through RISC3D (0.28)

V3DRender registers with RISC3D flags bit 4 and answers driver reason 5:
r1 = -1 reads, 0 turns the desktop's 2D off, 1 on, 2 on without waiting;
r0 returns the state now. It is the same code as `*V3DRender 2D off|on|async`.
RISC3D's window uses it, and RISC3D calls it from a callback after
registration with the saved `RISC3D$2D`.

## No memory calls inside GraphicsV (0.26)

While V3DRender is handling a GraphicsV call (the desktop's copies and fills),
it claims and frees no RISC OS memory: each such call issues
Service_MemoryMoved to the Wimp in the middle of its redraw. Freed pages stay
kept, the binner's memory isn't topped up, and that work is done from a
transient callback, or at a program's next call into V3DRender. Nothing
changes for programs.

## After a hang (0.13)

As Linux (`drm_sched_resubmit_jobs`), only the job that hung is ended as
failed. Jobs then running on the other queues (for example the next
frame's bin job while a render job hangs) go back to the front of their
queues and run again from the start. `*V3DReset` still ends every job.

## The desktop's copies and fills (0.17)

Nothing changes in the requests. When the desktop's 2D work is on
(`*V3DRender 2D on`), V3DRender itself submits render jobs, from a file
of its own, for the desktop's copies and fills. A program's job may then
wait behind up to four of them (each a few hundred microseconds at most),
and their GPU time is in everyone's figures (`DRM_IOCTL_RISCOS_FILE_STATS`),
not the program's. The desktop's work never waits for a program's: while
a program has jobs queued or running, it is left to the CPU.

## RISC3D reason 4: hardware (0.20)

V3DRender registers with RISC3D with flags 9: bit 0 marks a DRM device,
and bit 3 means it answers reason 4. For reason 4, r1 points to a buffer
and r2 is its size. On exit r2 is the size needed, including the
terminating 0; the text is cut to fit. Each line is `label TAB value` and
ends with LF. The lines are GPU, Clock, GPU memory, Largest image and
Desktop 2D. Programs read it through `RISC3D_Describe` (RISC3D 0.09).
