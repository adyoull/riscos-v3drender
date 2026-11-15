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
- After such a job, it invalidates them before the file's next request
  returns. So once a wait has seen the job finish, the CPU reads what the
  GPU wrote.
- Put the BO in the job's `bo_handles`, as Mesa does for every BO a job
  uses.

**`DRM_IOCTL_RISCOS_IMPORT_MEMORY`** (`'R'`, nr 2, `struct
drm_riscos_import_memory { __u64 address; __u32 size, handle, offset, pad;
}`):

- Memory the program already has (a sprite, screen memory) becomes a BO.
  `address` and `size` must be multiples of 4 KB. It returns `handle` and
  `offset` (the GPU address).
- V3DRender looks up each page's physical address (OS_Memory 65) and maps
  them in the GPU's MMU. Nothing is copied or zeroed.
- `MMAP_BO` gives back the program's own address.
- The caches are kept as for a cached BO.
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

