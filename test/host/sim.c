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
void plat_cache_clean (const void *p, uint32_t n) { (void) p; (void) n; cache_cleans++; }
void plat_cache_inval (const void *p, uint32_t n) { (void) p; (void) n; cache_invals++; }

int
plat_pages_claim (struct v3d_dev *v3d, uint32_t first, uint32_t n, int cached)
{
  last_cached = (unsigned) cached;
  (void) v3d;
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

int
main (void)
{
  test_probe_and_bo ();
  test_cl_jobs ();
  test_old_struct_csd_reset ();
  test_v71 ();
  test_013 ();
  test_015 ();
  test_many ();
  printf ("%d checks, %d failed\n", checks, failures);
  return failures != 0;
}
