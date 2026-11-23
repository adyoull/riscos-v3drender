/* sim.c: the core on the build machine, with a simulated V3D.

   The "GPU" here keeps a register file per bank. Writing a queue's start
   register (CT0QEA, CT1QEA, TFU ICFG, CSD CFG0) marks that queue busy;
   the test then finishes it with sim_finish(), which sets the interrupt
   status bit as the hardware would and calls the core's interrupt
   handler. Pages get scattered "physical" addresses so that the MMU code
   has to join runs. Then the tests drive the core through v3d_ioctl as
   Mesa would.

   SPDX-License-Identifier: GPL-2.0-or-later
   Copyright (c) 2026 Andrew Youll */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "v3d.h"
#include "v3d_regs.h"
#include "../../kit/src/tiling.h"

/* ---- the simulated machine ---- */

static uint32_t regs[V3D_NBANKS][0x4000 / 4];
static int busy[V3D_HW_QUEUES];
static uint32_t now_cs = 1000;
static int resets, auto_finish, logs;
static uint8_t *area;
static uint32_t area_pages = 4096;	/* 16 MB */
static uint8_t claimed[4096];
static int claim_fails, claims, releases;	/* (0.25, 0.26) */
static uint32_t pt[1 << 20];

/* (0.26) an L2T flush can be left pending for a few reads, and a TMU
   write combiner flush started while one is counts as a clobber
   (GFXH-1897) */
static int l2t_pending, l2t_clobbers;

/* (0.31) render lists carried out as they start (PRESENT's test) */
static void (*present_exec) (uint32_t start, uint32_t end);

uint32_t
v3d_rd (struct v3d_dev *v3d, int bank, uint32_t off)
{
  (void) v3d;
  if (bank == V3D_BANK_CORE0 && off == V3D_CTL_L2TCACTL && l2t_pending)
    {
      l2t_pending--;
      return regs[bank][off / 4] | V3D_L2TCACTL_L2TFLS;
    }
  return regs[bank][off / 4];
}

static void
start (int q)
{
  busy[q] = 1;
}

void
v3d_wr (struct v3d_dev *v3d, int bank, uint32_t off, uint32_t val)
{
  int ver = v3d->ver;
  if (bank == V3D_BANK_CORE0 && off == V3D_CTL_INT_CLR)
    regs[bank][V3D_CTL_INT_STS / 4] &= ~val;
  else if (bank == V3D_BANK_HUB && off == V3D_HUB_INT_CLR)
    regs[bank][V3D_HUB_INT_STS / 4] &= ~val;
  else if (bank == V3D_BANK_CORE0 && off == V3D_CLE_CT0QEA)
    start (V3D_BIN);
  else if (bank == V3D_BANK_CORE0 && off == V3D_CLE_CT1QEA)
    {
      start (V3D_RENDER);
      if (present_exec)
	present_exec (regs[V3D_BANK_CORE0][V3D_CLE_CT1QBA / 4], val);
    }
  else if (bank == V3D_BANK_HUB && off == (uint32_t) V3D_TFU_ICFG (ver))
    start (V3D_TFU);
  else if (bank == V3D_BANK_CORE0 && off == (uint32_t) V3D_CSD_QUEUED_CFG0 (ver))
    start (V3D_CSD);
  /* busy bits read back as done, so waits end at once */
  if (bank == V3D_BANK_HUB && off == V3D_MMUC_CONTROL)
    val &= ~V3D_MMUC_CONTROL_FLUSHING;
  if (bank == V3D_BANK_HUB && off == V3D_MMU_CTL)
    val &= ~V3D_MMU_CTL_TLB_CLEARING;
  if (bank == V3D_BANK_CORE0 && off == V3D_CTL_L2TCACTL)
    {
      if (l2t_pending && (val & V3D_L2TCACTL_TMUWCF))
	l2t_clobbers++;
      val &= ~(V3D_L2TCACTL_TMUWCF | V3D_L2TCACTL_L2TFLS);
    }
  if (!(bank == V3D_BANK_CORE0 && off == V3D_CTL_INT_CLR)
      && !(bank == V3D_BANK_HUB && off == V3D_HUB_INT_CLR))
    regs[bank][off / 4] = val;
}

/* scattered pages: each group of 16 is contiguous, groups reversed; the
   first 1024 pages and the top 2048 (where aligned BOs go, 0.13) are
   contiguous runs, for the super page tests */
uint64_t
plat_page_pa (struct v3d_dev *v3d, uint32_t page)
{
  (void) v3d;
  if (page < 1024)
    return 0x100000000ull + (uint64_t) page * 4096;
  if (page >= 2048)
    return 0x300000000ull + (uint64_t) page * 4096;
  uint32_t g = page / 16, i = page % 16;
  return 0x200000000ull + (uint64_t) ((1000 - g) * 16 + i) * 4096;
}

static unsigned cache_cleans, cache_invals, last_cached;
uint64_t plat_virt_pa (uintptr_t va) { return va ? 0x400000000ull + (va & ~4095ull) : 0; }
void plat_zero_pages (void *p, uint32_t n) { memset (p, 0, n); }
void plat_cache_clean (const void *p, uint32_t n) { (void) p; (void) n; cache_cleans++; }
void plat_cache_inval (const void *p, uint32_t n) { (void) p; (void) n; cache_invals++; }

int
plat_pages_claim (struct v3d_dev *v3d, uint32_t first, uint32_t n, int cached)
{
  last_cached = (unsigned) cached;
  (void) v3d;
  if (claim_fails)
    {
      claim_fails--;
      return -1;
    }
  claims++;
  for (uint32_t p = first; p < first + n; p++)
    {
      if (claimed[p])
	{
	  printf ("FAIL: page %u claimed twice\n", p);
	  exit (1);
	}
      claimed[p] = 1;
      memset (area + p * 4096, 0xAA, 4096);	/* not zero: the core must clear */
    }
  return 0;
}

void
plat_pages_release (struct v3d_dev *v3d, uint32_t first, uint32_t n)
{
  (void) v3d;
  releases++;
  for (uint32_t p = first; p < first + n; p++)
    claimed[p] = 0;
}

void plat_gpu_reset (struct v3d_dev *v3d) { (void) v3d; resets++; memset (busy, 0, sizeof busy); }
uint32_t plat_time_cs (void) { return now_cs; }
static uint64_t ns_ticks;
uint64_t plat_time_ns (void) { return (uint64_t) now_cs * 10000000u + (ns_ticks += 1000); }
void plat_barrier (void) { }
unsigned plat_irq_off (void) { return 0; }
void plat_irq_restore (unsigned s) { (void) s; }
void *plat_alloc (size_t n) { return calloc (1, n); }
void *dump_buf; uint32_t dump_bytes;
void *plat_dump_buffer (uint32_t n) { free (dump_buf); dump_bytes = n; return dump_buf = calloc (1, n); }
void plat_free (void *p) { free (p); }
void plat_log (struct v3d_dev *v3d, const char *m, uint32_t v) { (void) v3d; (void) m; (void) v; logs++; }

static struct v3d_dev *dev;

/* the GPU finishes queue q: the interrupt */
static void
sim_finish (int q)
{
  if (!busy[q])
    {
      printf ("FAIL: finishing queue %d, which isn't running\n", q);
      exit (1);
    }
  busy[q] = 0;
  static const uint32_t core_bit[] = { V3D_INT_FLDONE, V3D_INT_FRDONE, 0, 0 };
  if (q == V3D_TFU)
    regs[V3D_BANK_HUB][V3D_HUB_INT_STS / 4] |= V3D_HUB_INT_TFUC;
  else if (q == V3D_CSD)
    regs[V3D_BANK_CORE0][V3D_CTL_INT_STS / 4] |= V3D_INT_CSDDONE (dev->ver);
  else
    regs[V3D_BANK_CORE0][V3D_CTL_INT_STS / 4] |= core_bit[q];
  v3d_irq (dev);
}

void
plat_wait (void)
{
  now_cs++;
  if (auto_finish)
    for (int q = 0; q < V3D_HW_QUEUES; q++)
      if (busy[q])
	sim_finish (q);
}

/* ---- tests ---- */

static int checks, failures;
#define CHECK(c) do { checks++; if (!(c)) { failures++; \
  printf ("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static int
ioctl_ (int f, uint32_t req, void *arg)
{
  return v3d_ioctl (dev, f, req, arg);
}

static uint32_t
bo_new (int f, uint32_t size, uint32_t *offset)
{
  struct drm_v3d_create_bo c = { .size = size };
  CHECK (ioctl_ (f, DRM_IOCTL_V3D_CREATE_BO, &c) == 0);
  if (offset)
    *offset = c.offset;
  return c.handle;
}

static uint32_t
syncobj (int f, int signalled)
{
  struct drm_syncobj_create c = { 0, signalled ? DRM_SYNCOBJ_CREATE_SIGNALED : 0 };
  CHECK (ioctl_ (f, DRM_IOCTL_SYNCOBJ_CREATE, &c) == 0);
  return c.handle;
}

static int
syncobj_done (int f, uint32_t h)
{
  struct drm_syncobj_wait w = { .handles = (uintptr_t) &h, .timeout_nsec = 0,
				.count_handles = 1 };
  return ioctl_ (f, DRM_IOCTL_SYNCOBJ_WAIT, &w) == 0;
}

static void
setup (int ver)
{
  memset (regs, 0, sizeof regs);
  memset (busy, 0, sizeof busy);
  memset (claimed, 0, sizeof claimed);
  resets = 0;
  if (ver == 42)
    {
      regs[V3D_BANK_HUB][V3D_HUB_IDENT0 / 4] = 0x42554856;
      regs[V3D_BANK_HUB][V3D_HUB_IDENT1 / 4] = 0x000e1124;	/* a Pi 4's */
      regs[V3D_BANK_HUB][V3D_HUB_IDENT3 / 4] = 0x00000e00;
    }
  else
    regs[V3D_BANK_HUB][V3D_HUB_IDENT1 / 4] = 0x00000117;	/* 7.1, 1 core */
  regs[V3D_BANK_HUB][V3D_MMU_DEBUG_INFO / 4] = (6 << 8) | (2 << 4);
  regs[V3D_BANK_CORE0][V3D_CTL_IDENT0 / 4] = 0x04443356;
  dev = calloc (1, sizeof *dev);
  area = calloc (area_pages, 4096);
  dev->ver = ver;
  dev->pt = pt;
  dev->pt_pa = 0x80000000ull;
  dev->scratch_pa = 0x80400000ull;
  dev->area = area;
  dev->area_pages = area_pages;
}

static void
teardown (void)
{
  v3d_fini (dev);
  int left = 0;
  for (uint32_t p = 0; p < area_pages; p++)
    left += claimed[p];
  CHECK (left == 0);			/* every page given back */
  free (area);
  free (dev);
}

static void
test_probe_and_bo (void)
{
  setup (42);
  CHECK (v3d_hw_probe (dev) == 0);
  CHECK (dev->ver == 42 && dev->cores == 1 && dev->rev == 14);
  CHECK (dev->va_width == 32 && dev->pa_width == 36);
  CHECK (v3d_init (dev) == 0);
  CHECK (dev->noverflow_pool == V3D_OVERFLOW_POOL / 2);
  CHECK (regs[V3D_BANK_HUB][V3D_MMU_PT_PA_BASE / 4] == 0x80000);
  CHECK (regs[V3D_BANK_HUB][V3D_MMU_CTL / 4] & V3D_MMU_CTL_ENABLE);
  int f = v3d_open (dev, 77);
  CHECK (f >= 0);

  uint32_t off, h = bo_new (f, 10000, &off);
  CHECK (h == 1 && off != 0 && (off & 4095) == 0);
  struct drm_v3d_mmap_bo m = { .handle = h };
  CHECK (ioctl_ (f, DRM_IOCTL_V3D_MMAP_BO, &m) == 0);
  uint8_t *p = (uint8_t *) (uintptr_t) m.offset;
  CHECK (p == area + off);
  CHECK (p[0] == 0 && p[3 * 4096 - 1] == 0);	/* zeroed */
  uint32_t page = off >> 12;
  CHECK ((pt[page] & (1u << 28)) && (pt[page] & (1u << 29)));
  CHECK ((uint64_t) (pt[page] & 0xffffff) << 12 == plat_page_pa (dev, page));
  CHECK (pt[page + 3] == 0);			/* 3 pages only */

  struct drm_v3d_get_bo_offset o = { .handle = h };
  CHECK (ioctl_ (f, DRM_IOCTL_V3D_GET_BO_OFFSET, &o) == 0 && o.offset == off);
  struct drm_v3d_get_param gp = { .param = DRM_V3D_PARAM_V3D_HUB_IDENT1 };
  CHECK (ioctl_ (f, DRM_IOCTL_V3D_GET_PARAM, &gp) == 0 && gp.value == 0x000e1124);
  gp.param = DRM_V3D_PARAM_V3D_CORE0_IDENT0;
  gp.value = 0;
  CHECK (ioctl_ (f, DRM_IOCTL_V3D_GET_PARAM, &gp) == 0 && gp.value == 0x04443356);
  gp.param = DRM_V3D_PARAM_SUPPORTS_CSD;
  CHECK (ioctl_ (f, DRM_IOCTL_V3D_GET_PARAM, &gp) == 0 && gp.value == 1);
  gp.param = 999;
  CHECK (ioctl_ (f, DRM_IOCTL_V3D_GET_PARAM, &gp) == -V3D_EINVAL);
  struct drm_v3d_create_bo bad = { .size = 4096, .flags = 1 };
  CHECK (ioctl_ (f, DRM_IOCTL_V3D_CREATE_BO, &bad) == -V3D_EINVAL);

  /* a big BO over scattered pages: 4K, 64K and 1M entries */
  uint32_t off2;
  bo_new (f, 600 * 4096, &off2);
  uint32_t p2 = off2 >> 12, n4 = 0, n64 = 0, n1m = 0;
  for (uint32_t i = 0; i < 600; i++)
    {
      uint32_t e = pt[p2 + i];
      CHECK ((uint64_t) (e & 0xffffff) << 12 == plat_page_pa (dev, p2 + i));
      if (e & (1u << 31)) n1m++;
      else if (e & (1u << 30)) n64++;
      else n4++;
    }
  CHECK (n64 > 0);
  printf ("  600-page BO at page %u: %u 4K, %u 64K, %u 1M entries\n", p2, n4, n64, n1m);

  struct drm_gem_close gc = { .handle = h };
  CHECK (ioctl_ (f, DRM_IOCTL_GEM_CLOSE, &gc) == 0);
  CHECK (pt[page] == 0);
  CHECK (ioctl_ (f, DRM_IOCTL_GEM_CLOSE, &gc) == -V3D_EINVAL);
  v3d_close (dev, f);
  teardown ();
}

static void
test_cl_jobs (void)
{
  setup (42);
  CHECK (v3d_hw_probe (dev) == 0 && v3d_init (dev) == 0);
  int f = v3d_open (dev, 1);
  uint32_t tex = bo_new (f, 4096, 0), cl = bo_new (f, 8192, 0);
  uint32_t out = syncobj (f, 0), tfu_out = syncobj (f, 0);
  CHECK (!syncobj_done (f, out));

  /* a TFU job writes tex; a CL job then uses it: the render waits */
  struct drm_v3d_submit_tfu t = { .icfg = 1, .bo_handles = { tex }, .out_sync = tfu_out };
  CHECK (ioctl_ (f, DRM_IOCTL_V3D_SUBMIT_TFU, &t) == 0);
  CHECK (busy[V3D_TFU]);
  uint32_t handles[] = { tex, cl };
  struct drm_v3d_submit_cl s = {
    .bcl_start = 0x1000, .bcl_end = 0x1100, .rcl_start = 0x2000, .rcl_end = 0x2100,
    .qma = 0x3000, .qms = 0x1000, .qts = 0x4000,
    .bo_handles = (uintptr_t) handles, .bo_handle_count = 2, .out_sync = out,
  };
  CHECK (ioctl_ (f, DRM_IOCTL_V3D_SUBMIT_CL, &s) == 0);
  CHECK (busy[V3D_BIN]);			/* bin doesn't wait for BOs (as Linux) */
  CHECK (regs[V3D_BANK_CORE0][V3D_CLE_CT0QBA / 4] == 0x1000);
  CHECK (regs[V3D_BANK_CORE0][V3D_CLE_CT0QMA / 4] == 0x3000);
  CHECK (regs[V3D_BANK_CORE0][V3D_CLE_CT0QTS / 4] == (0x4000 | V3D_CLE_CT0QTS_ENABLE));
  CHECK (!busy[V3D_RENDER]);

  /* the binner runs out of memory: it gets the pool's */
  uint32_t pool = dev->noverflow_pool;
  regs[V3D_BANK_CORE0][V3D_CTL_INT_STS / 4] |= V3D_INT_OUTOMEM;
  v3d_irq (dev);
  CHECK (dev->noverflow_pool == pool - 1);
  CHECK (regs[V3D_BANK_CORE0][V3D_PTB_BPOS / 4] == V3D_OVERFLOW_BYTES);
  CHECK (regs[V3D_BANK_CORE0][V3D_PTB_BPOA / 4] != 0);

  sim_finish (V3D_BIN);
  CHECK (!busy[V3D_RENDER]);			/* still waiting for the TFU */
  sim_finish (V3D_TFU);
  CHECK (syncobj_done (f, tfu_out));
  CHECK (busy[V3D_RENDER]);
  CHECK (regs[V3D_BANK_CORE0][V3D_CLE_CT1QBA / 4] == 0x2000);
  CHECK (!syncobj_done (f, out));

  struct drm_v3d_wait_bo w = { .handle = cl, .timeout_ns = 0 };
  CHECK (ioctl_ (f, DRM_IOCTL_V3D_WAIT_BO, &w) == -V3D_ETIME);

  /* closing a BO the GPU is using: it stays until the job is done */
  int clbo = dev->files[f].handles[cl];
  struct drm_gem_close gc = { .handle = cl };
  CHECK (ioctl_ (f, DRM_IOCTL_GEM_CLOSE, &gc) == 0);
  CHECK (dev->bos[clbo].used && pt[dev->bos[clbo].first] != 0);

  sim_finish (V3D_RENDER);
  CHECK (syncobj_done (f, out));
  w.handle = tex;
  CHECK (ioctl_ (f, DRM_IOCTL_V3D_WAIT_BO, &w) == 0);
  CHECK (dev->noverflow_pool == pool + 1);	/* overflow memory back (the
						   pool was topped up meanwhile) */
  CHECK (!dev->bos[clbo].used);			/* and the closed BO gone */

  /* a sync file: export, wait, close */
  struct drm_syncobj_handle sh = { .handle = out,
				   .flags = DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_EXPORT_SYNC_FILE };
  CHECK (ioctl_ (f, DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD, &sh) == 0 && sh.fd > 0);
  struct drm_riscos_sync_file sf = { .fd = sh.fd, .timeout_ms = 0 };
  CHECK (ioctl_ (f, DRM_IOCTL_RISCOS_SYNC_FILE_WAIT, &sf) == 0 && sf.signalled);
  uint32_t imp = syncobj (f, 0);
  struct drm_syncobj_handle ih = { .handle = imp, .fd = sh.fd,
				   .flags = DRM_SYNCOBJ_FD_TO_HANDLE_FLAGS_IMPORT_SYNC_FILE };
  CHECK (ioctl_ (f, DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE, &ih) == 0 && syncobj_done (f, imp));
  CHECK (ioctl_ (f, DRM_IOCTL_RISCOS_SYNC_FILE_CLOSE, &sf) == 0);
  CHECK (ioctl_ (f, DRM_IOCTL_RISCOS_SYNC_FILE_WAIT, &sf) == -V3D_EINVAL);

  /* in_sync_bcl: a bin job waits for a sync object */
  uint32_t gate = syncobj (f, 0), out2 = syncobj (f, 0);
  struct drm_v3d_submit_tfu t2 = { .icfg = 1, .bo_handles = { tex }, .out_sync = gate };
  CHECK (ioctl_ (f, DRM_IOCTL_V3D_SUBMIT_TFU, &t2) == 0);
  struct drm_v3d_submit_cl s2 = { .bcl_start = 1, .bcl_end = 2, .rcl_start = 3, .rcl_end = 4,
				  .in_sync_bcl = gate, .out_sync = out2,
				  .flags = DRM_V3D_SUBMIT_CL_FLUSH_CACHE };
  CHECK (ioctl_ (f, DRM_IOCTL_V3D_SUBMIT_CL, &s2) == 0);
  CHECK (!busy[V3D_BIN]);
  sim_finish (V3D_TFU);
  CHECK (busy[V3D_BIN]);
  sim_finish (V3D_BIN);
  sim_finish (V3D_RENDER);
  CHECK (syncobj_done (f, out2));		/* after the cache clean */
  CHECK (dev->stats.jobs[V3D_CACHE_CLEAN] == 1);

  /* a wait that sleeps: the GPU finishes while we wait */
  uint32_t out3 = syncobj (f, 0);
  struct drm_v3d_submit_cl s3 = { .rcl_start = 5, .rcl_end = 6, .out_sync = out3 };
  CHECK (ioctl_ (f, DRM_IOCTL_V3D_SUBMIT_CL, &s3) == 0);
  CHECK (!busy[V3D_BIN] && busy[V3D_RENDER]);	/* no bin when bcl is empty */
  auto_finish = 1;
  struct drm_syncobj_wait sw = { .handles = (uintptr_t) &out3, .timeout_nsec = INT64_MAX,
				 .count_handles = 1, .flags = DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL };
  CHECK (ioctl_ (f, DRM_IOCTL_SYNCOBJ_WAIT, &sw) == 0);
  auto_finish = 0;

  /* unsupported things are refused as Linux refuses them */
  s3.flags = DRM_V3D_SUBMIT_EXTENSION;
  CHECK (ioctl_ (f, DRM_IOCTL_V3D_SUBMIT_CL, &s3) == -V3D_EINVAL);
  s3.flags = 0;
  s3.pad = 1;
  CHECK (ioctl_ (f, DRM_IOCTL_V3D_SUBMIT_CL, &s3) == -V3D_EINVAL);
  uint32_t nobo = 99;
  struct drm_v3d_submit_cl s4 = { .rcl_start = 5, .rcl_end = 6,
				  .bo_handles = (uintptr_t) &nobo, .bo_handle_count = 1 };
  CHECK (ioctl_ (f, DRM_IOCTL_V3D_SUBMIT_CL, &s4) == -V3D_ENOENT);
  CHECK (!busy[V3D_RENDER]);
  CHECK (ioctl_ (f, 0x12345678, &s4) == -V3D_ENOTTY);
  v3d_close (dev, f);
  teardown ();
}

/* an older (shorter) TFU structure, as Mesa 20.3 sends it: no flags,
   extensions or 7.1 fields */
struct old_tfu
{
  uint32_t icfg, iia, iis, ica, iua, ioa, ios, coef[4], bo_handles[4], in_sync, out_sync;
};

static void
test_old_struct_csd_reset (void)
{
  setup (42);
  CHECK (v3d_hw_probe (dev) == 0 && v3d_init (dev) == 0);
  int f = v3d_open (dev, 1);
  uint32_t b = bo_new (f, 4096, 0), out = syncobj (f, 0);
  struct old_tfu t = { .icfg = 5, .iia = 0x1234, .bo_handles = { b }, .out_sync = out };
  uint32_t req = _IOC (_IOC_WRITE, 'd', DRM_COMMAND_BASE + DRM_V3D_SUBMIT_TFU, sizeof t);
  CHECK (sizeof t < sizeof (struct drm_v3d_submit_tfu));
  CHECK (ioctl_ (f, req, &t) == 0);
  CHECK (regs[V3D_BANK_HUB][V3D_TFU_IIA (42) / 4] == 0x1234);
  CHECK (regs[V3D_BANK_HUB][V3D_TFU_ICFG (42) / 4] == (5 | V3D_TFU_ICFG_IOC));
  sim_finish (V3D_TFU);
  CHECK (syncobj_done (f, out));

  /* compute, then its cache clean */
  uint32_t cout = syncobj (f, 0);
  struct drm_v3d_submit_csd c = { .cfg = { 0x10000, 0x10000, 0x10000, 0, 7, 0x100, 0x200 },
				  .bo_handles = (uintptr_t) &b, .bo_handle_count = 1,
				  .out_sync = cout };
  CHECK (ioctl_ (f, DRM_IOCTL_V3D_SUBMIT_CSD, &c) == 0);
  CHECK (busy[V3D_CSD]);
  CHECK (regs[V3D_BANK_CORE0][V3D_CSD_QUEUED_CFG0 (42) / 4] == 0x10000);
  CHECK (regs[V3D_BANK_CORE0][(V3D_CSD_QUEUED_CFG0 (42) + 24) / 4] == 0x200);
  CHECK (!syncobj_done (f, cout));
  sim_finish (V3D_CSD);
  CHECK (syncobj_done (f, cout));

  /* a hang: the render job makes no progress for 500 ms */
  uint32_t hout = syncobj (f, 0);
  struct drm_v3d_submit_cl s = { .rcl_start = 0x10, .rcl_end = 0x20, .out_sync = hout };
  CHECK (ioctl_ (f, DRM_IOCTL_V3D_SUBMIT_CL, &s) == 0);
  CHECK (busy[V3D_RENDER]);
  now_cs += 20;
  v3d_service (dev);				/* notes the start */
  now_cs += 60;
  regs[V3D_BANK_CORE0][V3D_CLE_CTNCA (1) / 4] = 0x18;	/* it moved */
  v3d_service (dev);
  CHECK (resets == 0);
  now_cs += 30;
  v3d_service (dev);
  CHECK (resets == 0);				/* not 500 ms since */
  now_cs += 30;
  v3d_service (dev);				/* and then it hadn't moved */
  CHECK (resets == 1 && dev->stats.timeouts == 1);
  CHECK (dev->stats.first_hang.q == V3D_RENDER && dev->stats.first_hang.ct1ca == 0x18
	 && dev->stats.first_hang.start == 0x10 && dev->stats.first_hang.end == 0x20
	 && dev->stats.first_hang.words_at == 0x08 && dev->stats.first_hang.words[0] == 0);
  CHECK (dev->stats.last_hang.q == V3D_RENDER);
  CHECK (syncobj_done (f, hout));		/* signalled, as failed */
  struct drm_v3d_get_param rc = { .param = DRM_V3D_PARAM_GLOBAL_RESET_COUNTER };
  CHECK (ioctl_ (f, DRM_IOCTL_V3D_GET_PARAM, &rc) == 0 && rc.value == 1);
  rc.param = DRM_V3D_PARAM_CONTEXT_RESET_COUNTER;
  rc.value = 0;
  CHECK (ioctl_ (f, DRM_IOCTL_V3D_GET_PARAM, &rc) == 0 && rc.value == 1);
  CHECK (regs[V3D_BANK_HUB][V3D_MMU_CTL / 4] & V3D_MMU_CTL_ENABLE);	/* MMU back */

  /* a bin job that hangs: its render job is never run */
  uint32_t bout = syncobj (f, 0);
  struct drm_v3d_submit_cl bh = { .bcl_start = 0x30, .bcl_end = 0x31, .rcl_start = 0x40,
				  .rcl_end = 0x50, .out_sync = bout };
  CHECK (ioctl_ (f, DRM_IOCTL_V3D_SUBMIT_CL, &bh) == 0);
  CHECK (busy[V3D_BIN] && !busy[V3D_RENDER]);
  uint32_t rstarts = 0;
  regs[V3D_BANK_CORE0][V3D_CLE_CT1QBA / 4] = 0;
  now_cs += 20;
  v3d_service (dev);
  now_cs += 60;
  v3d_service (dev);
  CHECK (resets == 2 && dev->stats.timeouts == 2);
  CHECK (dev->stats.last_hang.q == V3D_BIN && dev->stats.last_hang.start == 0x30
	 && dev->stats.first_hang.q == V3D_RENDER);
  CHECK (!busy[V3D_RENDER] && regs[V3D_BANK_CORE0][V3D_CLE_CT1QBA / 4] != 0x40);
  CHECK (syncobj_done (f, bout));		/* the render job ended unrun */
  (void) rstarts;

  /* the GPU still works after the reset */
  uint32_t aout = syncobj (f, 0);
  s.out_sync = aout;
  CHECK (ioctl_ (f, DRM_IOCTL_V3D_SUBMIT_CL, &s) == 0);
  sim_finish (V3D_RENDER);
  CHECK (syncobj_done (f, aout));

  /* (0.10) a job that keeps moving for ever is reset after 5 s */
  uint32_t lout = syncobj (f, 0);
  s.out_sync = lout;
  CHECK (ioctl_ (f, DRM_IOCTL_V3D_SUBMIT_CL, &s) == 0);
  int r0 = resets;
  v3d_service (dev);				/* notes the start */
  for (int i = 0; i < 60 && resets == r0; i++)
    {
      now_cs += 10;
      regs[V3D_BANK_CORE0][V3D_CLE_CTNCA (1) / 4] += 8;	/* always moving */
      v3d_service (dev);
    }
  CHECK (resets == r0 + 1);
  CHECK (syncobj_done (f, lout));

  /* (0.10) recovery: a hung job with others queued behind it, all ended */
  uint32_t c1 = syncobj (f, 0), c2 = syncobj (f, 0), c3 = syncobj (f, 0);
  uint32_t cb = bo_new (f, 65536, 0);
  int cbo = dev->files[f].handles[cb];
  struct drm_v3d_submit_cl k1 = { .rcl_start = 0x10, .rcl_end = 0x20, .out_sync = c1,
				  .bo_handles = (uintptr_t) &cb, .bo_handle_count = 1 };
  CHECK (ioctl_ (f, DRM_IOCTL_V3D_SUBMIT_CL, &k1) == 0);
  k1.out_sync = c2;
  CHECK (ioctl_ (f, DRM_IOCTL_V3D_SUBMIT_CL, &k1) == 0);
  k1.out_sync = c3;
  CHECK (ioctl_ (f, DRM_IOCTL_V3D_SUBMIT_CL, &k1) == 0);
  CHECK (busy[V3D_RENDER] && !syncobj_done (f, c1) && !syncobj_done (f, c3));
  struct drm_gem_close gc = { .handle = cb };
  CHECK (ioctl_ (f, DRM_IOCTL_GEM_CLOSE, &gc) == 0);
  CHECK (dev->bos[cbo].refs > 0);		/* the jobs still hold it */
  r0 = resets;
  v3d_cancel_all (dev);
  CHECK (resets == r0 + 1);
  CHECK (syncobj_done (f, c1) && syncobj_done (f, c2) && syncobj_done (f, c3));
  CHECK (!busy[V3D_RENDER] && !dev->head[V3D_RENDER] && !dev->active[V3D_RENDER]);
  CHECK (dev->bos[cbo].refs == 0);		/* and let it go */
  v3d_cancel_all (dev);				/* with nothing to do */
  CHECK (resets == r0 + 2);
  /* and it still works */
  uint32_t c4 = syncobj (f, 0);
  s.out_sync = c4;
  CHECK (ioctl_ (f, DRM_IOCTL_V3D_SUBMIT_CL, &s) == 0);
  sim_finish (V3D_RENDER);
  CHECK (syncobj_done (f, c4));

  /* (0.11) a cached BO: mapped cacheable, cleaned at submit, invalidated
     at the file's next request after the job */
  struct drm_v3d_create_bo cc = { .size = 8192, .flags = DRM_RISCOS_BO_CPU_CACHED };
  CHECK (ioctl_ (f, DRM_IOCTL_V3D_CREATE_BO, &cc) == 0 && last_cached == 1);
  int ccbo = dev->files[f].handles[cc.handle];
  CHECK (dev->bos[ccbo].cached && dev->bos[ccbo].file == f);
  struct drm_v3d_create_bo bad = { .size = 4096, .flags = 1 };
  CHECK (ioctl_ (f, DRM_IOCTL_V3D_CREATE_BO, &bad) == -V3D_EINVAL);
  unsigned cl0 = cache_cleans, in0 = cache_invals;
  uint32_t c5 = syncobj (f, 0);
  struct drm_v3d_submit_cl k5 = { .rcl_start = 0x10, .rcl_end = 0x20, .out_sync = c5,
				  .bo_handles = (uintptr_t) &cc.handle, .bo_handle_count = 1 };
  CHECK (ioctl_ (f, DRM_IOCTL_V3D_SUBMIT_CL, &k5) == 0);
  CHECK (cache_cleans == cl0 + 1 && cache_invals == in0);
  sim_finish (V3D_RENDER);
  CHECK (syncobj_done (f, c5));			/* a request: the wait */
  CHECK (cache_invals == in0 + 1 && !dev->bos[ccbo].stale && dev->files[f].nstale == 0);
  struct drm_v3d_get_param gp = { .param = DRM_V3D_PARAM_SUPPORTS_TFU };
  CHECK (ioctl_ (f, DRM_IOCTL_V3D_GET_PARAM, &gp) == 0 && cache_invals == in0 + 1);	/* once */
  /* an ordinary BO: no cache work */
  uint32_t ob = bo_new (f, 4096, 0);
  cl0 = cache_cleans;
  k5.bo_handles = (uintptr_t) &ob;
  CHECK (ioctl_ (f, DRM_IOCTL_V3D_SUBMIT_CL, &k5) == 0 && cache_cleans == cl0);
  sim_finish (V3D_RENDER);

  /* (0.11) importing the program's memory */
  static uint8_t mem[5 * 4096] __attribute__ ((aligned (4096)));
  struct drm_riscos_import_memory im = { .address = (uintptr_t) mem, .size = 3 * 4096 };
  uint32_t used0 = dev->pages_used;
  CHECK (ioctl_ (f, DRM_IOCTL_RISCOS_IMPORT_MEMORY, &im) == 0 && im.handle && im.offset);
  int ibo = dev->files[f].handles[im.handle];
  CHECK (dev->bos[ibo].imported && dev->bos[ibo].npages == 3 && dev->pages_used == used0);
  for (uint32_t i = 0; i < 3; i++)
    CHECK ((pt[(im.offset >> 12) + i] & 0xFFFFFF)
	   == (uint32_t) ((0x400000000ull + (uintptr_t) mem + i * 4096) >> 12 & 0xFFFFFF)
	   && (pt[(im.offset >> 12) + i] & (1u << 28)));
  struct drm_v3d_mmap_bo mm = { .handle = im.handle };
  CHECK (ioctl_ (f, DRM_IOCTL_V3D_MMAP_BO, &mm) == 0 && mm.offset == (uintptr_t) mem);
  struct drm_riscos_import_memory im2 = { .address = (uintptr_t) mem + 4, .size = 4096 };
  CHECK (ioctl_ (f, DRM_IOCTL_RISCOS_IMPORT_MEMORY, &im2) == -V3D_EINVAL);	/* unaligned */
  im2.address = (uintptr_t) mem;
  im2.size = 100;
  CHECK (ioctl_ (f, DRM_IOCTL_RISCOS_IMPORT_MEMORY, &im2) == -V3D_EINVAL);
  im2.size = 0;
  CHECK (ioctl_ (f, DRM_IOCTL_RISCOS_IMPORT_MEMORY, &im2) == -V3D_EINVAL);
  /* used by a job, then closed: kept until the job is done, then out of the MMU */
  cl0 = cache_cleans;
  in0 = cache_invals;
  k5.bo_handles = (uintptr_t) &im.handle;
  CHECK (ioctl_ (f, DRM_IOCTL_V3D_SUBMIT_CL, &k5) == 0 && cache_cleans == cl0 + 1);
  struct drm_gem_close gci = { .handle = im.handle };
  CHECK (ioctl_ (f, DRM_IOCTL_GEM_CLOSE, &gci) == 0);
  CHECK (dev->bos[ibo].used && (pt[im.offset >> 12] & (1u << 28)));
  sim_finish (V3D_RENDER);
  v3d_service (dev);
  CHECK (!dev->bos[ibo].used && !(pt[im.offset >> 12] & (1u << 28)));
  CHECK (dev->pages_used == used0);

  /* a client that goes away with its jobs still running */
  int g = v3d_open (dev, 2);
  uint32_t gb = bo_new (g, 65536, 0);
  struct drm_v3d_submit_cl gs = { .rcl_start = 1, .rcl_end = 2,
				  .bo_handles = (uintptr_t) &gb, .bo_handle_count = 1 };
  CHECK (ioctl_ (g, DRM_IOCTL_V3D_SUBMIT_CL, &gs) == 0);
  v3d_close (dev, g);
  CHECK (!v3d_file_valid (dev, g));
  sim_finish (V3D_RENDER);
  v3d_close (dev, f);
  teardown ();
}

/* (0.13) riscos-mesa's four: aligned BOs for 64K/1M entries, busy time
   per file, uncached imports, innocent jobs run again after a reset */
static void
test_013 (void)
{
  setup (42);
  CHECK (v3d_hw_probe (dev) == 0 && v3d_init (dev) == 0);
  int f = v3d_open (dev, 1), g = v3d_open (dev, 2);

  /* alignment: 1 MB BOs on a 1 MB GPU address, 64 KB ones on 64 KB */
  uint32_t o4k, o1m, o64k, o20k;
  bo_new (f, 4096, &o4k);
  uint32_t h1m = bo_new (f, 1 << 20, &o1m);
  bo_new (f, 65536, &o64k);
  bo_new (f, 20480, &o20k);
  CHECK ((o1m & 0xFFFFF) == 0 && (o64k & 0xFFFF) == 0);
  CHECK (o20k < o1m);				/* small ones still fill holes */
  uint32_t p1m = o1m >> 12, p64k = o64k >> 12;
  /* the sim's first 512 pages are physically contiguous: a super page */
  CHECK (p1m >= 2048 && (pt[p1m] & (1u << 31)) && (pt[p1m + 255] & (1u << 31)));
  /* later pages are contiguous in aligned 16s: big pages */
  CHECK ((pt[p64k] & (1u << 30)) && (pt[p64k + 15] & (1u << 30)) && !(pt[p64k] & (1u << 31)));
  (void) h1m;

  /* busy time per file (DRM_IOCTL_RISCOS_FILE_STATS) */
  struct drm_riscos_file_stats st;
  memset (&st, 0xEE, sizeof st);
  CHECK (ioctl_ (f, DRM_IOCTL_RISCOS_FILE_STATS, &st) == 0);
  CHECK (st.file[V3D_BIN].jobs == 0 && st.file[V3D_RENDER].busy_ns == 0);
  struct drm_v3d_submit_cl s = { .bcl_start = 0x30, .bcl_end = 0x31, .rcl_start = 0x40,
				 .rcl_end = 0x50 };
  CHECK (ioctl_ (f, DRM_IOCTL_V3D_SUBMIT_CL, &s) == 0);
  sim_finish (V3D_BIN);
  sim_finish (V3D_RENDER);
  uint32_t b = bo_new (g, 4096, 0);
  struct drm_v3d_submit_csd c = { .cfg = { 0x10000, 0x10000, 0x10000, 0, 7, 0x100, 0x200 },
				  .bo_handles = (uintptr_t) &b, .bo_handle_count = 1 };
  CHECK (ioctl_ (g, DRM_IOCTL_V3D_SUBMIT_CSD, &c) == 0);
  sim_finish (V3D_CSD);
  v3d_service (dev);
  CHECK (ioctl_ (f, DRM_IOCTL_RISCOS_FILE_STATS, &st) == 0);
  CHECK (st.file[V3D_BIN].jobs == 1 && st.file[V3D_RENDER].jobs == 1
	 && st.file[V3D_BIN].busy_ns > 0 && st.file[V3D_RENDER].busy_ns > 0
	 && st.file[V3D_CSD].jobs == 0 && st.file[V3D_CSD].busy_ns == 0);
  CHECK (st.all[V3D_CSD].jobs == 1 && st.all[V3D_CACHE_CLEAN].jobs == 1
	 && st.all[V3D_CACHE_CLEAN].busy_ns > 0
	 && st.all[V3D_BIN].busy_ns == st.file[V3D_BIN].busy_ns);
  struct drm_riscos_file_stats sg;
  CHECK (ioctl_ (g, DRM_IOCTL_RISCOS_FILE_STATS, &sg) == 0);
  CHECK (sg.file[V3D_CSD].jobs == 1 && sg.file[V3D_CACHE_CLEAN].jobs == 1
	 && sg.file[V3D_CSD].busy_ns > 0 && sg.file[V3D_BIN].jobs == 0);
  CHECK (dev->stats.busy_ns[V3D_CSD] == sg.file[V3D_CSD].busy_ns);

  /* an uncached import: no cache maintenance; unknown flags refused */
  static uint8_t scr[4 * 4096] __attribute__ ((aligned (4096)));
  struct drm_riscos_import_memory im = { .address = (uintptr_t) scr, .size = sizeof scr,
					 .flags = DRM_RISCOS_IMPORT_UNCACHED };
  CHECK (ioctl_ (f, DRM_IOCTL_RISCOS_IMPORT_MEMORY, &im) == 0 && im.handle);
  CHECK (dev->bos[dev->files[f].handles[im.handle]].nocache);
  unsigned cl0 = cache_cleans, in0 = cache_invals;
  struct drm_v3d_submit_cl k = { .rcl_start = 0x10, .rcl_end = 0x20,
				 .bo_handles = (uintptr_t) &im.handle, .bo_handle_count = 1 };
  CHECK (ioctl_ (f, DRM_IOCTL_V3D_SUBMIT_CL, &k) == 0);
  sim_finish (V3D_RENDER);
  v3d_service (dev);
  struct drm_v3d_get_param gp = { .param = DRM_V3D_PARAM_SUPPORTS_TFU };
  CHECK (ioctl_ (f, DRM_IOCTL_V3D_GET_PARAM, &gp) == 0);
  CHECK (cache_cleans == cl0 && cache_invals == in0 && dev->files[f].nstale == 0);
  struct drm_riscos_import_memory im2 = { .address = (uintptr_t) scr, .size = 4096, .flags = 2 };
  CHECK (ioctl_ (f, DRM_IOCTL_RISCOS_IMPORT_MEMORY, &im2) == -V3D_EINVAL);
  im2.flags = 0;					/* a cached one still is */
  CHECK (ioctl_ (f, DRM_IOCTL_RISCOS_IMPORT_MEMORY, &im2) == 0);
  k.bo_handles = (uintptr_t) &im2.handle;
  CHECK (ioctl_ (f, DRM_IOCTL_V3D_SUBMIT_CL, &k) == 0 && cache_cleans == cl0 + 1);
  sim_finish (V3D_RENDER);

  /* a render job hangs while the next frame's bin job runs: the bin
     job is run again (Linux's resubmit), and its render job then runs */
  uint32_t o1 = syncobj (f, 0), o2 = syncobj (f, 0);
  s.out_sync = o1;
  CHECK (ioctl_ (f, DRM_IOCTL_V3D_SUBMIT_CL, &s) == 0);
  sim_finish (V3D_BIN);				/* frame 1's render job starts */
  struct drm_v3d_submit_cl s2 = { .bcl_start = 0x60, .bcl_end = 0x61, .rcl_start = 0x70,
				  .rcl_end = 0x80, .out_sync = o2 };
  CHECK (ioctl_ (f, DRM_IOCTL_V3D_SUBMIT_CL, &s2) == 0);
  CHECK (busy[V3D_BIN] && busy[V3D_RENDER]);	/* both on the GPU */
  regs[V3D_BANK_CORE0][V3D_CLE_CT0QBA / 4] = 0;
  int r0 = resets;
  now_cs += 20;
  v3d_service (dev);
  now_cs += 60;
  regs[V3D_BANK_CORE0][V3D_CLE_CTNCA (0) / 4] += 8;	/* the bin job is moving */
  v3d_service (dev);
  CHECK (resets == r0 + 1 && dev->stats.requeued == 1);
  CHECK (syncobj_done (f, o1) && !syncobj_done (f, o2));
  CHECK (busy[V3D_BIN] && regs[V3D_BANK_CORE0][V3D_CLE_CT0QBA / 4] == 0x60);	/* again */
  sim_finish (V3D_BIN);
  CHECK (busy[V3D_RENDER] && regs[V3D_BANK_CORE0][V3D_CLE_CT1QBA / 4] == 0x70);
  sim_finish (V3D_RENDER);
  CHECK (syncobj_done (f, o2));
  struct drm_v3d_get_param rc = { .param = DRM_V3D_PARAM_CONTEXT_RESET_COUNTER };
  CHECK (ioctl_ (f, DRM_IOCTL_V3D_GET_PARAM, &rc) == 0 && rc.value == 1);	/* the guilty one */

  /* *V3DReset (cancel_all) still ends everything, innocent or not */
  s.out_sync = o1;
  CHECK (ioctl_ (f, DRM_IOCTL_V3D_SUBMIT_CL, &s) == 0);
  sim_finish (V3D_BIN);
  s2.out_sync = o2;
  CHECK (ioctl_ (f, DRM_IOCTL_V3D_SUBMIT_CL, &s2) == 0);
  CHECK (busy[V3D_BIN] && busy[V3D_RENDER]);
  v3d_cancel_all (dev);
  CHECK (!busy[V3D_BIN] && !busy[V3D_RENDER] && dev->stats.requeued == 1);
  CHECK (syncobj_done (f, o1) && syncobj_done (f, o2) && !dev->head[V3D_RENDER]);

  v3d_close (dev, g);
  v3d_close (dev, f);
  teardown ();
}

/* (0.15) a cached BO used by two jobs: invalidated once, after the last */
static void
test_015 (void)
{
  setup (42);
  CHECK (v3d_hw_probe (dev) == 0 && v3d_init (dev) == 0);
  int f = v3d_open (dev, 1);
  struct drm_v3d_create_bo cc = { .size = 8192, .flags = DRM_RISCOS_BO_CPU_CACHED };
  CHECK (ioctl_ (f, DRM_IOCTL_V3D_CREATE_BO, &cc) == 0);
  int bo = dev->files[f].handles[cc.handle];
  uint32_t o1 = syncobj (f, 0), o2 = syncobj (f, 0);
  struct drm_v3d_submit_cl k = { .rcl_start = 0x10, .rcl_end = 0x20, .out_sync = o1,
				 .bo_handles = (uintptr_t) &cc.handle, .bo_handle_count = 1 };
  unsigned cl0 = cache_cleans, in0 = cache_invals;
  CHECK (ioctl_ (f, DRM_IOCTL_V3D_SUBMIT_CL, &k) == 0);
  k.out_sync = o2;
  CHECK (ioctl_ (f, DRM_IOCTL_V3D_SUBMIT_CL, &k) == 0);	/* queued behind */
  CHECK (cache_cleans == cl0 + 2 && dev->stats.cache_cleans == 2);
  sim_finish (V3D_RENDER);				/* the first: the second starts */
  CHECK (syncobj_done (f, o1) && !syncobj_done (f, o2));
  CHECK (cache_invals == in0 && dev->bos[bo].stale && dev->files[f].nstale == 1
	 && dev->stats.cache_deferred >= 1);		/* put off */
  sim_finish (V3D_RENDER);
  CHECK (syncobj_done (f, o2));
  CHECK (cache_invals == in0 + 1 && !dev->bos[bo].stale && dev->files[f].nstale == 0
	 && dev->stats.cache_invals == 1 && dev->stats.cache_ns > 0);
  struct drm_v3d_get_param gp = { .param = DRM_V3D_PARAM_SUPPORTS_TFU };
  CHECK (ioctl_ (f, DRM_IOCTL_V3D_GET_PARAM, &gp) == 0 && cache_invals == in0 + 1);
  v3d_close (dev, f);
  teardown ();
}

/* (0.17) the desktop's copies: the framestore mapped, a copy's render
   list (dumped for test/host/cl2d.py), the job (render only), async,
   declined while 3D work is on the GPU, forget, stop */
static void
test_017 (void)
{
  setup (42);
  CHECK (v3d_hw_probe (dev) == 0 && v3d_init (dev) == 0);
  CHECK (dev->twod.file == -1);
  struct v3d_2d_copy c = { 0, 0, 7680, 100, 100, 4, 0, 0 };
  CHECK (v3d_2d_copy (dev, &c) == -V3D_EINVAL);		/* not started */
  CHECK (v3d_2d_start (dev) == 0 && dev->twod.file >= 0 && dev->twod.cl_cpu);
  CHECK (v3d_2d_copy (dev, &c) == -V3D_EINVAL);		/* no screen */
  /* a 1920x1080 32bpp framestore at a 1 MB-aligned &3E000000 + 64 KB */
  uint64_t fs = 0x3E010000ull;
  CHECK (v3d_2d_screen (dev, fs, 1920 * 1080 * 4) == 0);
  int sbo = dev->files[dev->twod.file].handles[dev->twod.scr_handle];
  CHECK (sbo && dev->bos[sbo].imported && dev->bos[sbo].nocache && !dev->bos[sbo].cpu);
  uint32_t p0 = dev->bos[sbo].first;
  CHECK ((p0 & 255) == 0 && (pt[p0] & 0xFFFFFF) == 0x3E000 && (pt[p0] & (1u << 31)));	/* 1 MB entries */
  CHECK (dev->twod.scr_gpu == p0 * 4096 + 0x10000);
  CHECK (v3d_2d_screen (dev, fs, 1920 * 1080 * 4) == 0
	 && dev->files[dev->twod.file].handles[dev->twod.scr_handle] == sbo);	/* kept */
  /* outside the framestore */
  struct v3d_2d_copy bad = { 1920 * 1079 * 4, 0, 7680, 2, 2, 4, 0, 0 };
  CHECK (v3d_2d_copy (dev, &bad) == -V3D_EINVAL);
  struct v3d_2d_copy wide = { 0, 0, 7680, 1921, 2, 4, 0, 0 };
  CHECK (v3d_2d_copy (dev, &wide) == -V3D_EINVAL);
  /* a 200x100 copy, left running */
  c = (struct v3d_2d_copy) { 10 * 7680 + 40, 300 * 7680 + 800, 7680, 200, 100, 4, 0, 0 };
  CHECK (v3d_2d_copy (dev, &c) == 0 && dev->twod.pending && busy[V3D_RENDER] && !busy[V3D_BIN]);
  CHECK (regs[V3D_BANK_CORE0][V3D_CLE_CT1QBA / 4] == dev->twod.cl_gpu);
  uint32_t len = regs[V3D_BANK_CORE0][V3D_CLE_CT1QEA / 4] - dev->twod.cl_gpu;
  CHECK (len == 52 + 4 * 2 * 32 + 1);			/* 4x2 tiles */
  FILE *o = fopen ("out/cl2d/base", "w");
  if (o)
    {
      fprintf (o, "%u\n", dev->twod.scr_gpu);
      fclose (o);
    }
  o = fopen ("out/cl2d/copy-200x100.bin", "wb");
  if (o)
    {
      fwrite (dev->twod.cl_cpu, 1, len, o);
      fclose (o);
    }
  sim_finish (V3D_RENDER);
  CHECK (v3d_2d_sync (dev) == 0 && !dev->twod.pending);
  /* a scroll down and right, waited for: tiles from the far end */
  c = (struct v3d_2d_copy) { 0, 7680 * 20 + 8 * 4, 7680, 130, 70, 4,
			     V3D_2D_COLS_DESCENDING | V3D_2D_ROWS_DESCENDING, 0 };
  CHECK (v3d_2d_copy (dev, &c) == 0);
  len = regs[V3D_BANK_CORE0][V3D_CLE_CT1QEA / 4] - dev->twod.cl_gpu;
  o = fopen ("out/cl2d/scroll-130x70.bin", "wb");
  if (o)
    {
      fwrite (dev->twod.cl_cpu, 1, len, o);
      fclose (o);
    }
  /* the next copy waits for it first (the list's buffer is reused) */
  sim_finish (V3D_RENDER);
  c = (struct v3d_2d_copy) { 0, 7680 * 200, 7680, 64, 64, 4, 0, 0 };
  CHECK (v3d_2d_copy (dev, &c) == 0 && dev->twod.copies == 3 && dev->twod.scrolls == 1);
  sim_finish (V3D_RENDER);
  CHECK (v3d_2d_sync (dev) == 0);
  /* 16bpp: BGR565 (format 7) */
  c = (struct v3d_2d_copy) { 0, 3840 * 300, 3840, 100, 64, 2, 0, 0 };
  CHECK (v3d_2d_copy (dev, &c) == 0);
  len = regs[V3D_BANK_CORE0][V3D_CLE_CT1QEA / 4] - dev->twod.cl_gpu;
  o = fopen ("out/cl2d/copy16-100x64.bin", "wb");
  if (o)
    {
      fwrite (dev->twod.cl_cpu, 1, len, o);
      fclose (o);
    }
  sim_finish (V3D_RENDER);
  CHECK (v3d_2d_sync (dev) == 0);
  /* async: lists queue behind each other in the buffer's four parts */
  c = (struct v3d_2d_copy) { 0, 7680 * 200, 7680, 64, 64, 4, 0, 0 };
  uint32_t waits = dev->twod.waits;
  CHECK (v3d_2d_copy (dev, &c) == 0 && v3d_2d_copy (dev, &c) == 0 && dev->twod.pending == 2
	 && dev->twod.slot == 2 && dev->twod.waits == waits);
  CHECK (regs[V3D_BANK_CORE0][V3D_CLE_CT1QBA / 4] == dev->twod.cl_gpu
	 && dev->head[V3D_RENDER] && dev->head[V3D_RENDER]->a.cl.start == dev->twod.cl_gpu + (128u * 1024 - 64) / 4 / 64 * 64);
  struct v3d_2d_fill fl = { 7680 * 500 + 40, 7680, 300, 200, 4, 0x00FF8040, 0 };
  CHECK (v3d_2d_fill (dev, &fl) == 0 && dev->twod.pending == 3 && dev->twod.fills == 1);
  CHECK (v3d_2d_copy (dev, &c) == 0 && dev->twod.pending == 4 && dev->twod.slot == 0);
  for (int i = 0; i < 4; i++)
    sim_finish (V3D_RENDER);
  {
    uint32_t at = 2 * ((128u * 1024 - 64) / 4 / 64 * 64);
    len = v3d_2d_fill_rcl (dev->twod.cl_cpu + 100000, 20000, dev->twod.cl_gpu + 128 * 1024 - 64,
			   dev->twod.scr_gpu + fl.dst, 7680, 300, 200, 27, 0x00FF8040);
    CHECK (len && !memcmp (dev->twod.cl_cpu + at, dev->twod.cl_cpu + 100000, len));
    o = fopen ("out/cl2d/fill-300x200.bin", "wb");
    if (o)
      {
	fwrite (dev->twod.cl_cpu + at, 1, len, o);
	fclose (o);
      }
  }
  /* a fifth waits for all four, then starts again at the first part */
  CHECK (v3d_2d_copy (dev, &c) == 0 && dev->twod.waits == waits + 1 && dev->twod.pending == 1
	 && dev->twod.slot == 1 && regs[V3D_BANK_CORE0][V3D_CLE_CT1QBA / 4] == dev->twod.cl_gpu);
  sim_finish (V3D_RENDER);
  CHECK (v3d_2d_sync (dev) == 0);
  /* a list bigger than a part: everything waited for, the whole buffer */
  CHECK (v3d_2d_screen (dev, fs, 5120 * 2048) == 0);	/* 2560x2048 at 16bpp */
  CHECK (v3d_2d_copy (dev, &c) == 0 && dev->twod.pending == 1);
  struct v3d_2d_copy big = { 0, 5120, 5120, 2560, 2047, 2, V3D_2D_ROWS_DESCENDING, 0 };
  waits = dev->twod.waits;
  sim_finish (V3D_RENDER);
  CHECK (v3d_2d_copy (dev, &big) == 0 && dev->twod.waits == waits + 1
	 && dev->twod.pending == V3D_2D_SLOTS && dev->twod.slot == 0
	 && regs[V3D_BANK_CORE0][V3D_CLE_CT1QBA / 4] == dev->twod.cl_gpu
	 && regs[V3D_BANK_CORE0][V3D_CLE_CT1QEA / 4] - dev->twod.cl_gpu == 52 + 40 * 32 * 32 + 1);
  sim_finish (V3D_RENDER);
  CHECK (v3d_2d_copy (dev, &c) == 0 && dev->twod.waits == waits + 2 && dev->twod.slot == 1);
  sim_finish (V3D_RENDER);
  CHECK (v3d_2d_sync (dev) == 0);
  sbo = dev->files[dev->twod.file].handles[dev->twod.scr_handle];
  /* a program's 3D job on the GPU: declined, nothing submitted */
  int f = v3d_open (dev, 1);
  struct drm_v3d_submit_cl k = { .rcl_start = 0x10, .rcl_end = 0x20 };
  CHECK (ioctl_ (f, DRM_IOCTL_V3D_SUBMIT_CL, &k) == 0 && busy[V3D_RENDER]);
  uint32_t qba = regs[V3D_BANK_CORE0][V3D_CLE_CT1QBA / 4];
  CHECK (v3d_2d_copy (dev, &c) == -V3D_EBUSY && dev->twod.busy == 1
	 && regs[V3D_BANK_CORE0][V3D_CLE_CT1QBA / 4] == qba);
  sim_finish (V3D_RENDER);
  v3d_service (dev);
  CHECK (v3d_2d_copy (dev, &c) == 0);
  /* the job ended by a reset: an error (the caller turns 2D off) */
  regs[V3D_BANK_CORE0][V3D_CLE_CT1CA / 4] = 0x1234;
  now_cs += 20;
  v3d_service (dev);
  now_cs += 60;
  CHECK (v3d_2d_sync (dev) == -V3D_EIO && dev->twod.failed == 1);
  /* the screen forgotten (a mode change), then stopped */
  v3d_2d_forget_screen (dev);
  CHECK (!dev->twod.scr_handle && !dev->bos[sbo].used);
  CHECK (v3d_2d_copy (dev, &c) == -V3D_EINVAL);
  int tf = dev->twod.file;
  v3d_2d_stop (dev);
  CHECK (dev->twod.file == -1 && !dev->files[tf].used);
  v3d_close (dev, f);
  teardown ();
}

/* (0.17) the copies' render lists carried out as the GPU does them: tile
   by tile, each loaded from the source and stored to the destination,
   clipped to the frame. Checked against memmove (an overlapping copy must
   look as if the source were read first), at both depths, for shifts in
   every direction, small and large. */
static uint8_t fb2d[256 * 1024], want2d[sizeof fb2d];

static int
exec_2d (const uint8_t *cl, uint32_t len, uint32_t base)
{
  uint32_t w = 0, h = 0, col = 0, row = 0, clear = 0;
  int rgba = 0;				/* the buffer holds RGBA8 (cleared), not loaded pixels */
  static uint8_t tile[64 * 64 * 4];
  for (uint32_t i = 0; i < len;)
    {
      uint8_t op = cl[i];
      uint64_t g;
      uint32_t addr, stride, bpp, fmt;
      switch (op)
	{
	case 121:
	  if ((cl[i + 1] & 15) == 0)
	    {
	      w = cl[i + 2] | cl[i + 3] << 8;
	      h = cl[i + 4] | cl[i + 5] << 8;
	    }
	  if ((cl[i + 1] & 15) == 3)
	    memcpy (&clear, cl + i + 2, 4);
	  i += 9;
	  break;
	case 25:
	  for (uint32_t k = 0; k < 64 * 64; k++)
	    memcpy (tile + 4 * k, &clear, 4);
	  rgba = 1;
	  i += 2;
	  break;
	case 122: i += 9; break;
	case 123: i += 5; break;
	case 126: i += 2; break;
	case 124:
	  col = (cl[i + 1] | cl[i + 2] << 8 | cl[i + 3] << 16) & 0xFFF;
	  row = (cl[i + 1] | cl[i + 2] << 8 | cl[i + 3] << 16) >> 12;
	  i += 4;
	  break;
	case 30:
	case 29:
	  memcpy (&g, cl + i + 1, 8);
	  memcpy (&addr, cl + i + 9, 4);
	  if (op == 29 && (g & 0xF) == 8)
	    {
	      i += 13;			/* stores nothing */
	      break;
	    }
	  if (op == 30)
	    rgba = 0;
	  fmt = (g >> 12) & 63;
	  stride = (g >> 28) & 0xFFFFF;
	  bpp = fmt == 27 ? 4 : fmt == 7 ? 2 : 0;
	  if (!bpp || (g & 0xFF) || addr < base)
	    return 0;
	  for (uint32_t y = 0; y < 64 && row * 64 + y < h; y++)
	    for (uint32_t x = 0; x < 64 && col * 64 + x < w; x++)
	      {
		uint32_t off = addr - base + (row * 64 + y) * stride + (col * 64 + x) * bpp;
		if (off + bpp > sizeof fb2d)
		  return 0;
		const uint8_t *tp = tile + (y * 64 + x) * 4;
		if (op == 30)
		  memcpy (tile + (y * 64 + x) * 4, fb2d + off, bpp);
		else if (rgba && bpp == 2)	/* RGBA8 to BGR565, rounded */
		  {
		    uint16_t v = (uint16_t) (((tp[0] * 31 + 127) / 255) << 11
					     | ((tp[1] * 63 + 127) / 255) << 5
					     | (tp[2] * 31 + 127) / 255);
		    memcpy (fb2d + off, &v, 2);
		  }
		else
		  memcpy (fb2d + off, tp, bpp);
	      }
	  i += 13;
	  break;
	case 26: case 27: case 1: i += 1; break;	/* (1: NOP, padding) */
	case 13: return i + 1 == len;
	default: return 0;
	}
    }
  return 0;
}

static void
test_017_exec (void)
{
  static uint8_t cl[128 * 1024];
  uint32_t seed = 1, bad = 0, runs = 0;
  for (int bpp = 2; bpp <= 4; bpp += 2)
    for (int t = 0; t < 400; t++)
      {
	seed = seed * 1103515245u + 12345u;
	uint32_t stride = 400 * (uint32_t) bpp, rows = sizeof fb2d / stride;
	uint32_t w = 1 + (seed >> 8) % 300, h = 1 + (seed >> 3) % 150;
	seed = seed * 1103515245u + 12345u;
	int near = t & 1;		/* half overlap: shifts under a tile */
	uint32_t sx = (seed >> 4) % (400 - w + 1), sy = (seed >> 12) % (rows - h + 1);
	seed = seed * 1103515245u + 12345u;
	int32_t ddx = near ? (int32_t) ((seed >> 5) % 81) - 40 : (int32_t) ((seed >> 5) % 400) - (int32_t) sx;
	int32_t ddy = near ? (int32_t) ((seed >> 15) % 81) - 40 : (int32_t) ((seed >> 15) % rows) - (int32_t) sy;
	int32_t dx = (int32_t) sx + ddx, dy = (int32_t) sy + ddy;
	if (dx < 0) dx = 0;
	if (dy < 0) dy = 0;
	if ((uint32_t) dx + w > 400) dx = (int32_t) (400 - w);
	if ((uint32_t) dy + h > rows) dy = (int32_t) (rows - h);
	for (uint32_t i = 0; i < sizeof fb2d; i++)
	  fb2d[i] = (uint8_t) (i * 7 + i / 255);
	memcpy (want2d, fb2d, sizeof fb2d);
	static uint8_t tmp[sizeof fb2d];
	for (uint32_t y = 0; y < h; y++)
	  memcpy (tmp + y * w * bpp, want2d + (sy + y) * stride + sx * bpp, w * bpp);
	for (uint32_t y = 0; y < h; y++)
	  memcpy (want2d + (dy + y) * stride + dx * bpp, tmp + y * w * bpp, w * bpp);
	int overlap = sx < (uint32_t) dx + w && (uint32_t) dx < sx + w && sy < (uint32_t) dy + h && (uint32_t) dy < sy + h;
	uint32_t order = overlap ? (((uint32_t) dx > sx ? V3D_2D_COLS_DESCENDING : 0)
				    | ((uint32_t) dy > sy ? V3D_2D_ROWS_DESCENDING : 0)) : 0;
	uint32_t base = 0x00400000;
	uint32_t len = v3d_2d_copy_rcl (cl, sizeof cl, 0, 0x1000, base + sy * stride + sx * bpp,
					base + (uint32_t) dy * stride + (uint32_t) dx * bpp, stride, w, h,
					bpp == 4 ? 27 : 7, order);
	runs++;
	if (!len || !exec_2d (cl, len, base) || memcmp (fb2d, want2d, sizeof fb2d))
	  {
	    if (bad++ < 5)
	      printf ("  2D copy %ux%u (%u,%u)->(%d,%d) %dbpp order %u wrong\n", w, h, sx, sy, dx, dy, bpp * 8, order);
	  }
      }
  CHECK (bad == 0);
  printf ("  2D copies carried out: %u, %u wrong\n", runs, bad);
  /* the order matters: scrolling down by 1 with the rows ascending is wrong */
  for (uint32_t i = 0; i < sizeof fb2d; i++)
    fb2d[i] = (uint8_t) i;
  uint32_t len = v3d_2d_copy_rcl (cl, sizeof cl, 0, 0x1000, 0x400000, 0x400000 + 1600, 1600,
				  100, 100, 27, 0);
  memcpy (want2d, fb2d, sizeof fb2d);
  memmove (want2d + 1600, want2d, 1600 * 100);
  CHECK (exec_2d (cl, len, 0x400000) && memcmp (fb2d, want2d, sizeof fb2d) != 0);
  /* and too small a buffer gives 0 */
  CHECK (v3d_2d_copy_rcl (cl, 100, 0, 0x1000, 0, 0, 1600, 400, 400, 27, 0) == 0);
}

/* (0.17) solid fills: the list carried out against a CPU fill, at both
   depths; every 16bpp colour survives the widening to RGBA8 and back */
static void
test_017_fill (void)
{
  static uint8_t cl[128 * 1024];
  uint32_t seed = 7, bad = 0, runs = 0;
  for (int bpp = 2; bpp <= 4; bpp += 2)
    for (int t = 0; t < 300; t++)
      {
	seed = seed * 1103515245u + 12345u;
	uint32_t stride = 400 * (uint32_t) bpp, rows = sizeof fb2d / stride;
	uint32_t w = 1 + (seed >> 8) % 400, h = 1 + (seed >> 3) % rows;
	seed = seed * 1103515245u + 12345u;
	uint32_t x = (seed >> 4) % (400 - w + 1), y = (seed >> 12) % (rows - h + 1);
	seed = seed * 1103515245u + 12345u;
	uint32_t pixel = bpp == 4 ? seed : (seed >> 9) & 0xFFFF;
	for (uint32_t i = 0; i < sizeof fb2d; i++)
	  fb2d[i] = (uint8_t) (i * 13 + 5);
	memcpy (want2d, fb2d, sizeof fb2d);
	for (uint32_t yy = 0; yy < h; yy++)
	  for (uint32_t xx = 0; xx < w; xx++)
	    memcpy (want2d + (y + yy) * stride + (x + xx) * bpp, &pixel, bpp);
	uint32_t base = 0x00400000;
	uint32_t len = v3d_2d_fill_rcl (cl, sizeof cl, 0x1000, base + y * stride + x * bpp,
					stride, w, h, bpp == 4 ? 27 : 7, v3d_2d_rgba (pixel, bpp));
	runs++;
	if (!len || !exec_2d (cl, len, base) || memcmp (fb2d, want2d, sizeof fb2d))
	  if (bad++ < 5)
	    printf ("  2D fill %ux%u at (%u,%u) %dbpp &%X wrong\n", w, h, x, y, bpp * 8, pixel);
      }
  CHECK (bad == 0);
  printf ("  2D fills carried out: %u, %u wrong\n", runs, bad);
  int exact = 1;
  for (uint32_t v = 0; v < 65536; v++)
    {
      uint32_t c = v3d_2d_rgba (v, 2);
      uint32_t r = (c & 255) * 31 + 127, g = ((c >> 8) & 255) * 63 + 127, b = ((c >> 16) & 255) * 31 + 127;
      if (((r / 255) << 11 | (g / 255) << 5 | b / 255) != v || c >> 24 != 255)
	exact = 0;
    }
  CHECK (exact);
  CHECK (v3d_2d_rgba (0x12345678, 4) == 0x12345678);
  /* a fill's list: one clearing tile, then four packets a tile */
  uint32_t len = v3d_2d_fill_rcl (cl, sizeof cl, 0x1000, 0x400000, 7680, 200, 100, 27, 0xFF00FF);
  CHECK (len == 52 + 24 + 2 * 4 * 20 + 1);
  /* (0.18) built from an odd address, byte by byte: the same list */
  static uint8_t odd[4096 + 1];
  CHECK (v3d_2d_fill_rcl (odd + 1, 4096, 0x1000, 0x400000, 7680, 200, 100, 27, 0xFF00FF) == len
	 && !memcmp (odd + 1, cl, len));
  uint32_t lc = v3d_2d_copy_rcl (cl, sizeof cl, 0, 0x1000, 0x400000, 0x500000, 7680, 300, 200, 27, 3);
  CHECK (lc == 52 + 5 * 4 * 32 + 1 && v3d_2d_copy_rcl (odd + 1, 4096, 0, 0x1000, 0x400000, 0x500000,
							7680, 300, 200, 27, 3) == lc
	 && !memcmp (odd + 1, cl, lc));
  CHECK (v3d_2d_copy_rcl (cl, lc - 1, 0, 0x1000, 0x400000, 0x500000, 7680, 300, 200, 27, 3) == 0
	 && v3d_2d_fill_rcl (cl, len - 1, 0x1000, 0x400000, 7680, 200, 100, 27, 0) == 0);
}

static void
test_v71 (void)
{
  setup (71);
  regs[V3D_BANK_SMS][V3D_SMS_TEE_CS / 4] = 0;	/* SMS idle */
  CHECK (v3d_hw_probe (dev) == 0 && dev->ver == 71);
  CHECK (v3d_init (dev) == 0);
  int f = v3d_open (dev, 1);
  uint32_t b = bo_new (f, 4096, 0), out = syncobj (f, 0);
  struct drm_v3d_submit_tfu t = { .icfg = 1, .bo_handles = { b }, .out_sync = out,
				  .v71 = { .ioc = 0x55 } };
  CHECK (ioctl_ (f, DRM_IOCTL_V3D_SUBMIT_TFU, &t) == 0);
  CHECK (regs[V3D_BANK_HUB][V3D_V7_TFU_IOC / 4] == 0x55);
  CHECK (regs[V3D_BANK_HUB][V3D_TFU_ICFG (71) / 4] == (1 | V3D_TFU_ICFG_IOC));
  sim_finish (V3D_TFU);
  struct drm_v3d_submit_csd c = { .cfg = { 1, 2, 3, 4, 5, 6, 7 } };
  CHECK (ioctl_ (f, DRM_IOCTL_V3D_SUBMIT_CSD, &c) == 0);
  CHECK (regs[V3D_BANK_CORE0][V3D_CSD_QUEUED_CFG0 (71) / 4] == 1);
  sim_finish (V3D_CSD);
  CHECK (syncobj_done (f, out));
  v3d_close (dev, f);
  teardown ();

  /* the wrong machine: an unpowered V3D reads 0xdeadbeef */
  setup (42);
  regs[V3D_BANK_HUB][V3D_HUB_IDENT1 / 4] = 0xdeadbeef;
  CHECK (v3d_hw_probe (dev) == -V3D_EIO);
  free (area);
  free (dev);
}

static void
test_many (void)
{
  /* more jobs than the job table holds, and many BOs: slots are reused */
  setup (42);
  CHECK (v3d_hw_probe (dev) == 0 && v3d_init (dev) == 0);
  int f = v3d_open (dev, 1);
  auto_finish = 1;
  uint32_t b = bo_new (f, 4096, 0);
  for (int i = 0; i < 3 * V3D_MAX_JOBS; i++)
    {
      struct drm_v3d_submit_cl s = { .bcl_start = 1, .bcl_end = 2, .rcl_start = 3, .rcl_end = 4,
				     .bo_handles = (uintptr_t) &b, .bo_handle_count = 1 };
      CHECK (ioctl_ (f, DRM_IOCTL_V3D_SUBMIT_CL, &s) == 0);
      if (i % 7 == 0)
	plat_wait ();
    }
  struct drm_v3d_wait_bo w = { .handle = b, .timeout_ns = ~0ull };
  CHECK (ioctl_ (f, DRM_IOCTL_V3D_WAIT_BO, &w) == 0);
  CHECK (dev->stats.jobs[V3D_RENDER] == 3 * V3D_MAX_JOBS);
  uint32_t hs[200];
  for (int i = 0; i < 200; i++)
    hs[i] = bo_new (f, 4096 * (1 + i % 5), 0);
  for (int i = 0; i < 200; i += 2)
    {
      struct drm_gem_close gc = { .handle = hs[i] };
      CHECK (ioctl_ (f, DRM_IOCTL_GEM_CLOSE, &gc) == 0);
    }
  for (int i = 0; i < 100; i++)
    bo_new (f, 4096 * (1 + i % 3), 0);
  auto_finish = 0;
  v3d_close (dev, f);
  teardown ();
}

/* (0.25) the page pool: freed pages stay claimed, in place, for the next
   BO of their kind; given back past pool_max, when the last program
   closes, and when a claim fails */
static int
gem_close (int f, uint32_t h)
{
  struct drm_gem_close gc = { .handle = h };
  return ioctl_ (f, DRM_IOCTL_GEM_CLOSE, &gc);
}

static int
pages_claimed (void)
{
  int n = 0;
  for (uint32_t p = 0; p < area_pages; p++)
    n += claimed[p];
  return n;
}

static void
test_025_pool (void)
{
  setup (42);
  CHECK (v3d_hw_probe (dev) == 0 && v3d_init (dev) == 0);
  CHECK (dev->pool_max == V3D_POOL_DEFAULT && dev->pool_pages == 0);
  int f = v3d_open (dev, 5);
  int base = pages_claimed ();
  uint32_t off, off2, h = bo_new (f, 5 * 4096, &off);
  memset (area + off, 0x55, 5 * 4096);
  int c0 = claims;
  CHECK (gem_close (f, h) == 0);
  CHECK (dev->pool_pages == 5 && pages_claimed () == base + 5);	/* kept */
  h = bo_new (f, 5 * 4096 - 100, &off2);
  CHECK (off2 == off && claims == c0);				/* no claim */
  CHECK (dev->stats.bo_from_pool == 1 && dev->stats.pages_reused == 5);
  CHECK (area[off] == 0 && area[off + 5 * 4096 - 1] == 0);	/* zeroed again */
  CHECK (dev->pool_pages == 0 && dev->pages_used >= 5);
  /* a smaller one in the pool's pages */
  CHECK (gem_close (f, h) == 0);
  h = bo_new (f, 2 * 4096, &off2);
  CHECK (claims == c0 && off2 == off && dev->pool_pages == 3);
  /* cached: the pooled pages aren't its kind */
  uint32_t claimed0 = dev->stats.pages_claimed;
  struct drm_v3d_create_bo cc = { .size = 3 * 4096, .flags = DRM_RISCOS_BO_CPU_CACHED };
  CHECK (ioctl_ (f, DRM_IOCTL_V3D_CREATE_BO, &cc) == 0);
  CHECK (dev->stats.pages_claimed == claimed0 + 3 && last_cached == 1);
  CHECK (dev->bos[dev->files[f].handles[cc.handle]].cached == 1);
  CHECK (gem_close (f, cc.handle) == 0);
  int p_before = (int) dev->pool_pages;
  struct drm_v3d_create_bo cc2 = { .size = 3 * 4096, .flags = DRM_RISCOS_BO_CPU_CACHED };
  CHECK (ioctl_ (f, DRM_IOCTL_V3D_CREATE_BO, &cc2) == 0);
  CHECK (cc2.offset == cc.offset && (int) dev->pool_pages == p_before - 3);
  CHECK (gem_close (f, cc2.handle) == 0);
  CHECK (gem_close (f, h) == 0);
  /* the pool's limit */
  dev->pool_max = 8;
  v3d_pool_trim (dev, dev->pool_max);
  CHECK (dev->pool_pages <= 8);
  uint32_t a = bo_new (f, 6 * 4096, 0), b = bo_new (f, 6 * 4096, 0);
  CHECK (gem_close (f, a) == 0 && gem_close (f, b) == 0);
  CHECK (dev->pool_pages <= 8);
  CHECK (pages_claimed () == base + (int) dev->pool_pages);
  /* a claim that fails: the pool goes back and it's tried again */
  dev->pool_max = V3D_POOL_DEFAULT;
  uint32_t big = bo_new (f, 40 * 4096, 0);
  CHECK (gem_close (f, big) == 0 && dev->pool_pages >= 40);
  claim_fails = 1;
  uint32_t big2 = bo_new (f, 300 * 4096, 0);	/* more than the pool */
  CHECK (big2 && claim_fails == 0);
  CHECK (pages_claimed () == base + 300 + (int) dev->pool_pages);
  claim_fails = 2;
  struct drm_v3d_create_bo nope = { .size = 64 * 4096 };
  CHECK (ioctl_ (f, DRM_IOCTL_V3D_CREATE_BO, &nope) == -V3D_ENOMEM);
  CHECK (dev->pool_pages == 0 && pages_claimed () == base + 300);
  claim_fails = 0;
  /* with only the desktop's 2D open, nothing is kept */
  CHECK (gem_close (f, big2) == 0 && dev->pool_pages == 300);
  int f2 = v3d_open (dev, 6);
  dev->twod.file = f2;
  v3d_close (dev, f);				/* the last program */
  CHECK (dev->pool_pages == 0 && pages_claimed () == base);
  uint32_t t = bo_new (f2, 4096, 0);
  CHECK (gem_close (f2, t) == 0 && dev->pool_pages == 0);
  dev->twod.file = -1;
  v3d_close (dev, f2);
  CHECK (dev->stats.bo_creates >= 10 && dev->stats.create_ns >= dev->stats.zero_ns);
  printf ("  pool: %u of %u BOs from the pool, %u pages reused, %u claimed\n",
	  dev->stats.bo_from_pool, dev->stats.bo_creates, dev->stats.pages_reused,
	  dev->stats.pages_claimed);
  teardown ();
}

/* (0.26) inside a GraphicsV call (no_page_ops) nothing claims or gives
   back RISC OS memory; it is done at the next v3d_service outside */
static void
test_026_no_page_ops (void)
{
  setup (42);
  CHECK (v3d_hw_probe (dev) == 0 && v3d_init (dev) == 0);
  int f = v3d_open (dev, 5);
  uint32_t a = bo_new (f, 4 * 4096, 0), b = bo_new (f, 6 * 4096, 0);
  CHECK (gem_close (f, a) == 0 && dev->pool_pages == 4);
  int base = pages_claimed ();
  dev->pool_max = 4;
  /* a close past the limit: kept, for later */
  dev->no_page_ops = 1;
  int c0 = claims, r0 = releases;
  CHECK (gem_close (f, b) == 0);
  CHECK (releases == r0 && dev->pool_pages == 10 && dev->page_work_due);
  CHECK (pages_claimed () == base);
  /* a BO that would need claims: refused, nothing claimed or given back */
  struct drm_v3d_create_bo big = { .size = 20 * 4096 };
  CHECK (ioctl_ (f, DRM_IOCTL_V3D_CREATE_BO, &big) == -V3D_ENOMEM);
  CHECK (claims == c0 && releases == r0 && dev->pool_pages == 10);
  /* one the kept pages hold: fine */
  uint32_t off;
  uint32_t k = bo_new (f, 3 * 4096, &off);
  CHECK (k && claims == c0 && releases == r0 && dev->pool_pages == 7);
  /* an import goes clear of the kept pages */
  static uint8_t mem[2 * 4096] __attribute__ ((aligned (4096)));
  struct drm_riscos_import_memory im = { .address = (uintptr_t) mem, .size = 2 * 4096 };
  CHECK (ioctl_ (f, DRM_IOCTL_RISCOS_IMPORT_MEMORY, &im) == 0);
  for (uint32_t p = im.offset >> 12; p < (im.offset >> 12) + 2; p++)
    CHECK (!((dev->page_backed[p >> 5] >> (p & 31)) & 1));
  CHECK (releases == r0 && dev->pool_pages == 7);
  /* the binner's memory isn't topped up in here */
  int ob = dev->overflow_pool[--dev->noverflow_pool];
  uint32_t n0 = dev->noverflow_pool;
  dev->page_work_due = 0;
  v3d_service (dev);
  CHECK (dev->noverflow_pool == n0 && claims == c0 && dev->page_work_due);
  /* trimming asked for in here: later too */
  v3d_pool_trim (dev, 0);
  CHECK (releases == r0 && dev->pool_pages == 7);
  /* outside: the work is done */
  dev->no_page_ops = 0;
  v3d_service (dev);
  CHECK (!dev->page_work_due && dev->pool_pages <= dev->pool_max);
  CHECK (dev->noverflow_pool > n0 && claims > c0);
  v3d_bo_put (dev, ob);			/* the one taken out above */
  CHECK (gem_close (f, k) == 0 && gem_close (f, im.handle) == 0);
  dev->pool_max = V3D_POOL_DEFAULT;
  /* the last program closing inside: its pages stay until outside */
  uint32_t c = bo_new (f, 5 * 4096, 0);
  CHECK (gem_close (f, c) == 0 && dev->pool_pages >= 5);
  dev->no_page_ops = 1;
  r0 = releases;
  v3d_close (dev, f);
  CHECK (releases == r0 && dev->pool_pages >= 5 && dev->page_work_due);
  dev->no_page_ops = 0;
  v3d_service (dev);
  CHECK (dev->pool_pages == 0 && !dev->page_work_due && releases > r0);
  /* GFXH-1897: the clean waits for a pending L2T flush first */
  l2t_pending = 5;
  int lg = logs;
  v3d_clean_caches (dev);
  CHECK (l2t_clobbers == 0 && l2t_pending == 0 && logs == lg);
  teardown ();
}

/* (0.30) BO and handle tables grow past their first 1024; refused
   CREATE_BOs are counted by reason */
static void
test_030_tables (void)
{
  setup (42);
  CHECK (v3d_hw_probe (dev) == 0 && v3d_init (dev) == 0);
  CHECK (dev->nbos == V3D_FIRST_BOS);
  int f = v3d_open (dev, 5);
  CHECK (dev->files[f].nhandles == V3D_FIRST_HANDLES);
  static uint32_t h[1500], off[1500];
  int ok = 1;
  for (int i = 0; i < 1500 && ok; i++)
    {
      h[i] = bo_new (f, 4096, &off[i]);
      ok = h[i] != 0;
      if (ok)
	area[off[i]] = (uint8_t) i;
    }
  CHECK (ok && dev->nbos == 2 * V3D_FIRST_BOS && dev->files[f].nhandles == 2 * V3D_FIRST_HANDLES);
  ok = 1;
  for (int i = 0; i < 1500; i++)		/* every BO still where it was */
    {
      struct drm_v3d_get_bo_offset g = { .handle = h[i] };
      ok &= ioctl_ (f, DRM_IOCTL_V3D_GET_BO_OFFSET, &g) == 0 && g.offset == off[i]
	&& area[off[i]] == (uint8_t) i;
    }
  CHECK (ok);
  CHECK (dev->stats.bo_refused == 0);
  /* too big for the area */
  struct drm_v3d_create_bo big = { .size = (uint32_t) dev->area_pages * 4096 };
  CHECK (ioctl_ (f, DRM_IOCTL_V3D_CREATE_BO, &big) == -V3D_ENOMEM);
  CHECK (dev->stats.refused_space == 1 && dev->stats.refused_last_bytes == big.size);
  /* RISC OS gives nothing */
  claim_fails = 2;
  struct drm_v3d_create_bo m = { .size = 64 * 4096 };
  CHECK (ioctl_ (f, DRM_IOCTL_V3D_CREATE_BO, &m) == -V3D_ENOMEM);
  claim_fails = 0;
  CHECK (dev->stats.refused_memory == 1 && dev->stats.bo_refused == 2);
  /* no handle: a file whose table can't grow */
  uint32_t keep = dev->files[f].nhandles;
  for (uint32_t i = 1; i < keep; i++)
    if (!dev->files[f].handles[i])
      dev->files[f].handles[i] = 1;		/* (taken, for this test) */
  dev->files[f].nhandles = V3D_MAX_HANDLES;	/* pretend it's at the limit */
  uint16_t *real = dev->files[f].handles;
  dev->files[f].handles = calloc (V3D_MAX_HANDLES, 2);
  for (uint32_t i = 1; i < V3D_MAX_HANDLES; i++)
    dev->files[f].handles[i] = 1;
  struct drm_v3d_create_bo nh = { .size = 4096 };
  CHECK (ioctl_ (f, DRM_IOCTL_V3D_CREATE_BO, &nh) == -V3D_ENOSPC);
  CHECK (dev->stats.refused_handles == 1 && dev->stats.bo_refused == 3);
  free (dev->files[f].handles);
  dev->files[f].handles = real;
  dev->files[f].nhandles = keep;
  for (uint32_t i = 1; i < keep; i++)
    {
      int used = 0;
      for (int j = 0; j < 1500; j++)
	used |= h[j] == i;
      if (!used)
	dev->files[f].handles[i] = 0;
    }
  for (int i = 0; i < 1500; i++)
    CHECK (gem_close (f, h[i]) == 0);
  v3d_close (dev, f);
  teardown ();
}

/* (0.31) DRM_IOCTL_RISCOS_PRESENT. The render lists are carried out as
   the GPU would as each job starts (loads and stores with their formats,
   memory layouts, R/B swaps and alpha), into a stand-in screen, and the
   screen compared with one drawn here pixel by pixel from the source. */
static uint8_t pscr[4 * 1024 * 1024], pwant[sizeof pscr];
static uint32_t pscr_gpu, pscr_bytes, pjobs, pbad_list;
static struct v3d_screen psc;
static uint32_t pres[4];
static const char *pdump;		/* the next list, to this file (cl2d.py checks it) */
static int psc_ret;

static int
fake_screen (struct v3d_dev *v3d, struct v3d_screen *s)
{
  (void) v3d;
  *s = psc;
  return psc_ret;
}

static uint8_t *
gpu_ptr (uint32_t a, uint32_t n)
{
  if (a >= pscr_gpu && a - pscr_gpu + (uint64_t) n <= pscr_bytes)
    return pscr + (a - pscr_gpu);
  if ((uint64_t) a + n <= (uint64_t) area_pages * 4096)
    return area + a;
  return 0;
}

static uint32_t
x5 (uint32_t v) { return (v * 527u + 23u) >> 6; }
static uint32_t
x6 (uint32_t v) { return (v * 259u + 33u) >> 6; }

/* a pixel in memory (format fmt) to R, G, B, A and back */
static void
unpack (uint32_t fmt, const uint8_t *m, uint8_t *c)
{
  if (fmt == 27)
    memcpy (c, m, 4);
  else
    {
      uint32_t v = m[0] | m[1] << 8;
      if (fmt == 7)
	c[0] = (uint8_t) x5 (v >> 11), c[1] = (uint8_t) x6 ((v >> 5) & 63), c[2] = (uint8_t) x5 (v & 31), c[3] = 255;
      else
	c[0] = (uint8_t) x5 (v & 31), c[1] = (uint8_t) x5 ((v >> 5) & 31), c[2] = (uint8_t) x5 ((v >> 10) & 31),
	  c[3] = v >> 15 ? 255 : 0;
    }
}

static uint32_t n5 (uint32_t v) { return (v * 31 + 127) / 255; }
static uint32_t n6 (uint32_t v) { return (v * 63 + 127) / 255; }

static void
pack (uint32_t fmt, const uint8_t *c, uint8_t *m)
{
  if (fmt == 27)
    memcpy (m, c, 4);
  else
    {
      uint32_t v = fmt == 7 ? n5 (c[0]) << 11 | n6 (c[1]) << 5 | n5 (c[2])
	: n5 (c[0]) | n5 (c[1]) << 5 | n5 (c[2]) << 10 | (c[3] >= 128) << 15;
      m[0] = (uint8_t) v, m[1] = (uint8_t) (v >> 8);
    }
}

static void
swap_rb (uint8_t *c)
{
  uint8_t t = c[0];
  c[0] = c[2];
  c[2] = t;
}

static void
exec_present (uint32_t start, uint32_t end)
{
  uint8_t *cl = gpu_ptr (start, end - start);
  uint32_t w = 0, h = 0, col = 0, row = 0;
  static uint8_t tile[64 * 64 * 4];
  pjobs++;
  if (!cl)
    {
      pbad_list++;
      return;
    }
  if (pdump)
    {
      FILE *o = fopen (pdump, "wb");
      if (o)
	{
	  fwrite (cl, 1, end - start, o);
	  fclose (o);
	}
      pdump = 0;
    }
  for (uint32_t i = 0; i < end - start;)
    {
      uint8_t op = cl[i];
      uint64_t g;
      uint32_t addr;
      switch (op)
	{
	case 121:
	  if ((cl[i + 1] & 15) == 0)
	    w = cl[i + 2] | cl[i + 3] << 8, h = cl[i + 4] | cl[i + 5] << 8;
	  i += 9;
	  break;
	case 122: i += 9; break;
	case 123: i += 5; break;
	case 126: i += 2; break;
	case 124:
	  col = (cl[i + 1] | cl[i + 2] << 8 | cl[i + 3] << 16) & 0xFFF;
	  row = (cl[i + 1] | cl[i + 2] << 8 | cl[i + 3] << 16) >> 12;
	  i += 4;
	  break;
	case 30:
	case 29:
	  {
	    memcpy (&g, cl + i + 1, 8);
	    memcpy (&addr, cl + i + 9, 4);
	    uint32_t fmt = (g >> 12) & 63, mem = (g >> 4) & 7, field = (g >> 28) & 0xFFFFF;
	    uint32_t bpp = fmt == 27 ? 4 : 2;
	    /* (0.32) as the GPU: no R/B swap of a 3-channel format (BGR565) */
	    int swap = ((g >> 20) & 1) && fmt != 7, alpha1 = op == 30 && ((g >> 18) & 1);
	    if ((g & 0xF) || (fmt != 27 && fmt != 7 && fmt != 45) || (mem != 0 && mem != 4 && mem != 5)
		|| (mem && (op == 29 || bpp != 4)) || (g >> 48))
	      {
		pbad_list++;
		return;
	      }
	    for (uint32_t y = 0; y < 64 && row * 64 + y < h; y++)
	      for (uint32_t x = 0; x < 64 && col * 64 + x < w; x++)
		{
		  uint32_t px = col * 64 + x, py = row * 64 + y;
		  uint32_t off = mem ? uif_pixel_offset (field * 8, px, py, mem == 5)
		    : py * field + px * bpp;
		  uint8_t *m = gpu_ptr (addr + off, bpp), *c = tile + (y * 64 + x) * 4;
		  if (!m)
		    {
		      pbad_list++;
		      return;
		    }
		  if (op == 30)
		    {
		      unpack (fmt, m, c);
		      if (swap)
			swap_rb (c);
		      if (alpha1)
			c[3] = 255;
		    }
		  else
		    {
		      uint8_t t[4];
		      memcpy (t, c, 4);
		      if (swap)
			swap_rb (t);
		      pack (fmt, t, m);
		    }
		}
	    i += 13;
	    break;
	  }
	case 26: case 27: case 1: i += 1; break;
	case 13:
	  if (i + 1 != end - start)
	    pbad_list++;
	  return;
	default:
	  pbad_list++;
	  return;
	}
    }
  pbad_list++;
}

/* the screen as it should be: each rectangle clipped, each pixel taken
   from the source (raster or UIF, its format) and put in the screen's */
static void
want_rects (const struct drm_riscos_present *a, const uint8_t *src,
	    const struct drm_riscos_present_rect *r, uint32_t n)
{
  uint32_t sbpp = a->format == DRM_FORMAT_RGB565 ? 2 : 4;
  int sswap = a->format == DRM_FORMAT_ARGB8888 || a->format == DRM_FORMAT_XRGB8888;
  uint32_t dfmt = psc.kind == V3D_SCREEN_RGBA8 ? 27 : psc.kind == V3D_SCREEN_565 ? 7 : 45;
  uint32_t dbpp = dfmt == 27 ? 4 : 2;
  for (uint32_t k = 0; k < n; k++)
    for (int32_t y = 0; y < r[k].height; y++)
      for (int32_t x = 0; x < r[k].width; x++)
	{
	  int64_t dx = (int64_t) r[k].dst_x + x, dy = (int64_t) r[k].dst_y + y;
	  int64_t sx = (int64_t) r[k].src_x + x, sy = (int64_t) r[k].src_y + y;
	  if (dx < 0 || dy < 0 || dx >= psc.width || dy >= psc.height
	      || sx < 0 || sy < 0 || sx >= a->width || sy >= a->height)
	    continue;
	  uint32_t so = (a->flags & DRM_RISCOS_PRESENT_SRC_UIF)
	    ? uif_pixel_offset (a->uif_height_or_stride, (uint32_t) sx, (uint32_t) sy, 0)
	    : (uint32_t) sy * a->uif_height_or_stride + (uint32_t) sx * sbpp;
	  uint8_t c[4];
	  unpack (sbpp == 4 ? 27 : 7, src + so, c);
	  if (sswap)
	    swap_rb (c);
	  c[3] = 255;
	  if (psc.rb_swap)
	    swap_rb (c);
	  pack (dfmt, c, pwant + psc.off + (uint32_t) dy * psc.stride + (uint32_t) dx * dbpp);
	}
}

static int
scr_same (void)
{
  if (pbad_list)
    printf ("  bad lists: %u\n", pbad_list);
  for (uint32_t i = 0; i < sizeof pscr; i++)
    if (pscr[i] != pwant[i])
      {
	printf ("  screen differs at %u (row %u, byte %u): %02x, want %02x\n", i,
		(i - psc.off) / psc.stride, (i - psc.off) % psc.stride, pscr[i], pwant[i]);
	return 0;
      }
  return !pbad_list;
}

static int
present_ (int f, struct drm_riscos_present *a)
{
  int ret = ioctl_ (f, DRM_IOCTL_RISCOS_PRESENT, a);
  if (auto_finish)
    while (!v3d_idle (dev))
      {
	plat_wait ();
	v3d_service (dev);
      }
  return ret;
}

static void
test_031_present (void)
{
  setup (42);
  CHECK (v3d_hw_probe (dev) == 0 && v3d_init (dev) == 0);
  CHECK (dev->present.file == -1);
  int f = v3d_open (dev, 1);
  uint32_t soff;
  uint32_t src = bo_new (f, 512 * 1024, &soff);
  uint8_t *sp = area + soff;
  for (uint32_t i = 0; i < 512 * 1024; i++)
    sp[i] = (uint8_t) (i * 13 + (i >> 9) * 7 + (i >> 17));
  struct drm_riscos_present_rect rr[5] = {
    { 100, 50, 150, 100, 10, 20 },		/* inside */
    { 600, 450, 100, 100, 0, 0 },		/* over the screen's corner */
    { -20, -10, 90, 70, 5, 5 },			/* over its top left */
    { 700, 10, 30, 30, 0, 0 },			/* off the screen */
    { 300, 300, 80, 80, 260, 150 },		/* over the source's corner */
  };
  struct drm_riscos_present a = {
    .handle = src, .format = DRM_FORMAT_ABGR8888, .width = 300, .height = 200,
    .uif_height_or_stride = 1216, .rect_count = 5, .rects = (uintptr_t) rr,
  };
  /* not offered (no screen hook): -ENOTTY, EGL falls back */
  CHECK (present_ (f, &a) == -V3D_ENOTTY && dev->present.file == -1);
  dev->present_screen = fake_screen;
  /* a 640x480 32bpp screen, stride 2600, the VDU writing to its second bank */
  psc = (struct v3d_screen) { 0x3E010000ull, sizeof pscr, 2600 * 480, 2600, 640, 480,
			      V3D_SCREEN_RGBA8, 0, 0 };
  psc_ret = 0;
  present_exec = exec_present;
  auto_finish = 1;
  for (uint32_t i = 0; i < sizeof pscr; i++)
    pscr[i] = (uint8_t) (i * 5 + 3);
  memcpy (pwant, pscr, sizeof pscr);
  uint32_t out = syncobj (f, 0);
  a.out_sync = out;
  auto_finish = 0;
  pjobs = 0;
  CHECK (present_ (f, &a) == 0 && dev->present.file >= 0 && pjobs == 1);
  pscr_gpu = dev->present.scr_gpu;		/* known once mapped */
  pscr_bytes = sizeof pscr;
  CHECK (!syncobj_done (f, out));		/* not until the jobs finish */
  CHECK (dev->present.presents == 1 && dev->present.rects == 4 && dev->present.clipped_away == 1);
  /* the first job ran before pscr_gpu was known: do it all again */
  while (busy[V3D_RENDER])
    sim_finish (V3D_RENDER);
  v3d_service (dev);
  CHECK (syncobj_done (f, out));
  memcpy (pscr, pwant, sizeof pscr);
  pjobs = 0;
  pbad_list = 0;
  auto_finish = 1;
  CHECK (present_ (f, &a) == 0 && pjobs == 4);
  want_rects (&a, sp, rr, 5);
  CHECK (scr_same ());
  /* ARGB onto a TRGB screen (two swaps), 16bpp 5:6:5 TBGR and 1:5:5:5 */
  static const struct { uint32_t fourcc, kind, swap, stride; } cases[] = {
    { DRM_FORMAT_ARGB8888, V3D_SCREEN_RGBA8, 1, 2600 },
    { DRM_FORMAT_XBGR8888, V3D_SCREEN_RGBA8, 1, 2560 },
    { DRM_FORMAT_RGB565, V3D_SCREEN_565, 1, 1280 },	/* (0.32) through the staging buffer */
    { DRM_FORMAT_ABGR8888, V3D_SCREEN_565, 1, 1280 },	/* (0.32) MESA's C64K: blue at the top */
    { DRM_FORMAT_RGB565, V3D_SCREEN_565, 0, 1300 },
    { DRM_FORMAT_ABGR8888, V3D_SCREEN_5551, 0, 1280 },
    { DRM_FORMAT_ARGB8888, V3D_SCREEN_5551, 1, 1280 },
  };
  for (unsigned k = 0; k < sizeof cases / sizeof cases[0]; k++)
    {
      psc.kind = cases[k].kind;
      psc.rb_swap = cases[k].swap;
      psc.stride = cases[k].stride;
      psc.off = k * 4096 + (k & 1) * 2;
      a.format = cases[k].fourcc;
      a.uif_height_or_stride = a.format == DRM_FORMAT_RGB565 ? 640 : 1216;
      pbad_list = 0;
      if (k == 6)
	pdump = "out/cl2d/present-5551.bin";
      CHECK (present_ (f, &a) == 0);
      if (k == 6)
	{
	  FILE *o = fopen ("out/cl2d/present", "w");
	  if (o)
	    {
	      fprintf (o, "5551 %u %u %u %u\n", soff + 20 * 1216 + 10 * 4,
		       pscr_gpu + psc.off + 50 * psc.stride + 100 * 2, 150, 100);
	      fclose (o);
	    }
	}
      want_rects (&a, sp, rr, 5);
      CHECK (scr_same ());
    }
  /* (0.32) MESA's case exactly: blue (ABGR 00 00 FF FF) onto 5:6:5 TBGR
     is &F800 (blue at the top), not &001F */
  {
    psc.kind = V3D_SCREEN_565, psc.rb_swap = 1, psc.stride = 1280, psc.off = 0;
    struct drm_riscos_present b = a;
    b.format = DRM_FORMAT_ABGR8888, b.uif_height_or_stride = 1216;
    struct drm_riscos_present_rect br[1] = { { 0, 0, 1, 1, 0, 0 } };
    b.rects = (uintptr_t) br, b.rect_count = 1;
    uint8_t keep[4];
    memcpy (keep, sp, 4);
    sp[0] = 0, sp[1] = 0, sp[2] = 0xFF, sp[3] = 0xFF;
    CHECK (present_ (f, &b) == 0 && (pscr[0] | pscr[1] << 8) == 0xF800);
    memcpy (sp, keep, 4);
    memcpy (pwant, pscr, sizeof pscr);
  }
  /* a UIF source (100x70, padded to 72 rows), untiled first */
  psc.kind = V3D_SCREEN_RGBA8, psc.rb_swap = 0, psc.stride = 2600, psc.off = 0;
  struct drm_riscos_present u = a;
  u.format = DRM_FORMAT_ABGR8888, u.flags = DRM_RISCOS_PRESENT_SRC_UIF;
  u.width = 100, u.height = 70, u.uif_height_or_stride = 72;
  struct drm_riscos_present_rect ur[2] = { { 30, 40, 100, 70, 0, 0 }, { 400, 300, 50, 30, 33, 21 } };
  u.rects = (uintptr_t) ur, u.rect_count = 2;
  pjobs = 0;
  pbad_list = 0;
  pdump = "out/cl2d/present-uif.bin";
  uint32_t untiled = dev->present.untiled;
  CHECK (present_ (f, &u) == 0 && pjobs == 3 && dev->present.untiled == untiled + 1 && dev->present.stage_handle);
  {
    FILE *o = fopen ("out/cl2d/present", "a");
    if (o)
      {
	fprintf (o, "uif %u %u %u %u\n", soff, dev->present.stage_gpu, 100, 70);
	fclose (o);
      }
  }
  want_rects (&u, sp, ur, 2);
  CHECK (scr_same ());
  u.uif_height_or_stride = 70;			/* not whole blocks */
  CHECK (present_ (f, &u) == -V3D_EINVAL);
  u.uif_height_or_stride = 64;			/* shorter than the image */
  CHECK (present_ (f, &u) == -V3D_EINVAL);
  /* checks without drawing, and refusals */
  struct drm_riscos_present q = { .handle = 0, .rect_count = 0 };
  uint32_t probes = dev->present.probes;
  pjobs = 0;
  CHECK (present_ (f, &q) == 0 && dev->present.probes == probes + 1 && pjobs == 0);
  q = a;
  q.rect_count = 0;
  CHECK (present_ (f, &q) == 0 && pjobs == 0);
  q.handle = 999;
  CHECK (present_ (f, &q) == -V3D_ENOENT);
  q = a;
  q.flags = DRM_RISCOS_PRESENT_VSYNC;
  CHECK (present_ (f, &q) == -V3D_EOPNOTSUPP);
  q.flags = 0x80;
  CHECK (present_ (f, &q) == -V3D_EINVAL);
  q = a;
  q.format = 0x34325258u + 1;
  CHECK (present_ (f, &q) == -V3D_EINVAL);
  q = a;
  q.uif_height_or_stride = 1000;		/* under width x 4 */
  CHECK (present_ (f, &q) == -V3D_EINVAL);
  q.height = 600, q.uif_height_or_stride = 1216;	/* past the BO */
  CHECK (present_ (f, &q) == -V3D_EINVAL);
  q = a;
  q.rect_count = DRM_RISCOS_PRESENT_MAX_RECTS + 1;
  CHECK (present_ (f, &q) == -V3D_EINVAL);
  psc.kind = 9;					/* a screen it doesn't take */
  CHECK (present_ (f, &a) == -V3D_EINVAL);
  psc.kind = V3D_SCREEN_RGBA8;
  psc.cached = 1;
  CHECK (present_ (f, &a) == -V3D_EBUSY);
  psc.cached = 0;
  psc_ret = -V3D_EINVAL;
  CHECK (present_ (f, &a) == -V3D_EINVAL);
  psc_ret = 0;
  CHECK (dev->present.refused >= 11);
  /* everything off the screen: nothing drawn, out_sync signalled at once */
  struct drm_riscos_present_rect off[1] = { { 2000, 0, 10, 10, 0, 0 } };
  q = a;
  q.rects = (uintptr_t) off, q.rect_count = 1;
  uint32_t out2 = syncobj (f, 0);
  q.out_sync = out2;
  pjobs = 0;
  CHECK (present_ (f, &q) == 0 && pjobs == 0 && syncobj_done (f, out2));
  /* the program's frame first: its job on the source, then the present
     after it (implicit, by the source BO), and after in_sync */
  auto_finish = 0;
  struct drm_v3d_submit_cl k = { .rcl_start = 0x10, .rcl_end = 0x20,
				 .bo_handles = (uintptr_t) &src, .bo_handle_count = 1 };
  present_exec = 0;
  CHECK (ioctl_ (f, DRM_IOCTL_V3D_SUBMIT_CL, &k) == 0 && busy[V3D_RENDER]);
  uint32_t in = syncobj (f, 0);
  struct drm_v3d_submit_tfu tfu = { 0 };
  tfu.out_sync = in;
  tfu.ios = 1;
  tfu.iia = 0x1000;
  tfu.ioa = 0x2000;
  CHECK (ioctl_ (f, DRM_IOCTL_V3D_SUBMIT_TFU, &tfu) == 0);
  q = a;
  q.in_sync = in;
  q.out_sync = out;
  uint32_t emitted = dev->emitted[V3D_RENDER];
  CHECK (present_ (f, &q) == 0 && dev->emitted[V3D_RENDER] == emitted + 4);
  struct v3d_job *j = dev->head[V3D_RENDER];
  CHECK (j && j->dep[V3D_RENDER] == emitted && j->dep[V3D_TFU] != 0 && !syncobj_done (f, out));
  auto_finish = 1;
  while (!syncobj_done (f, out))
    {
      plat_wait ();
      v3d_service (dev);
    }
  /* many presents: the list buffer's parts taken in turn, each waited for */
  present_exec = exec_present;
  uint32_t slot0 = dev->present.slot;
  for (int i = 0; i < 40; i++)
    CHECK (present_ (f, &a) == 0);
  CHECK (dev->present.slot != slot0 || dev->present.jobs > 160);
  /* the list split into bands: only the asked rows, and nothing past the room */
  {
    static uint8_t cl[8192];
    uint32_t len = v3d_present_rcl (cl, sizeof cl, 0x1000, 0x2000, 0, 0x3000, 0, 640, 480, 2, 3);
    CHECK (len == 52 + 10 * 3 * 32 + 1 && cl[52] == 124
	   && (cl[53] | cl[54] << 8 | cl[55] << 16) == 2u << 12);
    CHECK (v3d_present_rcl (cl, 52 + 10 * 3 * 32, 0x1000, 0x2000, 0, 0x3000, 0, 640, 480, 2, 3) == 0);
    CHECK (v3d_present_rcl (cl, sizeof cl, 0x1000, 0x2000, 0, 0x3000, 0, 640, 480, 7, 2) == 0);
  }
  /* a mode change: the new framestore mapped, the old let go */
  int sbo = dev->files[dev->present.file].handles[dev->present.scr_handle];
  psc.fs_pa = 0x3E810000ull;
  CHECK (present_ (f, &a) == 0 && !dev->bos[sbo].used
	 && dev->files[dev->present.file].handles[dev->present.scr_handle] != sbo);
  pres[0] = dev->present.presents, pres[1] = dev->present.rects;
  pres[2] = dev->present.jobs, pres[3] = dev->present.refused;
  /* the last program closed: PRESENT's file goes too */
  present_exec = 0;
  v3d_close (dev, f);
  CHECK (dev->present.file == -1 && !v3d_programs_open (dev));
  for (int i = 0; i < V3D_MAX_FILES; i++)
    CHECK (!dev->files[i].used);
  auto_finish = 0;
  teardown ();
}

int
main (void)
{
  test_probe_and_bo ();
  test_cl_jobs ();
  test_old_struct_csd_reset ();
  test_v71 ();
  test_013 ();
  test_015 ();
  test_017 ();
  test_017_exec ();
  test_017_fill ();
  test_many ();
  test_025_pool ();
  test_026_no_page_ops ();
  test_030_tables ();
  test_031_present ();
  printf ("  present: %u presents, %u rectangles, %u jobs, %u refused\n", pres[0], pres[1], pres[2], pres[3]);
  printf ("%d checks, %d failed\n", checks, failures);
  return failures != 0;
}
