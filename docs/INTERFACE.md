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

The structures for the RISC OS requests are in `include/drm/drm.h`.

## Files

A file is a client: its BO handles and sync objects. `V3DRender_Close`
closes one; files opened by a Wimp task are closed when it ends
(Service_WimpCloseDown), and RISC3D closes its contexts' files.

## Through RISC3D

RISC3D (0.02) lists V3DRender as a driver of the "DRM device" kind
(`RISC3D_Register` r4 bit 0, r5 = `V3DRender_Ioctl`, r6 -> "v3d").
Mesa's EGL can then:

1. `RISC3D_Info`: r3 non-zero, r4 bit 0 set: the driver is a DRM device,
   its ioctl SWI is r5, and Mesa's driver for it is named by r6 ("v3d").
2. `RISC3D_Open`: r1 is the file to give libdrm.
3. Run Mesa's v3d gallium driver on that file (as riscos-mesa-v3d does on
   the older module), with the layer above in place of libdrm's.
4. `RISC3D_Close` when the display is terminated.
