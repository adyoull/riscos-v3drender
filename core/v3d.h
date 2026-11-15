/* v3d.h: the V3D driver's core, shared by the RISC OS module and the
   host test.

   The core follows Linux's v3d driver (drivers/gpu/drm/v3d): the same
   hardware sequences (power-up checks, MMU, caches, interrupts, job
   start, reset), the same request interface (v3d_drm.h, unchanged) and
   the same rules for when a job may run (one job at a time on each of
   the bin, render, TFU and CSD queues; a render job after its bin job; a
   job after the earlier jobs on the buffers it uses; the sync objects it
   was given). What Linux gets from its kernel (the GPU scheduler, fences,
   GEM, shmem pages) is written here in a small fixed-size form, for a
   machine with one CPU core and no threads:

   - Fences are (queue, number): each queue numbers its jobs from 1 and
     records the last one finished. A fence is done when that count has
     reached it.
   - Jobs are started from the interrupt handler as soon as the last one
     on the queue finishes and the new one's fences are done, and from
     the SWI that submits them.
   - Buffers (BOs) are pages in one memory area that the CPU and the GPU
     see at the same offset: a BO's GPU address is its offset in the area.
     Freeing waits for the GPU (a BO is held by the jobs that use it).
   - Work that can't be done with interrupts off (freeing memory,
     replacing binner overflow memory, resetting a hung GPU) is done at
     the next call into the driver ("v3d_service").

   Hardware access goes through the platform's v3d_rd/v3d_wr (v3d_plat.h).

   SPDX-License-Identifier: GPL-2.0-or-later
   Copyright (C) 2014-2018 Broadcom (the Linux driver this follows)
   Copyright (c) 2026 Andrew Youll */
#ifndef V3D_H
#define V3D_H

#include <stdint.h>
#include <stddef.h>
#include "drm/drm.h"
#include "drm/v3d_drm.h"

void *memset (void *d, int c, size_t n);
void *memcpy (void *d, const void *s, size_t n);

/* queues: v3d_drm.h's enum v3d_queue (V3D_BIN .. V3D_CACHE_CLEAN; the
   CPU queue isn't supported yet) */
#define V3D_NQ (V3D_CACHE_CLEAN + 1)
#define V3D_HW_QUEUES 4			/* BIN..CSD run on the GPU */

enum v3d_bank { V3D_BANK_HUB, V3D_BANK_CORE0, V3D_BANK_SMS, V3D_BANK_BRIDGE, V3D_NBANKS };

#define V3D_GEN_33 33
#define V3D_GEN_41 41
#define V3D_GEN_42 42
#define V3D_GEN_71 71

#define V3D_PAGE 4096u
#define V3D_MMU_PAGE_SHIFT 12

/* Sizes (fixed tables: nothing is allocated with interrupts off) */
#define V3D_MAX_FILES 32
#define V3D_MAX_HANDLES 4096		/* per file */
#define V3D_MAX_SYNCOBJS 256		/* per file */
#define V3D_MAX_BOS 8192
#define V3D_MAX_JOBS 256
#define V3D_MAX_JOB_BOS 4096
#define V3D_MAX_SYNC_FILES 256
#define V3D_OVERFLOW_BYTES (256u * 1024)	/* as Linux */
#define V3D_OVERFLOW_POOL 16
#define V3D_JOB_OVERFLOWS 8
#define V3D_TIMEOUT_CS 50		/* as Linux's 500 ms */

struct v3d_fence { uint32_t q, seq; };	/* seq 0: no wait */

struct v3d_job
{
  struct v3d_job *next;
  uint8_t q, failed, started, skip;	/* skip: its bin job failed */
  uint8_t guilty;			/* timed out: the reset is its doing */
  uint32_t seq;
  uint32_t dep[V3D_NQ];			/* fences that must be done first */
  uint32_t start_cs, timedout_ctca, timedout_ctra;
  int file;
  union
  {
    struct { uint32_t start, end, qma, qms, qts, bstart, bend; struct v3d_job *render; } cl;
    struct drm_v3d_submit_tfu tfu;
    struct drm_v3d_submit_csd csd;
  } a;
  uint16_t *bos;			/* BOs held (references) */
  uint32_t nbos;
  uint16_t overflow[V3D_JOB_OVERFLOWS];	/* binner overflow BOs (render jobs) */
  uint32_t noverflow;
};

struct v3d_bo
{
  uint32_t first, npages;		/* pages in the area; GPU address = first * 4096 */
  uint32_t refs;			/* handles + jobs */
  uint32_t last[V3D_NQ];		/* the last job on each queue to use it */
  uint8_t used, overflow;
};

struct v3d_syncobj { uint8_t used, has_fence; struct v3d_fence f; };

struct v3d_file
{
  int used;
  uint32_t owner;			/* the platform's: a Wimp task, say */
  uint32_t resets;			/* resets its jobs caused */
  uint16_t *handles;			/* handle (from 1) -> BO */
  struct v3d_syncobj *syncobjs;		/* handle (from 1) */
};

/* (V3DRender_Info r8 points at this: its layout is part of the
   interface for tests; add to the end only) */
/* What the GPU was doing when a job was found hung (0.05), read just
   before the reset: which queue, the job's list, the control list
   executors' registers, the error and debug registers, and the list
   words around the hung queue's current address (0 where no buffer). */
struct v3d_hang
{
  uint32_t q, start, end;		/* q: V3D_BIN.. (~0: none yet) */
  uint32_t ct0cs, ct0ca, ct0ea, ct0ra, ct1cs, ct1ca, ct1ea, ct1ra;
  uint32_t pcs, bfc, rfc, err_stat, fdbgo, fdbgb, fdbgr, fdbgs;
  uint32_t bpca, bpcs, bpoa, bpos;	/* the binner's memory */
  uint32_t words_at;			/* GPU address of words[0] */
  uint32_t words[8];			/* CA - 16 .. CA + 16 */
};

/* V3DRender_Info r8 points here: add to the end only */
struct v3d_stats
{
  uint32_t jobs[V3D_NQ], irqs, irqs_empty, overflows, overflow_misses;
  uint32_t mmu_errors, mmu_last_addr, mmu_last_client, mmu_last_sts;
  uint32_t resets, timeouts, gmp_violations;
  struct v3d_hang first_hang, last_hang;	/* 0.05 */
};

struct v3d_dev
{
  /* set by the platform before v3d_init */
  void *plat;
  int ver;				/* from the platform's match (42, 71) */
  uint32_t *pt;				/* 4 MB page table, CPU address */
  uint64_t pt_pa;
  uint64_t scratch_pa;			/* a page for illegal addresses */
  uint32_t area_pages;			/* BO pages the area can hold */
  uint8_t *area;			/* CPU address of page 0 */

  /* from the hardware */
  int rev, cores, va_width, pa_width;
  uint32_t ident[4];

  /* queues and jobs */
  struct v3d_job *active[V3D_HW_QUEUES];
  struct v3d_job *head[V3D_NQ], *tail[V3D_NQ];	/* waiting, oldest first */
  volatile uint32_t completed[V3D_NQ];
  uint32_t emitted[V3D_NQ];
  struct v3d_job jobs[V3D_MAX_JOBS];
  struct v3d_job *free_jobs;
  struct v3d_job *volatile retired;	/* finished, BOs not yet released */
  volatile int reset_needed;
  uint32_t last_check_cs;

  /* buffers */
  struct v3d_bo bos[V3D_MAX_BOS];	/* [0] unused */
  uint32_t *page_map;			/* a bit per page: in use */
  uint32_t pages_used;
  uint16_t overflow_pool[V3D_OVERFLOW_POOL];
  volatile uint32_t noverflow_pool;

  struct v3d_file files[V3D_MAX_FILES];
  struct v3d_fence sync_files[V3D_MAX_SYNC_FILES];
  uint8_t sync_file_used[V3D_MAX_SYNC_FILES];

  struct v3d_stats stats;
  int dumped;				/* the first hang's dump is taken */
};

/* Errors are Linux's errno values, negated. */
#define V3D_EPERM 1
#define V3D_ENOENT 2
#define V3D_EINTR 4
#define V3D_EIO 5
#define V3D_E2BIG 7
#define V3D_EAGAIN 11
#define V3D_ENOMEM 12
#define V3D_EFAULT 14
#define V3D_EBUSY 16
#define V3D_EINVAL 22
#define V3D_ENOSPC 28
#define V3D_ENOTTY 25
#define V3D_ETIME 62
#define V3D_EOPNOTSUPP 95

/* v3d_hw.c: the hardware (from Linux's v3d_drv.c, v3d_gem.c, v3d_mmu.c,
   v3d_irq.c and v3d_sched.c) */
int v3d_hw_probe (struct v3d_dev *v3d);	/* identify; 0 or -errno */
void v3d_init_hw_state (struct v3d_dev *v3d);
int v3d_mmu_set_page_table (struct v3d_dev *v3d);
int v3d_mmu_flush_all (struct v3d_dev *v3d);
void v3d_invalidate_caches (struct v3d_dev *v3d);
void v3d_clean_caches (struct v3d_dev *v3d);
void v3d_irq_enable (struct v3d_dev *v3d);
void v3d_irq_disable (struct v3d_dev *v3d);
int v3d_irq (struct v3d_dev *v3d);	/* the interrupt: 1 if it was ours */
void v3d_reset (struct v3d_dev *v3d);
void v3d_record_hang (struct v3d_dev *v3d, struct v3d_job *job);
void v3d_dump_hang (struct v3d_dev *v3d, struct v3d_job *job);

/* The dump of the first hung job (0.06), what *V3DDump saves: words.
   "V3DD" "UMP1", header words (V3D_DUMP_HEAD), the queue, the list
   start and end, the bin list start and end, qma, qms, qts, the number
   of BOs, the words of struct v3d_hang, then per BO (the job's, then
   its binner overflow BOs): GPU address, bytes, contents. */
#define V3D_DUMP_HEAD 12
void v3d_idle_sms (struct v3d_dev *v3d);
void v3d_power_off_sms (struct v3d_dev *v3d);
void v3d_run_job (struct v3d_dev *v3d, struct v3d_job *job);
int v3d_job_progressing (struct v3d_dev *v3d, struct v3d_job *job);
void v3d_overflow_supply (struct v3d_dev *v3d);
const char *v3d_axi_client (uint32_t id);
void v3d_mmu_insert_ptes (struct v3d_dev *v3d, int bo);
void v3d_mmu_remove_ptes (struct v3d_dev *v3d, int bo);

/* v3d_sched.c: queues, fences, sync objects */
int v3d_fence_done (struct v3d_dev *v3d, struct v3d_fence f);
void v3d_pump (struct v3d_dev *v3d);	/* interrupts off */
void v3d_job_done (struct v3d_dev *v3d, int q, int failed);	/* interrupts off */
struct v3d_job *v3d_job_alloc (struct v3d_dev *v3d);
void v3d_push_job (struct v3d_dev *v3d, struct v3d_job *job);
void v3d_service (struct v3d_dev *v3d);	/* interrupts on, not in an interrupt */
int v3d_wait_fences (struct v3d_dev *v3d, const struct v3d_fence *f, int n,
		     int all, int64_t deadline_ns, int *first);
int v3d_idle (struct v3d_dev *v3d);
void v3d_gpu_reset_now (struct v3d_dev *v3d);
int64_t v3d_now_ns (void);

/* v3d_bo.c: buffers */
int v3d_bo_create (struct v3d_dev *v3d, uint32_t bytes, int *bo_out);
int v3d_page_used (struct v3d_dev *v3d, uint32_t page);
void v3d_bo_get (struct v3d_dev *v3d, int bo);
void v3d_bo_put (struct v3d_dev *v3d, int bo);	/* not with interrupts off */
int v3d_bo_init (struct v3d_dev *v3d);
void v3d_bo_fini (struct v3d_dev *v3d);
static inline uint32_t v3d_bo_offset (struct v3d_dev *v3d, int bo)
{ return v3d->bos[bo].first << V3D_MMU_PAGE_SHIFT; }
static inline void *v3d_bo_cpu (struct v3d_dev *v3d, int bo)
{ return v3d->area + (v3d->bos[bo].first << V3D_MMU_PAGE_SHIFT); }

/* v3d_ioctl.c: the request interface */
int v3d_init (struct v3d_dev *v3d);	/* after the platform's power-on */
void v3d_fini (struct v3d_dev *v3d);
int v3d_open (struct v3d_dev *v3d, uint32_t owner);	/* -> file, or -errno */
void v3d_close (struct v3d_dev *v3d, int file);
int v3d_ioctl (struct v3d_dev *v3d, int file, uint32_t request, void *arg);
int v3d_file_valid (struct v3d_dev *v3d, int file);

#include "v3d_plat.h"
#endif
