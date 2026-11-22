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

/* ---- the simulated machine ---- */

static uint32_t regs[V3D_NBANKS][0x4000 / 4];
static int busy[V3D_HW_QUEUES];
static uint32_t now_cs = 1000;
static int resets, auto_finish, logs;
static uint8_t *area;
static uint32_t area_pages = 4096;	/* 16 MB */
static uint8_t claimed[4096];
static int claim_fails, claims;	/* (0.25) */
static uint32_t pt[1 << 20];

uint32_t
v3d_rd (struct v3d_dev *v3d, int bank, uint32_t off)
{
  (void) v3d;
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
    start (V3D_RENDER);
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
    val &= ~(V3D_L2TCACTL_TMUWCF | V3D_L2TCACTL_L2TFLS);
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
  printf ("%d checks, %d failed\n", checks, failures);
  return failures != 0;
}
