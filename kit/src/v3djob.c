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
    .qma = f->alloc.gpu, .qms = f->alloc.size, .qts = f->state.gpu,
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
