/* drm.h: the small part of Linux's DRM interface that V3DRender takes.

   V3DRender_Ioctl takes the same request numbers and structures as Linux's
   v3d driver (v3d_drm.h, unchanged from Linux) and its generic GEM and sync
   object calls, so that Mesa's v3d driver and libdrm need no more than a
   thin layer (drmIoctl -> SWI) on RISC OS. Request numbers carry the
   structure's size, as on Linux: a shorter (older) structure is accepted
   and the rest is taken as zero, and a longer one has the extra ignored.

   The numbers and layouts here are Linux's (include/uapi/drm/drm.h, MIT
   licence); the definitions are written for RISC OS.

   SPDX-License-Identifier: MIT
   Copyright (c) 2026 Andrew Youll. */
#ifndef V3DR_DRM_H
#define V3DR_DRM_H
#include <stdint.h>

typedef uint8_t __u8;
typedef uint16_t __u16;
typedef uint32_t __u32;
typedef int32_t __s32;
typedef uint64_t __u64;
typedef int64_t __s64;

#define _IOC_NRBITS 8
#define _IOC_TYPEBITS 8
#define _IOC_SIZEBITS 14
#define _IOC_NRSHIFT 0
#define _IOC_TYPESHIFT (_IOC_NRSHIFT + _IOC_NRBITS)
#define _IOC_SIZESHIFT (_IOC_TYPESHIFT + _IOC_TYPEBITS)
#define _IOC_DIRSHIFT (_IOC_SIZESHIFT + _IOC_SIZEBITS)
#define _IOC_NONE 0u
#define _IOC_WRITE 1u
#define _IOC_READ 2u
#define _IOC(dir, type, nr, size) \
  (((dir) << _IOC_DIRSHIFT) | ((type) << _IOC_TYPESHIFT) \
   | ((nr) << _IOC_NRSHIFT) | ((size) << _IOC_SIZESHIFT))
#define _IOC_NR(r) (((r) >> _IOC_NRSHIFT) & 0xff)
#define _IOC_TYPE(r) (((r) >> _IOC_TYPESHIFT) & 0xff)
#define _IOC_SIZE(r) (((r) >> _IOC_SIZESHIFT) & 0x3fff)
#define _IOC_DIR(r) (((r) >> _IOC_DIRSHIFT) & 3)

#define DRM_IOCTL_BASE 'd'
#define DRM_IO(nr) _IOC (_IOC_NONE, DRM_IOCTL_BASE, nr, 0)
#define DRM_IOR(nr, t) _IOC (_IOC_READ, DRM_IOCTL_BASE, nr, sizeof (t))
#define DRM_IOW(nr, t) _IOC (_IOC_WRITE, DRM_IOCTL_BASE, nr, sizeof (t))
#define DRM_IOWR(nr, t) _IOC (_IOC_READ | _IOC_WRITE, DRM_IOCTL_BASE, nr, sizeof (t))
#define DRM_COMMAND_BASE 0x40
#define DRM_COMMAND_END 0xA0

struct drm_gem_close { __u32 handle, pad; };
struct drm_get_cap { __u64 capability, value; };
struct drm_syncobj_create { __u32 handle, flags; };
#define DRM_SYNCOBJ_CREATE_SIGNALED (1 << 0)
struct drm_syncobj_destroy { __u32 handle, pad; };
struct drm_syncobj_handle { __u32 handle, flags; __s32 fd; __u32 pad; };
#define DRM_SYNCOBJ_FD_TO_HANDLE_FLAGS_IMPORT_SYNC_FILE (1 << 0)
#define DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_EXPORT_SYNC_FILE (1 << 0)
struct drm_syncobj_wait
{
  __u64 handles;
  __s64 timeout_nsec;		/* absolute: see V3DRender's clock below */
  __u32 count_handles, flags, first_signaled, pad;
};
#define DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL (1 << 0)
#define DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT (1 << 1)
struct drm_syncobj_array { __u64 handles; __u32 count_handles, pad; };

#define DRM_IOCTL_GEM_CLOSE		DRM_IOW (0x09, struct drm_gem_close)
#define DRM_IOCTL_GET_CAP		DRM_IOWR (0x0c, struct drm_get_cap)
#define DRM_IOCTL_SYNCOBJ_CREATE	DRM_IOWR (0xBF, struct drm_syncobj_create)
#define DRM_IOCTL_SYNCOBJ_DESTROY	DRM_IOWR (0xC0, struct drm_syncobj_destroy)
#define DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD	DRM_IOWR (0xC1, struct drm_syncobj_handle)
#define DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE	DRM_IOWR (0xC2, struct drm_syncobj_handle)
#define DRM_IOCTL_SYNCOBJ_WAIT		DRM_IOWR (0xC3, struct drm_syncobj_wait)
#define DRM_IOCTL_SYNCOBJ_RESET		DRM_IOWR (0xC4, struct drm_syncobj_array)
#define DRM_IOCTL_SYNCOBJ_SIGNAL	DRM_IOWR (0xC5, struct drm_syncobj_array)

#define DRM_CAP_SYNCOBJ 0x13

/* RISC OS only (type 'R'): sync files. On Linux an exported fence is a
   file descriptor; here it is a number from 1 that V3DRender keeps until
   it is closed. libdrm's layer turns sync_wait (fd, ms) and close (fd)
   into these. */
struct drm_riscos_sync_file { __s32 fd; __s32 timeout_ms; __u32 signalled, pad; };
#define DRM_RISCOS_BASE 'R'
#define DRM_IOCTL_RISCOS_SYNC_FILE_WAIT \
  _IOC (_IOC_READ | _IOC_WRITE, DRM_RISCOS_BASE, 0x00, sizeof (struct drm_riscos_sync_file))
#define DRM_IOCTL_RISCOS_SYNC_FILE_CLOSE \
  _IOC (_IOC_WRITE, DRM_RISCOS_BASE, 0x01, sizeof (struct drm_riscos_sync_file))

/* RISC OS only (0.11), riscos-mesa's two requests for reading frames:

   - DRM_RISCOS_BO_CPU_CACHED in drm_v3d_create_bo.flags: the BO is
     mapped cacheable for the CPU. V3DRender cleans its lines from the
     CPU's caches when a job using it is submitted, and invalidates them
     after the job (0.14: cleans and invalidates), before the file's next request returns (so after the
     wait that saw it finish).
   - DRM_IOCTL_RISCOS_IMPORT_MEMORY: memory the program already has (a
     sprite, screen memory), page-aligned, becomes a BO in the GPU's
     address space: a handle and its GPU address. It is not copied or
     zeroed. MMAP_BO gives the program's own address back. Caches are
     kept as for a cached BO. GEM_CLOSE takes it out of the GPU's address
     space once no job uses it. The memory must stay where it is (same
     logical and physical pages) until then, and be mapped in the context
     of the program's V3DRender calls. */
#define DRM_RISCOS_BO_CPU_CACHED 0x80000000u
/* address: a pointer, 64 bits as Linux's uAPI passes them; offset: the
   GPU address (as CREATE_BO's) */
struct drm_riscos_import_memory { __u64 address; __u32 size, handle, offset, flags; };
#define DRM_IOCTL_RISCOS_IMPORT_MEMORY \
  _IOC (_IOC_READ | _IOC_WRITE, DRM_RISCOS_BASE, 0x02, sizeof (struct drm_riscos_import_memory))
/* (0.13) flags (the word was "pad" in 0.11 and 0.12, which refuse anything
   but 0 with EINVAL, so a program can try the flag and drop it):
   DRM_RISCOS_IMPORT_UNCACHED: the CPU doesn't cache this memory (screen
   memory, screen banks), so V3DRender does no cache maintenance for it. */
#define DRM_RISCOS_IMPORT_UNCACHED 1u

/* (0.13) DRM_IOCTL_RISCOS_FILE_STATS: time on the GPU, as Linux's fdinfo
   engine statistics. Per queue (V3D_BIN, V3D_RENDER, V3D_TFU, V3D_CSD,
   V3D_CACHE_CLEAN): the jobs finished and their time from being put on
   the GPU to finishing (ns), for the calling file and for everyone since
   V3DRender started. */
struct drm_riscos_queue_stats { __u64 busy_ns; __u32 jobs, pad; };
struct drm_riscos_file_stats
{
  struct drm_riscos_queue_stats file[5], all[5];
};
#define DRM_IOCTL_RISCOS_FILE_STATS \
  _IOC (_IOC_READ, DRM_RISCOS_BASE, 0x03, sizeof (struct drm_riscos_file_stats))

/* (0.31) DRM_IOCTL_RISCOS_PRESENT: copy a program's finished frame (a BO)
   to the screen on the GPU, as a render job on the program's queue: EGL's
   swap for a window on the RISC OS desktop. rects: rect_count of
   struct drm_riscos_present_rect, in screen pixels from the top left, each
   with where in the source it comes from; each is clipped to the screen
   and to the source. The screen is read afresh each call: the current
   mode and the bank the VDU writes to (OS_Byte 112). Taken screens:
   32bpp (TBGR or TRGB, alpha or not; the top byte is written FF), 16bpp
   5:6:5 and 1:5:5:5 (TBGR or TRGB); others -EINVAL. Sources: the fourccs
   below, raster (uif_height_or_stride: bytes a row) or UIF
   (DRM_RISCOS_PRESENT_SRC_UIF; uif_height_or_stride: the image's padded
   height in pixels, as Mesa's slice; _SRC_UIF_XOR for the XOR form).
   rect_count 0 checks the screen (and the source, if handle isn't 0)
   without drawing. Runs after in_sync (0: none) and after earlier jobs
   on the source; out_sync (0: none) is signalled when it has finished.
   DRM_RISCOS_PRESENT_VSYNC isn't offered: -EOPNOTSUPP. -EBUSY: the CPU
   caches the screen just now; copy it with the CPU this time. -ENOTTY:
   not offered (EGL falls back). */
struct drm_riscos_present_rect { __s32 dst_x, dst_y, width, height, src_x, src_y; };
struct drm_riscos_present
{
  __u32 handle, format, flags;
  __u32 width, height;			/* the source's, pixels */
  __u32 uif_height_or_stride;
  __u32 rect_count, pad;
  __u64 rects;
  __u32 in_sync, out_sync;
};
#define DRM_RISCOS_PRESENT_SRC_UIF 1u
#define DRM_RISCOS_PRESENT_VSYNC 2u
#define DRM_RISCOS_PRESENT_SRC_UIF_XOR 4u
#define DRM_RISCOS_PRESENT_MAX_RECTS 256u
#define DRM_FORMAT_ARGB8888 0x34325241u	/* 'AR24': B, G, R, A bytes */
#define DRM_FORMAT_XRGB8888 0x34325258u	/* 'XR24' */
#define DRM_FORMAT_ABGR8888 0x34324241u	/* 'AB24': R, G, B, A bytes */
#define DRM_FORMAT_XBGR8888 0x34324258u	/* 'XB24' */
#define DRM_FORMAT_RGB565 0x36314752u	/* 'RG16': R in the top 5 bits */
#define DRM_IOCTL_RISCOS_PRESENT \
  _IOC (_IOC_WRITE, DRM_RISCOS_BASE, 0x04, sizeof (struct drm_riscos_present))

/* V3DRender's clock for absolute timeouts (drm_syncobj_wait): nanoseconds
   since the machine started, counted as OS_ReadMonotonicTime * 10,000,000.
   INT64_MAX waits for ever. */

#endif
