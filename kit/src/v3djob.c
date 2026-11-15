/* v3djob.c: V3DRender's first GPU jobs on a Raspberry Pi 4.

   1. A 128x128 frame cleared to a colour by a bin job and a render job
      (V3DRender's queues, the GPU's MMU, its interrupt, the wait). Every
      pixel is read back and checked; the bytes around the image must be
      untouched.
   2. The same at 1920x1080, then 200 frames in a row, alternating two
      colours, timed.
   3. "V3DJob hang" only: a bin job that never ends (it waits for a
      semaphore). V3DRender should
      notice after 500 ms, reset the GPU, end the job, and the next frame
      should work.

   Writes what it finds to the screen, and with "-o file" adds it to
   that file too. The calls to RISC OS are in v3djob_riscos.c (SWIs, by
   number: provisional); test/host/jobhost.c gives the same calls on the
   build machine, against V3DRender's core and a simulated GPU.

   SPDX-License-Identifier: GPL-2.0-or-later
   Copyright (c) 2026 Andrew Youll */
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include "clbuild.h"
#include "drm/drm.h"
#include "drm/v3d_drm.h"
#include "v3djob.h"
#include "tiling.h"
#include "csd_prog.h"

static int file, fails;
static FILE *results;
static const char *results_path;

/* to the screen, and to the results file when there is one */
static void
say (const char *fmt, ...)
{
  va_list a;
  va_start (a, fmt);
  vprintf (fmt, a);
  va_end (a);
  if (results)
    {
      va_start (a, fmt);
      vfprintf (results, fmt, a);
      va_end (a);
    }
}
#define printf say

static int
ioctl_ (uint32_t req, void *arg)
{
  const char *err = 0;
  int r = sys_ioctl (file, req, arg, &err);
  if (err)
    {
      printf ("V3DRender_Ioctl: %s\n", err);
      return -9999;
    }
  return r;
}

static void
check (int cond, const char *what)
{
  printf ("  %s  %s\n", cond ? "ok  " : "FAIL", what);
  if (!cond)
    fails++;
}

struct bo { uint32_t handle, gpu; uint8_t *cpu; uint32_t size; };

static int
bo_new (struct bo *b, uint32_t size)
{
  struct drm_v3d_create_bo c = { .size = size };
  int r = ioctl_ (DRM_IOCTL_V3D_CREATE_BO, &c);
  if (r)
    {
      printf ("  CREATE_BO %u bytes: %d\n", size, r);
      return r;
    }
  struct drm_v3d_mmap_bo m = { .handle = c.handle };
  r = ioctl_ (DRM_IOCTL_V3D_MMAP_BO, &m);
  b->handle = c.handle;
  b->gpu = c.offset;
  b->cpu = (uint8_t *) (uintptr_t) m.offset;
  b->size = size;
  return r;
}

static void
bo_free (struct bo *b)
{
  struct drm_gem_close c = { .handle = b->handle };
  if (b->handle)
    ioctl_ (DRM_IOCTL_GEM_CLOSE, &c);
  b->handle = 0;
}

static uint32_t sync;

/* waits for the sync object; 0, or -62 (ETIME) after the time in cs */
static int
wait_sync (unsigned cs)
{
  struct drm_syncobj_wait w = {
    .handles = (uintptr_t) &sync,
    .timeout_nsec = (int64_t) (now_cs () + cs) * 10000000,
    .count_handles = 1,
    .flags = DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL,
  };
  return ioctl_ (DRM_IOCTL_SYNCOBJ_WAIT, &w);
}

struct frame
{
  struct bo target, alloc, state, lists;
  struct clear_job j;
  uint32_t qms;				/* the binner's memory given (0: all) */
};

static int
frame_new (struct frame *f, uint32_t w, uint32_t h, int hang)
{
  memset (f, 0, sizeof *f);
  uint32_t asize, ssize;
  clear_job_sizes (w, h, &asize, &ssize);
  /* the image has 4 KB of guard either side */
  if (bo_new (&f->target, w * h * 4 + 8192) || bo_new (&f->alloc, asize)
      || bo_new (&f->state, ssize) || bo_new (&f->lists, 3 * 65536))
    return -1;
  struct clear_job *j = &f->j;
  j->width = w;
  j->height = h;
  j->stride = w * 4;
  j->target = f->target.gpu + 4096;
  j->tile_alloc = f->alloc.gpu;
  j->tile_state = f->state.gpu;
  j->bcl = f->lists.cpu;
  j->bcl_gpu = f->lists.gpu;
  j->gen = f->lists.cpu + 65536;
  j->gen_gpu = f->lists.gpu + 65536;
  j->rcl = f->lists.cpu + 2 * 65536;
  j->rcl_gpu = f->lists.gpu + 2 * 65536;
  j->hang = hang;
  return 0;
}

static int
frame_submit (struct frame *f, uint32_t colour)
{
  f->j.colour = colour;
  clear_job_build (&f->j);
  uint32_t handles[4] = { f->target.handle, f->alloc.handle, f->state.handle, f->lists.handle };
  struct drm_v3d_submit_cl s = {
    .bcl_start = f->j.bcl_gpu, .bcl_end = f->j.bcl_gpu + f->j.bcl_len,
    .rcl_start = f->j.rcl_gpu, .rcl_end = f->j.rcl_gpu + f->j.rcl_len,
    .qma = f->alloc.gpu, .qms = f->qms ? f->qms : f->alloc.size, .qts = f->state.gpu,
    .bo_handles = (uintptr_t) handles, .bo_handle_count = 4,
    .out_sync = sync,
  };
  return ioctl_ (DRM_IOCTL_V3D_SUBMIT_CL, &s);
}

static void
frame_free (struct frame *f)
{
  bo_free (&f->target);
  bo_free (&f->alloc);
  bo_free (&f->state);
  bo_free (&f->lists);
}

/* every pixel the colour, the guard bytes untouched */
static int
frame_check (struct frame *f, uint32_t colour, uint32_t guard)
{
  uint32_t *p = (uint32_t *) (f->target.cpu + 4096);
  uint32_t n = f->j.width * f->j.height, bad = 0, first = 0, firstv = 0;
  for (uint32_t i = 0; i < n; i++)
    if (p[i] != colour && !bad++)
      first = i, firstv = p[i];
  uint32_t *g0 = (uint32_t *) f->target.cpu, *g1 = p + n, gbad = 0;
  for (int i = 0; i < 1024; i++)
    gbad += (g0[i] != guard) + (g1[i] != guard);
  if (bad)
    printf ("    %u of %u pixels wrong; the first at (%u,%u) is &%08X\n", bad, n,
	    first % f->j.width, first / f->j.width, firstv);
  if (gbad)
    printf ("    %u guard words changed\n", gbad);
  return !bad && !gbad;
}

static void
fill (struct frame *f, uint32_t v)
{
  uint32_t *p = (uint32_t *) f->target.cpu;
  for (uint32_t i = 0; i < f->target.size / 4; i++)
    p[i] = v;
}

static void
status (void)
{
  printf ("\n");
  sys_status (results_path);
}

/* ---- kit 3 ---- */

static const uint32_t *
stats (void)
{
  return sys_stats ();
}

enum { ST_JOBS = 0, ST_IRQS = 5, ST_IRQS_EMPTY, ST_OVERFLOWS, ST_OVERFLOW_MISSES, ST_MMU_ERRORS };

/* the TFU copies a raster image into one of the GPU's tiled layouts, as
   Mesa's v3dX(tfu) does for a blit (32-bit pixels as R32F: no conversion);
   each pixel is checked where Mesa's tiling code says it goes */
static void
test_tfu (const char *name, uint32_t w, uint32_t h, uint32_t format, int layout)
{
  struct bo src, dst;
  uint32_t bytes = w * h * 4;
  if (bo_new (&src, bytes) || bo_new (&dst, bytes + 4096))
    {
      check (0, "buffers for the TFU");
      return;
    }
  uint32_t *s = (uint32_t *) src.cpu, *d = (uint32_t *) dst.cpu;
  for (uint32_t y = 0; y < h; y++)
    for (uint32_t x = 0; x < w; x++)
      s[y * w + x] = 0xA5000000u | (y << 12) | x;
  for (uint32_t i = 0; i < (bytes + 4096) / 4; i++)
    d[i] = 0xDEADBEEF;
  struct drm_v3d_submit_tfu t = {
    .icfg = (0u << 18) | (29u << 9),		/* raster in; R32F */
    .iia = src.gpu, .iis = w,			/* stride in pixels */
    .ioa = dst.gpu | (format << 3),
    .ios = (h << 16) | w,
    .bo_handles = { dst.handle, src.handle },
    .out_sync = sync,
  };
  unsigned t0 = now_cs ();
  int r = ioctl_ (DRM_IOCTL_V3D_SUBMIT_TFU, &t);
  int w8 = r == 0 ? wait_sync (200) : r;
  char what[120];
  snprintf (what, sizeof what, "TFU %ux%u raster -> %s: done in %u cs", w, h, name, now_cs () - t0);
  check (r == 0 && w8 == 0 && now_cs () - t0 < 40, what);
  uint32_t bad = 0, firstx = 0, firsty = 0, got = 0;
  for (uint32_t y = 0; y < h; y++)
    for (uint32_t x = 0; x < w; x++)
      {
	uint32_t off = layout == 0 ? lt_pixel_offset (x, y)
		       : uif_pixel_offset (h, x, y, layout == 2);
	uint32_t v = d[off / 4];
	if (v != s[y * w + x] && !bad++)
	  firstx = x, firsty = y, got = v;
      }
  uint32_t guard = 0;
  for (uint32_t i = bytes / 4; i < (bytes + 4096) / 4; i++)
    guard += d[i] != 0xDEADBEEF;
  if (bad)
    printf ("    %u of %u pixels not where Mesa puts them; (%u,%u) has &%08X\n",
	    bad, w * h, firstx, firsty, got);
  snprintf (what, sizeof what, "every pixel where Mesa's tiling puts it%s",
	    guard ? " (but the memory after it was written)" : "");
  check (!bad && !guard, what);
  bo_free (&src);
  bo_free (&dst);
}

/* a compute dispatch: one workgroup of 16 runs csd_prog (made with
   Mesa's QPU packer), which stores a value from its uniforms at an
   address from its uniforms; then the cache clean */
static void
test_csd (void)
{
  struct bo code, unif, out;
  if (bo_new (&code, 4096) || bo_new (&unif, 4096) || bo_new (&out, 4096))
    {
      check (0, "buffers for the compute job");
      return;
    }
  memcpy (code.cpu, csd_prog, sizeof csd_prog);
  uint32_t *o = (uint32_t *) out.cpu;
  for (int i = 0; i < 1024; i++)
    o[i] = 0xDEADBEEF;
  uint32_t *u = (uint32_t *) unif.cpu;
  u[0] = 0xC0DE1234;
  u[1] = out.gpu + 256;
  uint32_t handles[3] = { code.handle, unif.handle, out.handle };
  struct drm_v3d_submit_csd c = {
    .cfg = { 1u << 16, 1u << 16, 1u << 16,		/* 1x1x1 workgroups */
	     (1u << 8) | (0u << 12) | 16,		/* 1 per supergroup, 1 batch, 16 each */
	     0,						/* batches - 1 (V3D 4.2) */
	     code.gpu | 4,				/* the program; propagate NaNs */
	     unif.gpu },
    .bo_handles = (uintptr_t) handles, .bo_handle_count = 3,
    .out_sync = sync,
  };
  uint32_t before = stats ()[ST_JOBS + V3D_CSD];
  unsigned t0 = now_cs ();
  int r = ioctl_ (DRM_IOCTL_V3D_SUBMIT_CSD, &c);
  int w8 = r == 0 ? wait_sync (200) : r;
  char what[120];
  snprintf (what, sizeof what, "compute job done in %u cs (its interrupt came)", now_cs () - t0);
  check (r == 0 && w8 == 0 && now_cs () - t0 < 40, what);
  check (stats ()[ST_JOBS + V3D_CSD] == before + 1, "counted as a compute job");
  if (o[64] != 0xC0DE1234)
    printf ("    the word reads &%08X\n", o[64]);
  check (o[64] == 0xC0DE1234, "the GPU's program stored &C0DE1234 where it was told");
  int others = 0;
  for (int i = 0; i < 1024; i++)
    others += i != 64 && o[i] != 0xDEADBEEF;
  check (!others, "and nothing else");
  bo_free (&code);
  bo_free (&unif);
  bo_free (&out);
}

/* the binner given 8 KB for a 1080p frame's 510 tile lists: it runs out,
   V3DRender's interrupt gives it overflow memory, and it carries on */
static void
test_overflow (void)
{
  struct frame f;
  if (frame_new (&f, 1920, 1080, 0))
    {
      check (0, "buffers for the overflow frame");
      return;
    }
  f.qms = 8192;
  fill (&f, 0x5A5A5A5A);
  uint32_t before = stats ()[ST_OVERFLOWS], misses = stats ()[ST_OVERFLOW_MISSES];
  unsigned t0 = now_cs ();
  int r = frame_submit (&f, 0xFF808080);
  int w8 = r == 0 ? wait_sync (200) : r;
  char what[120];
  snprintf (what, sizeof what, "a 1080p frame with 8 KB of binner memory: done in %u cs", now_cs () - t0);
  check (r == 0 && w8 == 0 && now_cs () - t0 < 40, what);
  printf ("    overflows given: %u\n", stats ()[ST_OVERFLOWS] - before);
  check (stats ()[ST_OVERFLOWS] > before && stats ()[ST_OVERFLOW_MISSES] == misses,
	 "the binner ran out and was given more");
  check (frame_check (&f, 0xFF808080, 0x5A5A5A5A), "every pixel right");
  frame_free (&f);
}

/* the GPU through RISC3D (0.02 or later), as Mesa will reach it */
static void
test_risc3d (void)
{
  const char *name, *drm;
  unsigned flags, ioctl_swi;
  const char *e = sys_risc3d_info (&name, &flags, &ioctl_swi, &drm);
  if (e)
    {
      printf ("    RISC3D isn't loaded: skipped (%s)\n", e);
      return;
    }
  printf ("    RISC3D's driver: %s; flags %u, ioctl SWI &%X, DRM driver %s\n",
	  name ? name : "(none)", flags, ioctl_swi, drm ? drm : "(none)");
  check (name && strstr (name, "V3DRender"), "RISC3D lists V3DRender");
  check ((flags & 1) && ioctl_swi == 0xC3D43 && drm && !strcmp (drm, "v3d"),
	 "as a DRM device: V3DRender_Ioctl, \"v3d\"");
  check (!(flags & 2), "drawing on a GPU, not in software");
  int ctx, rfile;
  e = sys_risc3d_open (&ctx, &rfile);
  check (!e, "RISC3D_Open");
  if (e)
    {
      printf ("    %s\n", e);
      return;
    }
  printf ("    context %d, V3DRender file %d\n", ctx, rfile);
  int mine = file;
  file = rfile;				/* the frame on RISC3D's file */
  struct drm_syncobj_create sc = { 0, 0 };
  ioctl_ (DRM_IOCTL_SYNCOBJ_CREATE, &sc);
  uint32_t mysync = sync;
  sync = sc.handle;
  struct frame f;
  if (!frame_new (&f, 128, 128, 0))
    {
      fill (&f, 0xDEADBEEF);
      check (frame_submit (&f, 0x11223344) == 0 && wait_sync (200) == 0,
	     "a frame on that file");
      check (frame_check (&f, 0x11223344, 0xDEADBEEF), "every pixel right");
      frame_free (&f);
    }
  sys_risc3d_close (ctx);
  struct drm_v3d_get_param gp = { .param = DRM_V3D_PARAM_SUPPORTS_TFU };
  const char *err = 0;
  sys_ioctl (rfile, DRM_IOCTL_V3D_GET_PARAM, &gp, &err);
  check (err != 0, "RISC3D_Close closed V3DRender's file");
  file = mine;
  sync = mysync;
}

int
main (int argc, char **argv)
{
  int hang = 0;
  for (int i = 1; i < argc; i++)
    if (!strcmp (argv[i], "hang"))
      hang = 1;
    else if (!strcmp (argv[i], "-o") && i + 1 < argc)
      results = fopen (results_path = argv[++i], "a");	/* add to the file */
  unsigned ver, v3d, hz;
  const char *e = sys_info (&ver, &v3d, &hz);
  if (e)
    {
      printf ("V3DRender isn't loaded: %s\n", e);
      return 1;
    }
  printf ("V3DJob: V3DRender %u.%02u, V3D %u.%u\n", ver / 100, ver % 100, v3d / 10, v3d % 10);
  e = sys_open (&file);
  if (e)
    {
      printf ("V3DRender_Open: %s\n", e);
      return 1;
    }
  sys_info (&ver, &v3d, &hz);
  printf ("Clock with a file open: %u MHz\n", hz / 1000000);
  struct drm_syncobj_create sc = { 0, 0 };
  ioctl_ (DRM_IOCTL_SYNCOBJ_CREATE, &sc);
  sync = sc.handle;

  struct frame f;
  if (!hang)
    {
      printf ("\n1. A 128x128 frame cleared to &44332211\n");
      if (frame_new (&f, 128, 128, 0))
	return 1;
      fill (&f, 0xDEADBEEF);
      unsigned t0 = now_cs ();
      int r = frame_submit (&f, 0x44332211);
      check (r == 0, "SUBMIT_CL accepted");
      r = wait_sync (200);
      printf ("    wait: %d after %u cs\n", r, now_cs () - t0);
      check (r == 0, "the GPU finished it (within 2 s)");
      check (now_cs () - t0 < 40, "in well under half a second (its interrupt came: no reset)");
      check (frame_check (&f, 0x44332211, 0xDEADBEEF), "every pixel cleared, nothing else touched");
      frame_free (&f);

      printf ("\n2. 1920x1080 frames\n");
      if (frame_new (&f, 1920, 1080, 0))
	return 1;
      fill (&f, 0x5A5A5A5A);
      r = frame_submit (&f, 0xFF0000FF);
      check (r == 0 && wait_sync (200) == 0, "one frame, cleared to red");
      check (frame_check (&f, 0xFF0000FF, 0x5A5A5A5A), "every pixel red");
      unsigned frames = 200, t = now_cs (), ok = 1;
      for (unsigned i = 0; i < frames && ok; i++)
	ok = frame_submit (&f, i & 1 ? 0xFF00FF00 : 0xFFFF0000) == 0 && wait_sync (200) == 0;
      t = now_cs () - t;
      check (ok, "200 frames, each waited for");
      check (frame_check (&f, 0xFF00FF00, 0x5A5A5A5A), "the last one green");
      if (t)
	printf ("    %u frames in %u cs: %u frames a second, %u MB/s written\n",
		frames, t, frames * 100 / t, (unsigned) ((uint64_t) frames * 1920 * 1080 * 4 / 1048576 * 100 / t));
      frame_free (&f);

      printf ("\n4. The TFU (texture formatting: Mesa's mipmaps and blits)\n");
      test_tfu ("lineartile", 64, 4, 3, 0);
      test_tfu ("UIF", 128, 128, 6, 1);
      test_tfu ("UIF with XOR", 256, 256, 7, 2);
      printf ("\n5. A compute job (CSD: GLES 3.1 compute shaders)\n");
      test_csd ();
      printf ("\n6. The binner running out of memory\n");
      test_overflow ();
      printf ("\n7. Through RISC3D\n");
      test_risc3d ();
      uint32_t const *st = stats ();
      check (st[ST_IRQS_EMPTY] == 0 && st[ST_MMU_ERRORS] == 0,
	     "no stray interrupts and no MMU errors in all that");
    }
  else
    {
      printf ("\n3. A job that never ends (the reset test)\n");
      if (frame_new (&f, 128, 128, 1))
	return 1;
      unsigned t0 = now_cs ();
      int r = frame_submit (&f, 0);
      check (r == 0, "SUBMIT_CL accepted");
      r = wait_sync (300);
      printf ("    wait: %d after %u cs\n", r, now_cs () - t0);
      check (r == 0, "V3DRender ended it (the GPU was reset)");
      frame_free (&f);
      printf ("   then a normal frame:\n");
      if (frame_new (&f, 128, 128, 0))
	return 1;
      fill (&f, 0xDEADBEEF);
      r = frame_submit (&f, 0x80402010);
      check (r == 0 && wait_sync (200) == 0, "the GPU works after the reset");
      check (frame_check (&f, 0x80402010, 0xDEADBEEF), "every pixel right");
      frame_free (&f);
    }

  struct drm_syncobj_destroy sd = { sync, 0 };
  ioctl_ (DRM_IOCTL_SYNCOBJ_DESTROY, &sd);
  sys_close (file);
  printf (fails ? "\n%d failed.\n" : "\nAll passed.\n", fails);
  if (results)
    fclose (results);
  status ();
  return fails != 0;
}
