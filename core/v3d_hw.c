/* v3d_hw.c: the V3D hardware, as Linux's v3d driver drives it.

   Each function here follows the Linux function of the same name in
   drivers/gpu/drm/v3d (Raspberry Pi kernel rpi-6.12.y): v3d_drv.c
   (probe, SMS), v3d_gem.c (hardware state, caches, reset), v3d_mmu.c,
   v3d_irq.c and v3d_sched.c (starting jobs, timeouts). What is different
   is said where it is: the Linux kernel's locks, fences, work queues and
   tracing are replaced by the core's queues (v3d_sched.c), and waits are
   bounded by a count of register reads, since they can run with
   interrupts off.

   V3D 4.1 and later only: 3.3's GCA and L2C are left out (RISC OS has no
   machine with one). 4.2 is the Pi 4's, 7.1 the Pi 5's.

   SPDX-License-Identifier: GPL-2.0-or-later
   Copyright (C) 2014-2018 Broadcom
   Copyright (C) 2023 Raspberry Pi
   Copyright (c) 2026 Andrew Youll (the RISC OS version) */
#include "v3d.h"
#include "v3d_regs.h"

#define V3D_READ(o) v3d_rd (v3d, V3D_BANK_HUB, (o))
#define V3D_WRITE(o, v) v3d_wr (v3d, V3D_BANK_HUB, (o), (v))
#define V3D_CORE_READ(c, o) v3d_rd (v3d, V3D_BANK_CORE0 + (c), (o))
#define V3D_CORE_WRITE(c, o, v) v3d_wr (v3d, V3D_BANK_CORE0 + (c), (o), (v))
#define V3D_SMS_READ(o) v3d_rd (v3d, V3D_BANK_SMS, (o))
#define V3D_SMS_WRITE(o, v) v3d_wr (v3d, V3D_BANK_SMS, (o), (v))

#define V3D_SMS_IDLE 0x0
#define V3D_SMS_ISOLATING_FOR_RESET 0xa
#define V3D_SMS_RESETTING 0xb
#define V3D_SMS_POWER_OFF_STATE 0xd

/* Linux's wait_for (cond, 100 ms), as a count: a register read takes
   well over 100 ns, so this is at least as long. */
#define WAIT_READS 1000000u
#define wait_for(cond) \
  ({ uint32_t _n = 0; while (!(cond) && ++_n < WAIT_READS) ; _n >= WAIT_READS; })

#define V3D_PTE_SUPERPAGE BIT(31)
#define V3D_PTE_BIGPAGE BIT(30)
#define V3D_PTE_WRITEABLE BIT(29)
#define V3D_PTE_VALID BIT(28)

#define V3D_CORE_IRQS(ver) ((u32) (V3D_INT_OUTOMEM | V3D_INT_FLDONE \
				   | V3D_INT_FRDONE | V3D_INT_CSDDONE (ver) \
				   | (ver < 71 ? V3D_INT_GMPV : 0)))
#define V3D_HUB_IRQS(ver) ((u32) (V3D_HUB_INT_MMU_WRV | V3D_HUB_INT_MMU_PTI \
				  | V3D_HUB_INT_MMU_CAP | V3D_HUB_INT_TFUC \
				  | (ver >= 71 ? V3D_V7_HUB_INT_GMPV : 0)))

/* ---- v3d_drv.c ---- */

void
v3d_idle_sms (struct v3d_dev *v3d)
{
  if (v3d->ver < V3D_GEN_71)
    return;
  V3D_SMS_WRITE (V3D_SMS_TEE_CS, V3D_SMS_CLEAR_POWER_OFF);
  if (wait_for (V3D_GET_FIELD (V3D_SMS_READ (V3D_SMS_TEE_CS), V3D_SMS_STATE)
		== V3D_SMS_IDLE))
    plat_log (v3d, "Failed to power up SMS", 0);
  /* v3d_reset_sms */
  V3D_SMS_WRITE (V3D_SMS_REE_CS, V3D_SET_FIELD (0x4, V3D_SMS_STATE));
  if (wait_for (V3D_GET_FIELD (V3D_SMS_READ (V3D_SMS_REE_CS), V3D_SMS_STATE)
		!= V3D_SMS_ISOLATING_FOR_RESET
		&& V3D_GET_FIELD (V3D_SMS_READ (V3D_SMS_REE_CS), V3D_SMS_STATE)
		!= V3D_SMS_RESETTING))
    plat_log (v3d, "Failed to wait for SMS reset", 0);
}

void
v3d_power_off_sms (struct v3d_dev *v3d)
{
  if (v3d->ver < V3D_GEN_71)
    return;
  V3D_SMS_WRITE (V3D_SMS_TEE_CS, V3D_SMS_POWER_OFF);
  if (wait_for (V3D_GET_FIELD (V3D_SMS_READ (V3D_SMS_TEE_CS), V3D_SMS_STATE)
		== V3D_SMS_POWER_OFF_STATE))
    plat_log (v3d, "Failed to power off SMS", 0);
}

/* The part of v3d_platform_drm_probe that reads the hardware. The
   platform has mapped the registers and powered the V3D up. */
int
v3d_hw_probe (struct v3d_dev *v3d)
{
  int gen = v3d->ver;
  v3d_idle_sms (v3d);

  uint32_t mmu_debug = V3D_READ (V3D_MMU_DEBUG_INFO);
  v3d->pa_width = 30 + V3D_GET_FIELD (mmu_debug, V3D_MMU_PA_WIDTH);
  v3d->va_width = 30 + V3D_GET_FIELD (mmu_debug, V3D_MMU_VA_WIDTH);

  uint32_t ident1 = V3D_READ (V3D_HUB_IDENT1);
  v3d->ver = V3D_GET_FIELD (ident1, V3D_HUB_IDENT1_TVER) * 10
	     + V3D_GET_FIELD (ident1, V3D_HUB_IDENT1_REV);
  v3d->cores = V3D_GET_FIELD (ident1, V3D_HUB_IDENT1_NCORES);
  v3d->rev = V3D_GET_FIELD (V3D_READ (V3D_HUB_IDENT3), V3D_HUB_IDENT3_IPREV);
  v3d->ident[0] = V3D_READ (V3D_HUB_IDENT0);
  v3d->ident[1] = ident1;
  v3d->ident[2] = V3D_READ (V3D_HUB_IDENT2);
  v3d->ident[3] = V3D_READ (V3D_HUB_IDENT3);

  /* Linux only warns; here a mismatch means the wrong machine or a V3D
     that isn't powered (it reads 0xdeadbeef), so it is refused. */
  if (v3d->ver != gen || v3d->cores < 1)
    return -V3D_EIO;
  if (v3d->cores > 1)			/* multicore not yet implemented */
    v3d->cores = 1;
  return 0;
}

/* ---- v3d_gem.c ---- */

static void
v3d_init_core (struct v3d_dev *v3d, int core)
{
  /* (V3D 3.3's OVRTMUOUT isn't needed from 4.1.) Whenever we flush the
     L2T cache, we always want to flush the whole thing. */
  V3D_CORE_WRITE (core, V3D_CTL_L2TFLSTA, 0);
  V3D_CORE_WRITE (core, V3D_CTL_L2TFLEND, ~0u);
  /* (0.07) Linux leaves CT1QCFG at its reset value (0); a V3D that was
     already running at load hasn't been reset, so set it: no empty-tile
     filtering. */
  V3D_CORE_WRITE (core, V3D_CLE_CT1QCFG, 0);
}

void
v3d_init_hw_state (struct v3d_dev *v3d)
{
  v3d_init_core (v3d, 0);
}

/* Invalidates texture L2 cachelines. While there is a busy bit
   (V3D_L2TCACTL_L2TFLS), we don't need to wait for completion before
   dispatching the job: L2T accesses will be stalled until the flush has
   completed. (Linux holds cache_clean_lock so that this doesn't start
   while a clean is waiting; here both run with interrupts off.) */
static void
v3d_flush_l2t (struct v3d_dev *v3d, int core)
{
  V3D_CORE_WRITE (core, V3D_CTL_L2TCACTL,
		  V3D_L2TCACTL_L2TFLS
		  | V3D_SET_FIELD (V3D_L2TCACTL_FLM_FLUSH, V3D_L2TCACTL_FLM));
}

/* Cleans texture L1 and L2 cachelines (writing back dirty data), after a
   CSD job, before its completion is signalled. */
void
v3d_clean_caches (struct v3d_dev *v3d)
{
  int core = 0;
  V3D_CORE_WRITE (core, V3D_CTL_L2TCACTL, V3D_L2TCACTL_TMUWCF);
  if (wait_for (!(V3D_CORE_READ (core, V3D_CTL_L2TCACTL) & V3D_L2TCACTL_TMUWCF)))
    plat_log (v3d, "Timeout waiting for TMU write combiner flush", 0);
  V3D_CORE_WRITE (core, V3D_CTL_L2TCACTL,
		  V3D_L2TCACTL_L2TFLS
		  | V3D_SET_FIELD (V3D_L2TCACTL_FLM_CLEAN, V3D_L2TCACTL_FLM));
  if (wait_for (!(V3D_CORE_READ (core, V3D_CTL_L2TCACTL) & V3D_L2TCACTL_L2TFLS)))
    plat_log (v3d, "Timeout waiting for L2T clean", 0);
}

/* Invalidates the slice caches. These are read-only caches. */
static void
v3d_invalidate_slices (struct v3d_dev *v3d, int core)
{
  V3D_CORE_WRITE (core, V3D_CTL_SLCACTL,
		  V3D_SET_FIELD (0xf, V3D_SLCACTL_TVCCS)
		  | V3D_SET_FIELD (0xf, V3D_SLCACTL_TDCCS)
		  | V3D_SET_FIELD (0xf, V3D_SLCACTL_UCC)
		  | V3D_SET_FIELD (0xf, V3D_SLCACTL_ICC));
}

/* Invalidate the caches from the outside in. (L3 and L2C are V3D 3.x.) */
void
v3d_invalidate_caches (struct v3d_dev *v3d)
{
  v3d_flush_l2t (v3d, 0);
  v3d_invalidate_slices (v3d, 0);
}

/* The registers when a job is found hung, for *V3DRender (0.05) */
void
v3d_record_hang (struct v3d_dev *v3d, struct v3d_job *job)
{
  struct v3d_hang *h = &v3d->stats.last_hang;
  h->q = job->q;
  h->start = h->end = 0;
  if (job->q == V3D_BIN || job->q == V3D_RENDER)
    {
      h->start = job->a.cl.start;
      h->end = job->a.cl.end;
    }
  h->ct0cs = V3D_CORE_READ (0, V3D_CLE_CTNCS (0));
  h->ct0ca = V3D_CORE_READ (0, V3D_CLE_CTNCA (0));
  h->ct0ea = V3D_CORE_READ (0, V3D_CLE_CTNEA (0));
  h->ct0ra = V3D_CORE_READ (0, V3D_CLE_CTNRA (0));
  h->ct1cs = V3D_CORE_READ (0, V3D_CLE_CTNCS (1));
  h->ct1ca = V3D_CORE_READ (0, V3D_CLE_CTNCA (1));
  h->ct1ea = V3D_CORE_READ (0, V3D_CLE_CTNEA (1));
  h->ct1ra = V3D_CORE_READ (0, V3D_CLE_CTNRA (1));
  h->pcs = V3D_CORE_READ (0, V3D_CLE_PCS);
  h->bfc = V3D_CORE_READ (0, V3D_CLE_BFC);
  h->rfc = V3D_CORE_READ (0, V3D_CLE_RFC);
  h->err_stat = V3D_CORE_READ (0, V3D_ERR_STAT);
  h->fdbgo = V3D_CORE_READ (0, V3D_ERR_FDBGO);
  h->fdbgb = V3D_CORE_READ (0, V3D_ERR_FDBGB);
  h->fdbgr = V3D_CORE_READ (0, V3D_ERR_FDBGR);
  h->fdbgs = V3D_CORE_READ (0, V3D_ERR_FDBGS);
  h->bpca = V3D_CORE_READ (0, V3D_PTB_BPCA);
  h->bpcs = V3D_CORE_READ (0, V3D_PTB_BPCS);
  h->bpoa = V3D_CORE_READ (0, V3D_PTB_BPOA);
  h->bpos = V3D_CORE_READ (0, V3D_PTB_BPOS);
  uint32_t ca = job->q == V3D_RENDER ? h->ct1ca : job->q == V3D_BIN ? h->ct0ca : 0;
  h->words_at = ca ? (ca & ~3u) - 16 : 0;
  for (int i = 0; i < 8; i++)
    {
      uint32_t a = h->words_at + 4 * i;
      h->words[i] = ca && v3d_page_used (v3d, a >> V3D_MMU_PAGE_SHIFT)
	? *(volatile uint32_t *) (v3d->area + a) : 0;
    }
  if (v3d->stats.first_hang.q == ~0u)
    v3d->stats.first_hang = *h;
}

/* The first hung job's BOs as the GPU left them, with its registers,
   for *V3DDump (0.06). Interrupts on; the job is still active (hung). */
#define V3D_DUMP_MAX_BO (16u << 20)
void
v3d_dump_hang (struct v3d_dev *v3d, struct v3d_job *job)
{
  if (v3d->dumped || (job->q != V3D_BIN && job->q != V3D_RENDER))
    return;
  v3d->dumped = 1;
  /* a bin job's BOs are held by its render job (the submit's last) */
  struct v3d_job *r = job->q == V3D_BIN && job->a.cl.render ? job->a.cl.render : job;
  uint32_t hwords = sizeof (struct v3d_hang) / 4;
  uint32_t bytes = (V3D_DUMP_HEAD + hwords) * 4;
  uint32_t n = r->nbos + r->noverflow;
  for (uint32_t i = 0; i < n; i++)
    {
      int bo = i < r->nbos ? r->bos[i] : r->overflow[i - r->nbos];
      uint32_t size = v3d->bos[bo].npages << V3D_MMU_PAGE_SHIFT;
      bytes += 8 + (size <= V3D_DUMP_MAX_BO ? size : 0);
    }
  uint32_t *w = plat_dump_buffer (bytes);
  if (!w)
    return;
  w[0] = 0x44443356;			/* "V3DD" */
  w[1] = 0x31504d55;			/* "UMP1" */
  w[2] = V3D_DUMP_HEAD;
  w[3] = job->q;
  w[4] = r->a.cl.start;			/* the render list */
  w[5] = r->a.cl.end;
  w[6] = r->a.cl.bstart;		/* the bin list */
  w[7] = r->a.cl.bend;
  w[8] = r->a.cl.qma;
  w[9] = r->a.cl.qms;
  w[10] = r->a.cl.qts;
  w[11] = n;
  const uint32_t *h = (const uint32_t *) &v3d->stats.last_hang;
  for (uint32_t i = 0; i < hwords; i++)
    w[V3D_DUMP_HEAD + i] = h[i];
  uint32_t *p = w + V3D_DUMP_HEAD + hwords;
  for (uint32_t i = 0; i < n; i++)
    {
      int bo = i < r->nbos ? r->bos[i] : r->overflow[i - r->nbos];
      uint32_t size = v3d->bos[bo].npages << V3D_MMU_PAGE_SHIFT;
      if (size > V3D_DUMP_MAX_BO)
	size = 0;			/* too big to keep: listed only */
      *p++ = v3d_bo_offset (v3d, bo);
      *p++ = size;
      const volatile uint32_t *src = (const volatile uint32_t *) v3d_bo_cpu (v3d, bo);
      for (uint32_t k = 0; k < size / 4; k++)
	*p++ = src[k];
    }
}

/* After a hang: Linux's v3d_reset. The platform's reset line stands for
   reset_control_reset (or the bridge's SW_INIT on the 7278). */
void
v3d_reset (struct v3d_dev *v3d)
{
  plat_log (v3d, v3d->reset_asked ? "Resetting GPU on request. V3D_ERR_STAT:"
	    : "Resetting GPU for hang. V3D_ERR_STAT:", V3D_CORE_READ (0, V3D_ERR_STAT));
  v3d_irq_disable (v3d);
  if (v3d->ver >= V3D_GEN_71)
    {
      /* v3d_reset_sms */
      V3D_SMS_WRITE (V3D_SMS_REE_CS, V3D_SET_FIELD (0x4, V3D_SMS_STATE));
      if (wait_for (V3D_GET_FIELD (V3D_SMS_READ (V3D_SMS_REE_CS), V3D_SMS_STATE)
		    != V3D_SMS_ISOLATING_FOR_RESET
		    && V3D_GET_FIELD (V3D_SMS_READ (V3D_SMS_REE_CS), V3D_SMS_STATE)
		    != V3D_SMS_RESETTING))
	plat_log (v3d, "Failed to wait for SMS reset", 0);
    }
  plat_gpu_reset (v3d);
  v3d_init_hw_state (v3d);
  v3d_mmu_set_page_table (v3d);
  v3d_irq_enable (v3d);
  v3d->stats.resets++;
}

/* ---- v3d_mmu.c ----
   The V3D has an MMU with a single level of page tables for its 4 GB
   address space, mapping to bus addresses: up to 4 MB of physically
   contiguous memory for the PTEs. All BOs share it. */

int
v3d_mmu_flush_all (struct v3d_dev *v3d)
{
  V3D_WRITE (V3D_MMUC_CONTROL, V3D_MMUC_CONTROL_FLUSH | V3D_MMUC_CONTROL_ENABLE);
  if (wait_for (!(V3D_READ (V3D_MMUC_CONTROL) & V3D_MMUC_CONTROL_FLUSHING)))
    {
      plat_log (v3d, "MMUC flush wait idle failed", 0);
      return -V3D_ETIME;
    }
  V3D_WRITE (V3D_MMU_CTL, V3D_READ (V3D_MMU_CTL) | V3D_MMU_CTL_TLB_CLEAR);
  if (wait_for (!(V3D_READ (V3D_MMU_CTL) & V3D_MMU_CTL_TLB_CLEARING)))
    {
      plat_log (v3d, "MMU TLB clear wait idle failed", 0);
      return -V3D_ETIME;
    }
  return 0;
}

int
v3d_mmu_set_page_table (struct v3d_dev *v3d)
{
  V3D_WRITE (V3D_MMU_PT_PA_BASE, (uint32_t) (v3d->pt_pa >> V3D_MMU_PAGE_SHIFT));
  V3D_WRITE (V3D_MMU_CTL,
	     V3D_MMU_CTL_ENABLE
	     | V3D_MMU_CTL_PT_INVALID_ENABLE
	     | V3D_MMU_CTL_PT_INVALID_ABORT
	     | V3D_MMU_CTL_PT_INVALID_INT
	     | V3D_MMU_CTL_WRITE_VIOLATION_ABORT
	     | V3D_MMU_CTL_WRITE_VIOLATION_INT
	     | V3D_MMU_CTL_CAP_EXCEEDED_ABORT
	     | V3D_MMU_CTL_CAP_EXCEEDED_INT);
  V3D_WRITE (V3D_MMU_ILLEGAL_ADDR,
	     (uint32_t) (v3d->scratch_pa >> V3D_MMU_PAGE_SHIFT)
	     | V3D_MMU_ILLEGAL_ADDR_ENABLE);
  V3D_WRITE (V3D_MMUC_CONTROL, V3D_MMUC_CONTROL_ENABLE);
  return v3d_mmu_flush_all (v3d);
}

/* ---- v3d_irq.c ----
   When we take a bin, render, TFU done, or CSD done interrupt, the job
   on that queue is finished and the next may start. When we take the
   binner out of memory interrupt, the binner gets more memory so that
   the current job can make progress: from the pool, since nothing can be
   allocated here (Linux allocates in a work item). */

void
v3d_overflow_supply (struct v3d_dev *v3d)
{
  struct v3d_job *bin = v3d->active[V3D_BIN];
  /* We lost a race, and the bin job completed and exited: the HW
     signals OOM before it's fully OOM, so the binner might just barely
     complete. */
  if (!bin)
    return;
  struct v3d_job *render = bin->a.cl.render;
  if (!v3d->noverflow_pool || render->noverflow == V3D_JOB_OVERFLOWS)
    {
      v3d->stats.overflow_misses++;	/* the job will time out */
      return;
    }
  int bo = v3d->overflow_pool[--v3d->noverflow_pool];
  render->overflow[render->noverflow++] = (uint16_t) bo;
  v3d->stats.overflows++;
  v3d_mmu_flush_all (v3d);
  V3D_CORE_WRITE (0, V3D_PTB_BPOA, v3d_bo_offset (v3d, bo));
  V3D_CORE_WRITE (0, V3D_PTB_BPOS, V3D_OVERFLOW_BYTES);
}

static const char *const v3d41_axi_ids[] = {
  "L2T", "PTB", "PSE", "TLB", "CLE", "TFU", "MMU", "GMP",
};

/* v3d_irq and v3d_hub_irq together. The Pi 4's V3D has one interrupt
   line for both; the Pi 5's has two, and either may call this. Linux
   reads the hub only when the core had nothing; reading both costs a
   register read and means neither waits for the line to be taken
   again. Interrupts are off. */
int
v3d_irq (struct v3d_dev *v3d)
{
  int handled = 0;
  uint32_t intsts = V3D_CORE_READ (0, V3D_CTL_INT_STS);
  V3D_CORE_WRITE (0, V3D_CTL_INT_CLR, intsts);	/* acknowledge */

  if (intsts & V3D_INT_OUTOMEM)
    {
      /* Edge signalled: it won't happen again until we actually add
	 more memory. As of 4.1, FLDONE won't be reported until any OOM
	 state has been cleared. */
      v3d_overflow_supply (v3d);
      handled = 1;
    }
  if ((intsts & V3D_INT_FLDONE) && v3d->active[V3D_BIN])
    {
      v3d_job_done (v3d, V3D_BIN, 0);
      handled = 1;
    }
  if ((intsts & V3D_INT_FRDONE) && v3d->active[V3D_RENDER])
    {
      v3d_job_done (v3d, V3D_RENDER, 0);
      handled = 1;
    }
  if ((intsts & V3D_INT_CSDDONE (v3d->ver)) && v3d->active[V3D_CSD])
    {
      v3d_job_done (v3d, V3D_CSD, 0);
      handled = 1;
    }
  /* We shouldn't be triggering these if we have GMP in always-allowed
     mode. */
  if (v3d->ver < V3D_GEN_71 && (intsts & V3D_INT_GMPV))
    {
      v3d->stats.gmp_violations++;
      handled = 1;
    }

  uint32_t hubsts = V3D_READ (V3D_HUB_INT_STS);
  V3D_WRITE (V3D_HUB_INT_CLR, hubsts);
  if ((hubsts & V3D_HUB_INT_TFUC) && v3d->active[V3D_TFU])
    {
      v3d_job_done (v3d, V3D_TFU, 0);
      handled = 1;
    }
  if (hubsts & (V3D_HUB_INT_MMU_WRV | V3D_HUB_INT_MMU_PTI | V3D_HUB_INT_MMU_CAP))
    {
      uint32_t axi_id = V3D_READ (V3D_MMU_VIO_ID);
      /* the address is a page address beyond 32 bits: kept as Linux
	 prints it, shifted down to fit */
      v3d->stats.mmu_last_addr = V3D_READ (V3D_MMU_VIO_ADDR);
      V3D_WRITE (V3D_MMU_CTL, V3D_READ (V3D_MMU_CTL));
      v3d->stats.mmu_last_client = axi_id >> 5;
      v3d->stats.mmu_last_sts = hubsts;
      v3d->stats.mmu_errors++;
      handled = 1;
    }
  if (v3d->ver >= V3D_GEN_71 && (hubsts & V3D_V7_HUB_INT_GMPV))
    {
      v3d->stats.gmp_violations++;
      handled = 1;
    }

  v3d->stats.irqs++;
  if (!handled)
    v3d->stats.irqs_empty++;
  v3d_pump (v3d);
  return handled;
}

const char *
v3d_axi_client (uint32_t id)
{
  return id < sizeof v3d41_axi_ids / sizeof v3d41_axi_ids[0] ? v3d41_axi_ids[id] : "?";
}

/* Enable our set of interrupts, masking out any others. */
void
v3d_irq_enable (struct v3d_dev *v3d)
{
  /* Clear any pending interrupts someone might have left around for
     us (v3d_irq_init). */
  V3D_CORE_WRITE (0, V3D_CTL_INT_CLR, V3D_CORE_IRQS (v3d->ver));
  V3D_WRITE (V3D_HUB_INT_CLR, V3D_HUB_IRQS (v3d->ver));
  V3D_CORE_WRITE (0, V3D_CTL_INT_MSK_SET, ~V3D_CORE_IRQS (v3d->ver));
  V3D_CORE_WRITE (0, V3D_CTL_INT_MSK_CLR, V3D_CORE_IRQS (v3d->ver));
  V3D_WRITE (V3D_HUB_INT_MSK_SET, ~V3D_HUB_IRQS (v3d->ver));
  V3D_WRITE (V3D_HUB_INT_MSK_CLR, V3D_HUB_IRQS (v3d->ver));
}

void
v3d_irq_disable (struct v3d_dev *v3d)
{
  V3D_CORE_WRITE (0, V3D_CTL_INT_MSK_SET, ~0u);
  V3D_WRITE (V3D_HUB_INT_MSK_SET, ~0u);
  /* Clear any pending interrupts we might have left. */
  V3D_CORE_WRITE (0, V3D_CTL_INT_CLR, V3D_CORE_IRQS (v3d->ver));
  V3D_WRITE (V3D_HUB_INT_CLR, V3D_HUB_IRQS (v3d->ver));
}

/* ---- v3d_sched.c: starting jobs ----
   For simplicity, and to keep latency low, a new job goes to the HW only
   when it has completed the last one, instead of filling up the CT[01]Q
   FIFOs with jobs. */

#define V3D_CSD_CFG012_WG_COUNT_SHIFT 16

void
v3d_run_job (struct v3d_dev *v3d, struct v3d_job *job)
{
  switch (job->q)
    {
    case V3D_BIN:
      /* Clear out the overflow allocation, so we don't reuse the
	 overflow attached to a previous job. */
      V3D_CORE_WRITE (0, V3D_PTB_BPOS, 0);
      v3d_invalidate_caches (v3d);
      /* Set the current and end address of the control list. Writing
	 the end register is what starts the job. */
      if (job->a.cl.qma)
	{
	  V3D_CORE_WRITE (0, V3D_CLE_CT0QMA, job->a.cl.qma);
	  V3D_CORE_WRITE (0, V3D_CLE_CT0QMS, job->a.cl.qms);
	}
      if (job->a.cl.qts)
	V3D_CORE_WRITE (0, V3D_CLE_CT0QTS, V3D_CLE_CT0QTS_ENABLE | job->a.cl.qts);
      V3D_CORE_WRITE (0, V3D_CLE_CT0QBA, job->a.cl.start);
      V3D_CORE_WRITE (0, V3D_CLE_CT0QEA, job->a.cl.end);
      break;
    case V3D_RENDER:
      /* Can we avoid this flush? We need to be careful of scheduling,
	 though -- imagine job0 rendering to texture and job1 reading,
	 and them being executed as bin0, bin1, render0, render1, so that
	 render1's flush at bin time wasn't enough. */
      v3d_invalidate_caches (v3d);
      V3D_CORE_WRITE (0, V3D_CLE_CT1QBA, job->a.cl.start);
      V3D_CORE_WRITE (0, V3D_CLE_CT1QEA, job->a.cl.end);
      break;
    case V3D_TFU:
      {
	const struct drm_v3d_submit_tfu *a = &job->a.tfu;
	int ver = v3d->ver;
	V3D_WRITE (V3D_TFU_IIA (ver), a->iia);
	V3D_WRITE (V3D_TFU_IIS (ver), a->iis);
	V3D_WRITE (V3D_TFU_ICA (ver), a->ica);
	V3D_WRITE (V3D_TFU_IUA (ver), a->iua);
	V3D_WRITE (V3D_TFU_IOA (ver), a->ioa);
	if (ver >= V3D_GEN_71)
	  V3D_WRITE (V3D_V7_TFU_IOC, a->v71.ioc);
	V3D_WRITE (V3D_TFU_IOS (ver), a->ios);
	V3D_WRITE (V3D_TFU_COEF0 (ver), a->coef[0]);
	if (ver >= V3D_GEN_71 || (a->coef[0] & V3D_TFU_COEF0_USECOEF))
	  {
	    V3D_WRITE (V3D_TFU_COEF1 (ver), a->coef[1]);
	    V3D_WRITE (V3D_TFU_COEF2 (ver), a->coef[2]);
	    V3D_WRITE (V3D_TFU_COEF3 (ver), a->coef[3]);
	  }
	/* ICFG kicks off the job. */
	V3D_WRITE (V3D_TFU_ICFG (ver), a->icfg | V3D_TFU_ICFG_IOC);
	break;
      }
    case V3D_CSD:
      {
	v3d_invalidate_caches (v3d);
	uint32_t cfg0 = V3D_CSD_QUEUED_CFG0 (v3d->ver);
	for (int i = 1; i <= 6; i++)
	  V3D_CORE_WRITE (0, cfg0 + 4 * i, job->a.csd.cfg[i]);
	/* 7.1's eighth configuration register isn't used: make sure it
	   remains unused. */
	if (v3d->ver >= V3D_GEN_71)
	  V3D_CORE_WRITE (0, V3D_V7_CSD_QUEUED_CFG7, 0);
	/* CFG0 write kicks off the job. */
	V3D_CORE_WRITE (0, cfg0, job->a.csd.cfg[0]);
	break;
      }
    }
}

/* The timeout test (v3d_cl_job_timedout, v3d_csd_job_timedout): if the
   current address or return address have changed, then the GPU has
   probably made progress and we should delay the reset. TFU jobs are
   reset at the first timeout (v3d_generic_job_timedout). */
int
v3d_job_progressing (struct v3d_dev *v3d, struct v3d_job *job)
{
  uint32_t a, b;
  switch (job->q)
    {
    case V3D_BIN:
    case V3D_RENDER:
      a = V3D_CORE_READ (0, V3D_CLE_CTNCA (job->q));
      b = V3D_CORE_READ (0, V3D_CLE_CTNRA (job->q));
      break;
    case V3D_CSD:
      a = V3D_CORE_READ (0, V3D_CSD_CURRENT_CFG4 (v3d->ver));
      b = 0;
      break;
    default:
      return 0;
    }
  if (job->timedout_ctca != a || job->timedout_ctra != b)
    {
      job->timedout_ctca = a;
      job->timedout_ctra = b;
      return 1;
    }
  return 0;
}

/* the page table entries for a BO (v3d_mmu_insert_ptes): 4 KB pages,
   64 KB big pages or 1 MB super pages where the memory allows */
void v3d_mmu_insert_ptes (struct v3d_dev *v3d, int bo);
void v3d_mmu_remove_ptes (struct v3d_dev *v3d, int bo);

static int
v3d_mmu_is_aligned (uint32_t page, uint32_t page_address, uint32_t alignment)
{
  uint32_t m = (alignment >> V3D_MMU_PAGE_SHIFT) - 1;
  return (page & m) == 0 && (page_address & m) == 0;
}

void
v3d_mmu_insert_ptes (struct v3d_dev *v3d, int bo)
{
  struct v3d_bo *b = &v3d->bos[bo];
  uint32_t page = b->first, end = b->first + b->npages;
  while (page < end)
    {
      /* a run of pages whose physical addresses follow on */
      uint32_t pfn = (uint32_t) (plat_page_pa (v3d, page) >> V3D_MMU_PAGE_SHIFT);
      uint32_t run = 1;
      while (page + run < end
	     && (uint32_t) (plat_page_pa (v3d, page + run) >> V3D_MMU_PAGE_SHIFT)
		== pfn + run)
	run++;
      while (run > 0)
	{
	  uint32_t page_address = V3D_PTE_WRITEABLE | V3D_PTE_VALID | pfn, size;
	  if (run >= 256 && v3d_mmu_is_aligned (page, pfn, 1u << 20))
	    {
	      size = 256;
	      page_address |= V3D_PTE_SUPERPAGE;
	    }
	  else if (run >= 16 && v3d_mmu_is_aligned (page, pfn, 1u << 16))
	    {
	      size = 16;
	      page_address |= V3D_PTE_BIGPAGE;
	    }
	  else
	    size = 1;
	  for (uint32_t i = 0; i < size; i++)
	    v3d->pt[page++] = page_address + i;
	  pfn += size;
	  run -= size;
	}
    }
  plat_barrier ();
  v3d_mmu_flush_all (v3d);
}

void
v3d_mmu_remove_ptes (struct v3d_dev *v3d, int bo)
{
  struct v3d_bo *b = &v3d->bos[bo];
  for (uint32_t page = b->first; page < b->first + b->npages; page++)
    v3d->pt[page] = 0;
  plat_barrier ();
  v3d_mmu_flush_all (v3d);
}
