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
#include <stdlib.h>
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
	     /* the program; propagate NaNs; single segment: its only thrsw
		is the end, so it starts after the "last thrsw" (Mesa's
		single_seg; without this the QPU runs past the end: kit 3) */
	     code.gpu | 4 | 2,
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

/* Test 8 (kit 5): a compute dispatch of many workgroups, each writing
   its own number, so that threads run on every QPU. Mesa's programs on
   the Pi 4 draw a few tiles, then stop with pixels waiting for fragment
   shaders; if some QPUs never finish a thread, this stops too. */
static void
test_csd_wide (unsigned groups)
{
  struct bo code, unif, out;
  if (bo_new (&code, 4096) || bo_new (&unif, 4096) || bo_new (&out, groups * 4 + 4096))
    {
      check (0, "buffers for the compute job");
      return;
    }
  memcpy (code.cpu, csd_wide, sizeof csd_wide);
  uint32_t *o = (uint32_t *) out.cpu;
  for (unsigned i = 0; i < groups + 1024; i++)
    o[i] = 0xDEADBEEF;
  uint32_t *u = (uint32_t *) unif.cpu;
  u[0] = 2;
  u[1] = out.gpu;
  uint32_t handles[3] = { code.handle, unif.handle, out.handle };
  struct drm_v3d_submit_csd c = {
    .cfg = { groups << 16, 1u << 16, 1u << 16,
	     (1u << 8) | (0u << 12) | 16,		/* 1 per supergroup, 1 batch, 16 each */
	     groups - 1,				/* batches - 1 */
	     code.gpu | 4 | 2,				/* propagate NaNs; single segment */
	     unif.gpu },
    .bo_handles = (uintptr_t) handles, .bo_handle_count = 3,
    .out_sync = sync,
  };
  unsigned t0 = now_cs ();
  int r = ioctl_ (DRM_IOCTL_V3D_SUBMIT_CSD, &c);
  int w8 = r == 0 ? wait_sync (200) : r;
  char what[120];
  snprintf (what, sizeof what, "%u workgroups done in %u cs (no reset)", groups, now_cs () - t0);
  check (r == 0 && w8 == 0 && now_cs () - t0 < 40, what);
  unsigned wrong = 0, first = 0;
  for (unsigned i = 0; i < groups; i++)
    if (o[i] != i && !wrong++)
      first = i;
  if (wrong)
    printf ("    %u workgroups didn't write theirs; the first is %u (reads &%08X)\n",
	    wrong, first, o[first]);
  check (!wrong, "every workgroup wrote its number");
  unsigned others = 0;
  for (unsigned i = groups; i < groups + 1024; i++)
    others += o[i] != 0xDEADBEEF;
  check (!others, "and nothing else");
  bo_free (&code);
  bo_free (&unif);
  bo_free (&out);
}

/* Test 6 (kit 3) gave the binner 8 KB for a 1080p frame: it didn't run out
   on the Pi 4, because a cleared frame has no primitives and the tile
   lists' first blocks aren't taken from that memory. Overflow needs drawn
   triangles, so it waits for shaders (Mesa). */

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

/* ---- kit 6: replaying a job from a *V3DDump file ----
   The dump (V3DRender 0.06) holds the job's lists, its BOs as the GPU
   left them, and their GPU addresses. With nothing else allocated, BOs
   made in address order (with fillers for the gaps) land at the same
   addresses (V3DRender allocates first fit), so the lists run as they
   are. Each run copies the BOs back, makes one change to the generic
   tile list, submits, and says whether the GPU finished. */

struct rbo { uint32_t gpu, size; const uint8_t *data; struct bo bo; };

static uint8_t *rd_buf;
static uint32_t rd_w[12];
static struct rbo rbos[64];
static unsigned nrbos;

static int
replay_load (const char *path)
{
  FILE *f = fopen (path, "rb");
  if (!f)
    {
      printf ("  can't open %s\n", path);
      return -1;
    }
  fseek (f, 0, SEEK_END);
  long n = ftell (f);
  fseek (f, 0, SEEK_SET);
  rd_buf = malloc ((size_t) n);
  if (!rd_buf || fread (rd_buf, 1, (size_t) n, f) != (size_t) n)
    {
      fclose (f);
      printf ("  can't read %s\n", path);
      return -1;
    }
  fclose (f);
  memcpy (rd_w, rd_buf, sizeof rd_w);
  if (rd_w[0] != 0x44443356 || rd_w[1] != 0x31504d55)
    {
      printf ("  %s isn't a V3DRender dump\n", path);
      return -1;
    }
  uint32_t off = (rd_w[2] + 32) * 4;		/* the header, then struct v3d_hang */
  nrbos = rd_w[11] < 64 ? rd_w[11] : 64;
  for (unsigned i = 0; i < nrbos; i++)
    {
      memcpy (&rbos[i].gpu, rd_buf + off, 4);
      memcpy (&rbos[i].size, rd_buf + off + 4, 4);
      rbos[i].data = rd_buf + off + 8;
      off += 8 + rbos[i].size;
    }
  /* in address order */
  for (unsigned i = 0; i < nrbos; i++)
    for (unsigned j = i + 1; j < nrbos; j++)
      if (rbos[j].gpu < rbos[i].gpu)
	{
	  struct rbo t = rbos[i];
	  rbos[i] = rbos[j];
	  rbos[j] = t;
	}
  printf ("  render list &%08X-&%08X, bin list &%08X-&%08X, %u BOs\n",
	  rd_w[4], rd_w[5], rd_w[6], rd_w[7], nrbos);
  static struct bo fill[64];
  unsigned nfill = 0;
  for (unsigned i = 0; i < nrbos; i++)
    {
      if (!rbos[i].size)
	{
	  printf ("  BO &%08X wasn't kept in the dump\n", rbos[i].gpu);
	  return -1;
	}
      for (;;)
	{
	  if (bo_new (&rbos[i].bo, rbos[i].size))
	    return -1;
	  if (rbos[i].bo.gpu == rbos[i].gpu)
	    break;
	  uint32_t got = rbos[i].bo.gpu;
	  bo_free (&rbos[i].bo);
	  if (got > rbos[i].gpu || nfill == 64)
	    {
	      printf ("  can't place BO &%08X (got &%08X): run Replay just after loading V3DRender\n",
		      rbos[i].gpu, got);
	      return -1;
	    }
	  if (bo_new (&fill[nfill++], rbos[i].gpu - got))	/* a filler for the gap */
	    return -1;
	}
    }
  return 0;
}

static uint8_t *
replay_cpu (uint32_t gpu)
{
  for (unsigned i = 0; i < nrbos; i++)
    if (gpu >= rbos[i].gpu && gpu < rbos[i].gpu + rbos[i].size)
      return rbos[i].bo.cpu + (gpu - rbos[i].gpu);
  return 0;
}

/* the generic tile list's packets: the lengths of those Mesa puts there */
static int
gtl_len (uint8_t op)
{
  switch (op)
    {
    case 0x7D: case 0x1A: case 0x1B: case 0x12: case 0x01: return 1;	/* coords implicit, end of loads, end of tile, return, nop */
    case 0x38: case 0x15: case 0x19: return 2;	/* prim list format, branch implicit, clear */
    case 0x36: return 5;			/* instance ID */
    case 0x1D: case 0x1C: return 13;		/* store / load general */
    default: return 0;
    }
}

enum { NO_CHANGE, NO_Z_STORE, NO_COLOUR_STORE, NO_STORES, NO_PRIMITIVES,
       FS_NOT_4WAY, NO_SCOREBOARD, LAST_CHANGE = NO_SCOREBOARD };
static const char *const change_name[] = {
  "as Mesa sent it", "without the Z store", "without the colour store",
  "without either store", "without its primitives (no Branch to Implicit Tile List)",
  "with the fragment shader not 4-way threadable", "with the scoreboard turned off" };

/* the GL Shader State record the bin list names (opcode 64) */
static uint8_t *
replay_record (void)
{
  uint8_t *bcl = replay_cpu (rd_w[6]);
  for (uint32_t k = 0; bcl && k + 5 <= rd_w[7] - rd_w[6]; k++)
    if (bcl[k] == 64)
      {
	uint32_t w;
	memcpy (&w, bcl + k + 1, 4);
	uint8_t *rec = replay_cpu (w & ~31u);
	if (rec && (w & 31) <= 16)
	  return rec;
      }
  return 0;
}

static void
replay_run (int change)
{
  for (unsigned i = 0; i < nrbos; i++)
    memcpy (rbos[i].bo.cpu, rbos[i].data, rbos[i].size);
  /* the generic tile list: Start Address of Generic Tile List (opcode 20) in the RCL */
  uint8_t *rcl = replay_cpu (rd_w[4]);
  uint32_t gs = 0, ge = 0;
  /* found by opcode 20 followed by two addresses in a BO of the job */
  for (uint32_t k = 0; rcl && k + 9 <= rd_w[5] - rd_w[4]; k++)
    if (rcl[k] == 20)
      {
	uint32_t a, b;
	memcpy (&a, rcl + k + 1, 4);
	memcpy (&b, rcl + k + 5, 4);
	if (replay_cpu (a) && b > a && b - a < 256)
	  {
	    gs = a;
	    ge = b;
	    break;
	  }
      }
  uint8_t *g = gs ? replay_cpu (gs) : 0;
  int stores = 0, changed = change == NO_CHANGE;
  for (uint32_t k = 0; g && k < ge - gs + 1;)
    {
      int n = gtl_len (g[k]);
      if (!n)
	break;
      int kill = 0;
      if (g[k] == 0x1D)
	{
	  int z = stores++ > 0;			/* Mesa stores colour first, then Z */
	  kill = change == NO_STORES || (change == NO_Z_STORE && z) || (change == NO_COLOUR_STORE && !z);
	}
      if (g[k] == 0x15 && change == NO_PRIMITIVES)
	kill = 1;
      if (kill)
	{
	  memset (g + k, 0x01, (size_t) n);	/* NOPs */
	  changed = 1;
	}
      if (g[k] == 0x12)
	break;
      k += (uint32_t) n;
    }
  uint8_t *rec = replay_record ();
  if (rec && change == FS_NOT_4WAY)
    {
      rec[12] &= (uint8_t) ~1u;		/* bit 96: Fragment Shader 4-way threadable */
      changed = 1;
    }
  if (rec && change == NO_SCOREBOARD)
    {
      rec[2] |= 1;			/* bit 16: Turn off scoreboard */
      changed = 1;
    }
  printf ("\n  The job %s%s\n", change_name[change], changed ? "" : " (not found: run as sent)");
  struct drm_v3d_submit_cl c = {
    .bcl_start = rd_w[6], .bcl_end = rd_w[7], .rcl_start = rd_w[4], .rcl_end = rd_w[5],
    .qma = rd_w[8], .qms = rd_w[9], .qts = rd_w[10], .out_sync = sync,
  };
  uint32_t handles[64];
  for (unsigned i = 0; i < nrbos; i++)
    handles[i] = rbos[i].bo.handle;
  c.bo_handles = (uintptr_t) handles;
  c.bo_handle_count = nrbos;
  uint32_t resets = stats ()[13];
  unsigned t0 = now_cs ();
  int r = ioctl_ (DRM_IOCTL_V3D_SUBMIT_CL, &c);
  int w8 = r == 0 ? wait_sync (300) : r;
  const uint32_t *st = stats ();
  int hung = st[13] != resets;
  char what[160];
  if (r)
    snprintf (what, sizeof what, "SUBMIT_CL refused: %d", r);
  else if (hung)
    snprintf (what, sizeof what, "the GPU finished it: no, reset after %u cs (render CA &%08X, RA &%08X)",
	      now_cs () - t0, st[48 + 8], st[48 + 10]);
  else
    snprintf (what, sizeof what, "the GPU finished it in %u cs", now_cs () - t0);
  check (r == 0 && w8 == 0 && !hung, what);
}

int
main (int argc, char **argv)
{
  int hang = 0;
  const char *replay = 0;
  for (int i = 1; i < argc; i++)
    if (!strcmp (argv[i], "hang"))
      hang = 1;
    else if (!strcmp (argv[i], "replay") && i + 1 < argc)
      replay = argv[++i];
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

  if (replay)
    {
      printf ("\n9. Replaying the job in %s\n", replay);
      if (replay_load (replay) == 0)
	for (int change = NO_CHANGE; change <= LAST_CHANGE; change++)
	  replay_run (change);
      else
	check (0, "the job loaded");
      struct drm_syncobj_destroy sd = { sync, 0 };
      ioctl_ (DRM_IOCTL_SYNCOBJ_DESTROY, &sd);
      sys_close (file);
      printf (fails ? "\n%d failed.\n" : "\nAll passed.\n", fails);
      if (results)
	fclose (results);
      status ();
      return fails != 0;
    }

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
      printf ("\n8. Compute on every QPU: 64, then 4096 workgroups\n");
      test_csd_wide (64);
      test_csd_wide (4096);
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
