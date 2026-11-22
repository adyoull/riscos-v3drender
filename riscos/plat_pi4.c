/* plat_pi4.c: the Raspberry Pi 4 under RISC OS: registers, power, memory
   and the interrupt for the core (v3d_plat.h).

   The power-on sequence, the check for a V3D that is already running
   (so that reloading the module never resets a working GPU) and the
   reset sequence come from Rob Heaton's V3DDriver (GPL-2.0-or-later,
   github.com/robheaton/riscos-v3d, c/module.c v3d_hw_is_up,
   v3d_stop_bridge and v3d_power_on), proven on real Pi 4s; he took the
   register offsets and the sequence from rpi4-gpu-bare-metal-examples
   (MIT, Copyright (c) 2022 Random06457) and checked them against Linux's
   bcm2835-power.c. The firmware clock calls follow his v3d_clock.c.
   The interrupt is device 10 (iDev_GPU_VC3D: the V3D is VideoCore
   interrupt 10, GIC SPI 74), as in his v3d_irq.c.

   Memory:
   - The BOs: a physical memory pool (PMP) "V3DRender buffers". A BO's
     pages are claimed from RISC OS one by one, locked so that the OS
     never moves them, and mapped where the BO sits in the area, not
     cached but bufferable (Normal non-cacheable: Linux maps v3d BOs
     write-combined), readable and writable by programs (Mesa writes its
     command lists and vertices straight into them).
   - The GPU's page table: 4 MB, physically contiguous, in its own PMP,
     plus a page that the MMU sends illegal accesses to. Not accessible
     to programs.

   SPDX-License-Identifier: GPL-2.0-or-later
   Copyright (C) 2026 Rob Heaton (the Pi 4 power and reset sequences)
   Copyright (c) 2026 Andrew Youll */
#include "cmodule.h"
#include "v3d.h"
#include "v3d_regs.h"
#include "plat_pi4.h"

#define OS_ReadMonotonicTime	0x42
#define OS_Memory		0x68
#define BCMSupport_SendTempPropertyBuffer 0x591C5

/* the Pi 4's "low peripheral" physical addresses (as V3DDriver) */
#define PERIPHERAL_BASE	0xFE000000u
#define PM_BASE		(PERIPHERAL_BASE + 0x100000u)
#define PM_GRAFX	0x10Cu			/* from PM_BASE */
#define PM_PASSWORD	0x5A000000u
#define PM_V3DRSTN	(1u << 6)
#define V3D_BASE	(PERIPHERAL_BASE + 0xC00000u)	/* hub; core 0 at +0x4000 */
#define ASB_V3D_S_CTRL	0x11008u		/* from V3D_BASE */
#define ASB_V3D_M_CTRL	0x1100Cu
#define ASB_REQ_STOP	(1u << 0)
#define ASB_ACK		(1u << 1)

/* OS_DynamicArea */
#define DA_FLAG_NOT_DRAGGABLE	(1u << 7)
#define DA_FLAG_REQUIRES_PAGES	(1u << 8)
#define DA_FLAG_IS_PMP		(1u << 20)
#define PAGE_AP_USER_RW		0u
#define PAGE_AP_PRIVILEGED	2u
#define PAGE_NOT_CACHEABLE	(1u << 5)	/* bufferable: Normal, non-cacheable */
#define PAGE_LOCK		(1u << 15)
#define PAGE_CACHED_REQ		(1u << 31)	/* ours: map cacheable (0.11) */
#define DA_CLAIM_ANY		0xFFFFFFFEu	/* -2: the kernel picks */
#define DA_RELEASE		0xFFFFFFFFu	/* -1 */

struct pi4
{
  volatile uint32_t *v3d, *pm;		/* permanent mappings */
  struct pmp bo, pt;
};
static struct pi4 pi4;

/* ---- registers ---- */

uint32_t
v3d_rd (struct v3d_dev *v3d, int bank, uint32_t off)
{
  (void) v3d;
  if (bank > V3D_BANK_CORE0)
    return 0;				/* no SMS or bridge on the Pi 4 */
  return pi4.v3d[(bank * 0x4000u + off) / 4];
}

void
v3d_wr (struct v3d_dev *v3d, int bank, uint32_t off, uint32_t val)
{
  (void) v3d;
  if (bank <= V3D_BANK_CORE0)
    pi4.v3d[(bank * 0x4000u + off) / 4] = val;
}

#define REG(o) (pi4.v3d[(o) / 4])

/* ---- the firmware (V3DDriver's tag_call) ---- */

static oserror *
mailbox (uint32_t tag, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t *out0, uint32_t *out1)
{
  uint32_t req[9] = { sizeof req, 0, tag, 12, 0, a0, a1, a2, 0 }, resp[9];
  regs r = {{ (unsigned) req, (unsigned) resp, 0 }};
  oserror *e = swix_regs (BCMSupport_SendTempPropertyBuffer, &r);
  if (e)
    return e;
  if (out0)
    *out0 = resp[5];
  if (out1)
    *out1 = resp[6];
  return 0;
}

#define TAG_GET_BOARD_REVISION	0x00010002u
#define TAG_GET_CLOCK_STATE	0x00030001u
#define TAG_SET_CLOCK_STATE	0x00038001u
#define TAG_GET_CLOCK_RATE	0x00030002u
#define TAG_SET_CLOCK_RATE	0x00038002u
#define TAG_GET_MAX_CLOCK_RATE	0x00030004u
#define CLOCK_V3D		5u
#define TAG_GET_DOMAIN_STATE	0x00030030u	/* Linux's raspberrypi-power */
#define TAG_SET_DOMAIN_STATE	0x00038030u
#define DOMAIN_V3D		10u		/* RPI_POWER_DOMAIN_V3D */

/* the board's processor (new-style revision codes): 3 = BCM2711 */
int
pi4_soc (void)
{
  uint32_t rev;
  if (mailbox (TAG_GET_BOARD_REVISION, 0, 0, 0, &rev, 0))
    return -1;
  if (!(rev & (1u << 23)))
    return 0;				/* old-style: a Pi 1 */
  return (int) ((rev >> 12) & 15);
}

oserror *
pi4_clock (uint32_t *hz, uint32_t *max_hz)
{
  uint32_t echo;
  oserror *e = mailbox (TAG_GET_CLOCK_RATE, CLOCK_V3D, 0, 0, &echo, hz);
  if (!e && max_hz)
    e = mailbox (TAG_GET_MAX_CLOCK_RATE, CLOCK_V3D, 0, 0, &echo, max_hz);
  return e;
}

/* (0.14) the ARM's clock, for *V3DRender: frame rates depend on it */
oserror *
pi4_arm_clock (uint32_t *hz, uint32_t *max_hz)
{
  uint32_t echo;
  oserror *e = mailbox (TAG_GET_CLOCK_RATE, 3, 0, 0, &echo, hz);
  if (!e && max_hz)
    e = mailbox (TAG_GET_MAX_CLOCK_RATE, 3, 0, 0, &echo, max_hz);
  return e;
}

/* hz 0: the firmware's maximum. Only with no job running. */
oserror *
pi4_set_clock (uint32_t hz, uint32_t *now)
{
  uint32_t echo, max;
  oserror *e = mailbox (TAG_GET_MAX_CLOCK_RATE, CLOCK_V3D, 0, 0, &echo, &max);
  if (e)
    return e;
  if (!hz || (max && hz > max))
    hz = max;
  if (!hz)
    return 0;				/* the firmware didn't say: leave it */
  e = mailbox (TAG_SET_CLOCK_RATE, CLOCK_V3D, hz, 0, &echo, now);
  if (!e)
    e = mailbox (TAG_GET_CLOCK_RATE, CLOCK_V3D, 0, 0, &echo, now);
  return e;
}

/* ---- power (V3DDriver) ---- */

static void
spin (uint32_t n)
{
  for (volatile uint32_t i = 0; i < n; i++)
    ;
}

/* A powered V3D shows its reset released in PM_GRAFX, both ASB gates
   open, and the core's IDENT0 reading "V3D" (an unpowered one answers
   0xdeadbeef). The firmware's clock state can't be used: on a Pi 4 it
   reads "off" even while the V3D is running. */
int
pi4_v3d_is_up (void)
{
  if (!(pi4.pm[PM_GRAFX / 4] & PM_V3DRSTN))
    return 0;
  if ((REG (ASB_V3D_M_CTRL) | REG (ASB_V3D_S_CTRL)) & (ASB_REQ_STOP | ASB_ACK))
    return 0;
  return (REG (0x4000 + V3D_CTL_IDENT0) & 0x00FFFFFFu) == 0x00443356u;
}

ERROR_BLOCK (err_gate, 0x823E07, "A V3D bus gate didn't stop, so the GPU isn't being reset: restart the computer");
ERROR_BLOCK (err_open, 0x823E08, "The V3D's bus gates didn't open: restart the computer");

/* Stop the ASB bus gates, slave then master, before the reset is
   asserted (as Linux's bcm2835_asb_power_off). Resetting a running V3D
   behind open gates jams the slave bridge. */
static oserror *
stop_bridge (void)
{
  static const uint32_t order[2] = { ASB_V3D_S_CTRL, ASB_V3D_M_CTRL };
  if ((REG (ASB_V3D_S_CTRL) & ASB_REQ_STOP) && (REG (ASB_V3D_M_CTRL) & ASB_REQ_STOP))
    return 0;				/* already stopped: a fresh boot */
  for (int i = 0; i < 2; i++)
    {
      REG (order[i]) = PM_PASSWORD | REG (order[i]) | ASB_REQ_STOP;
      for (uint32_t n = 0; !(REG (order[i]) & ASB_ACK); n++)
	if (n > 100000u)
	  {
	    for (int j = i; j >= 0; j--)	/* put back, newest first */
	      REG (order[j]) = PM_PASSWORD | (REG (order[j]) & ~ASB_REQ_STOP);
	    return ERR (err_gate);
	  }
    }
  return 0;
}

/* Reset pulse in PM_GRAFX, then open the gates, master then slave. */
static oserror *
reset_and_open (void)
{
  oserror *e = stop_bridge ();
  if (e)
    return e;
  uint32_t v = pi4.pm[PM_GRAFX / 4];
  pi4.pm[PM_GRAFX / 4] = PM_PASSWORD | (v & ~PM_V3DRSTN);
  spin (100000);			/* no documented minimum pulse */
  v = pi4.pm[PM_GRAFX / 4];
  pi4.pm[PM_GRAFX / 4] = PM_PASSWORD | v | PM_V3DRSTN;
  REG (ASB_V3D_M_CTRL) = REG (ASB_V3D_M_CTRL) & ~ASB_REQ_STOP;
  for (uint32_t n = 0; REG (ASB_V3D_M_CTRL) & ASB_ACK; n++)
    if (n > 1000000u)
      return ERR (err_open);
  REG (ASB_V3D_S_CTRL) = REG (ASB_V3D_S_CTRL) & ~ASB_REQ_STOP;
  for (uint32_t n = 0; REG (ASB_V3D_S_CTRL) & ASB_ACK; n++)
    if (n > 1000000u)
      return ERR (err_open);
  return 0;
}

oserror *
pi4_map (void)
{
  regs r = {{ 13, V3D_BASE, 0x20000 }};
  oserror *e = swix_regs (OS_Memory, &r);
  if (e)
    return e;
  pi4.v3d = (volatile uint32_t *) r.r[3];
  regs p = {{ 13, PM_BASE, 0x1000 }};
  e = swix_regs (OS_Memory, &p);
  if (e)
    return e;
  pi4.pm = (volatile uint32_t *) p.r[3];
  return 0;
}

/* The firmware's V3D power domain (0.08), as Linux's raspberrypi-power
   driver asks for it before the v3d driver probes: *state = 1 on, 0 off,
   ~0 if the firmware doesn't know the domain. */
oserror *
pi4_domain (uint32_t *state)
{
  uint32_t echo;
  *state = ~0u;
  return mailbox (TAG_GET_DOMAIN_STATE, DOMAIN_V3D, ~0u, 0, &echo, state);
}

oserror *
pi4_set_domain (uint32_t on)
{
  uint32_t echo, state;
  return mailbox (TAG_SET_DOMAIN_STATE, DOMAIN_V3D, on, 0, &echo, &state);
}

uint32_t
pi4_pm_grafx (void)
{
  return pi4.pm[PM_GRAFX / 4];
}

/* The firmware turns the V3D's power domain off and on again (0.08),
   then the clock, the reset and the gates as at load. Idle only. */
oserror *
pi4_power_cycle (void)
{
  oserror *e = stop_bridge ();
  if (e)
    return e;
  e = pi4_set_domain (0);
  if (!e)
    {
      spin (1000000);
      e = pi4_set_domain (1);
    }
  if (e)
    return e;
  uint32_t echo, state, rate;
  e = mailbox (TAG_SET_CLOCK_STATE, CLOCK_V3D, 1, 0, &echo, &state);
  if (!e)
    e = mailbox (TAG_GET_CLOCK_RATE, CLOCK_V3D, 0, 0, &echo, &rate);
  if (!e && !rate)
    e = pi4_set_clock (0, &rate);
  return e;
}

/* Power the V3D up, unless it is up already (a reload: never reset it). */
oserror *
pi4_power_on (int *was_up)
{
  /* (0.20) The firmware's power domain BEFORE any V3D register is read.
     pi4_v3d_is_up reads the core's IDENT0, and a read from a V3D whose
     power domain is off can end in an asynchronous external abort,
     reported later at whatever code is running ("Internal abort at
     various locations": a CM4 at start-up, 21 Nov 2026). A domain the
     firmware reports off was never turned on by V3DRender (it turns it
     on at load and leaves it on), so nothing of ours is running on it
     and asking for it now is safe. A firmware without the domain
     interface answers ~0, and nothing changes. */
  uint32_t dom;
  if (!pi4_domain (&dom) && dom == 0)
    {
      pi4_set_domain (1);
      spin (100000);
    }
  *was_up = pi4_v3d_is_up ();
  if (*was_up)
    return 0;
  /* the firmware's clock first: a gated V3D takes register writes but
     its control lists never run */
  uint32_t echo, state, rate;
  oserror *e = mailbox (TAG_GET_CLOCK_STATE, CLOCK_V3D, 0, 0, &echo, &state);
  if (!e && !(state & 1))
    e = mailbox (TAG_SET_CLOCK_STATE, CLOCK_V3D, 1, 0, &echo, &state);
  if (!e)
    e = mailbox (TAG_GET_CLOCK_RATE, CLOCK_V3D, 0, 0, &echo, &rate);
  if (!e && !rate)
    e = pi4_set_clock (0, &rate);
  if (e)
    return e;
  return reset_and_open ();
}

/* the core's reset line (Linux: reset_control_reset on the pm's
   BCM2835_RESET_V3D). Interrupts are off; nothing here calls a SWI. */
void
plat_gpu_reset (struct v3d_dev *v3d)
{
  oserror *e = reset_and_open ();
  if (e)
    plat_log (v3d, e->errmess, 0);
}

/* ---- memory ---- */

/* the hang dump (0.06): an ordinary dynamic area, replaced each time */
static int dump_area;
static void *dump_base;
static uint32_t dump_size;

void *
plat_dump_buffer (uint32_t bytes)
{
  pi4_dump_free ();
  regs r = {{ 0, (unsigned) -1, bytes, (unsigned) -1, DA_FLAG_NOT_DRAGGABLE,
	      bytes, 0, 0, (unsigned) "V3DRender hang dump" }};
  if (swix_regs (OS_DynamicArea, &r))
    return 0;
  dump_area = (int) r.r[1];
  dump_base = (void *) r.r[3];
  dump_size = bytes;
  return dump_base;
}

void
pi4_dump_free (void)
{
  if (!dump_area)
    return;
  regs r = {{ 1, (unsigned) dump_area }};
  swix_regs (OS_DynamicArea, &r);
  dump_area = 0;
  dump_base = 0;
  dump_size = 0;
}

void *
pi4_dump (uint32_t *bytes)
{
  *bytes = dump_size;
  return dump_base;
}

oserror *
pmp_create (struct pmp *a, const char *name, uint32_t pages)
{
  regs r = {{ 0, (unsigned) -1, 0, (unsigned) -1,
	      DA_FLAG_NOT_DRAGGABLE | DA_FLAG_REQUIRES_PAGES | DA_FLAG_IS_PMP,
	      pages * 4096, (unsigned) pmp_handler, (unsigned) -1,
	      (unsigned) name, pages }};
  oserror *e = swix_regs (OS_DynamicArea, &r);
  if (e)
    return e;
  a->area = r.r[1];
  a->base = (uint8_t *) r.r[3];
  a->pages = pages;
  return 0;
}

void
pmp_remove (struct pmp *a)
{
  if (!a->area)
    return;
  regs r = {{ 1, a->area }};
  swix_regs (OS_DynamicArea, &r);
  a->area = 0;
}

/* entries of 3 words, in batches */
#define BATCH 64

static oserror *
pmp_op (struct pmp *a, int reason, uint32_t *list, uint32_t n)
{
  regs r = {{ (unsigned) reason, a->area, (unsigned) list, n }};
  return swix_regs (OS_DynamicArea, &r);
}

static void
pmp_unmap_release (struct pmp *a, uint32_t first, uint32_t n)
{
  uint32_t list[BATCH * 3];
  for (uint32_t done = 0; done < n;)
    {
      uint32_t k = n - done > BATCH ? BATCH : n - done;
      for (uint32_t i = 0; i < k; i++)
	{
	  list[i * 3] = first + done + i;
	  list[i * 3 + 1] = DA_RELEASE;
	  list[i * 3 + 2] = 0;
	}
      pmp_op (a, 22, list, k);		/* unmap */
      pmp_op (a, 21, list, k);		/* release */
      done += k;
    }
}

/* Claim pages first..first+n-1 (any physical pages, or from phys_page
   on if it isn't -1) and map them at the same place. */
oserror *
pmp_claim_map (struct pmp *a, uint32_t first, uint32_t n, uint32_t phys_page, uint32_t ap)
{
  uint32_t list[BATCH * 3];
  for (uint32_t done = 0; done < n;)
    {
      uint32_t k = n - done > BATCH ? BATCH : n - done;
      for (uint32_t i = 0; i < k; i++)
	{
	  list[i * 3] = first + done + i;
	  list[i * 3 + 1] = phys_page == DA_RELEASE ? DA_CLAIM_ANY : phys_page + done + i;
	  list[i * 3 + 2] = PAGE_LOCK;
	}
      oserror *e = pmp_op (a, 21, list, k);
      if (!e)
	{
	  for (uint32_t i = 0; i < k; i++)
	    list[i * 3 + 1] = list[i * 3],
	      list[i * 3 + 2] = (ap & ~PAGE_CACHED_REQ)
		| ((ap & PAGE_CACHED_REQ) ? 0 : PAGE_NOT_CACHEABLE) | PAGE_LOCK;
	  e = pmp_op (a, 22, list, k);
	}
      if (e)
	{
	  pmp_unmap_release (a, first, done + k);
	  return e;
	}
      done += k;
    }
  return 0;
}

uint64_t
pmp_pa (struct pmp *a, uint32_t page)
{
  regs r = {{ 65, (unsigned) (a->base + page * 4096) }};
  if (swix_regs (OS_Memory, &r))
    return 0;
  return r.r[0] | ((uint64_t) r.r[1] << 32);
}

/* Memory above 1 GB first (0.12). The VideoCore reaches only the first
   1 GB of RAM, so ReelHWAccel's H.264 buffers (and VCHIQ's) must come
   from there; the V3D's MMU reaches all of it. So V3DRender asks
   OS_Memory 12 for runs of contiguous pages between 1 GB and 4 GB
   (R4-R7, RISC OS 5.29+; an older RISC OS ignores the range and
   recommends from anywhere, as before), and only when there are none
   lets the kernel pick. Contiguous runs also let the GPU use 64 KB and
   1 MB pages. A Pi with 1 GB has nothing above, and gets what it had. */
#define HIGH_FROM	0x40000000u
#define HIGH_MIN_RUN	16u			/* 64 KB: smaller isn't worth a search */

static uint32_t
high_run (uint32_t pages, uint32_t align_log2)
{
  regs r = {{ 12u | 1u << 9, pages << 12, align_log2, 0,
	      HIGH_FROM, 0, 0xFFFFFFFFu, 0 }};
  if (swix_regs (OS_Memory, &r))
    return DA_RELEASE;
  return r.r[3];
}

int
plat_pages_claim (struct v3d_dev *v3d, uint32_t first, uint32_t n, int cached)
{
  (void) v3d;
  uint32_t ap = PAGE_AP_USER_RW | (cached ? PAGE_CACHED_REQ : 0);
  uint32_t done = 0, run = n, least = n < HIGH_MIN_RUN ? n : HIGH_MIN_RUN;
  while (done < n && run >= least && run)
    {
      if (run > n - done)
	run = n - done;
      /* (0.13) physically aligned as the GPU address is, so the GPU's MMU
	 can use 1 MB and 64 KB entries (the core aligns big BOs' GPU
	 addresses); else any 4 KB run */
      uint32_t at = first + done, align = 12;
      if (run >= 256 && !(at & 255))
	align = 20;
      else if (run >= 16 && !(at & 15))
	align = 16;
      uint32_t page = high_run (run, align);
      if (page == DA_RELEASE && align > 12)
	page = high_run (run, 12);
      if (page == DA_RELEASE
	  || pmp_claim_map (&pi4.bo, first + done, run, page, ap))
	{
	  run /= 2;				/* (a failed claim released its own) */
	  continue;
	}
      done += run;
    }
  if (done < n)
    {
      if (pmp_claim_map (&pi4.bo, first + done, n - done, DA_RELEASE, ap))
	{
	  if (done)
	    pmp_unmap_release (&pi4.bo, first, done);
	  return -1;
	}
    }
  return 0;
}

/* (0.11) OS_Memory 65: a logical address's physical address (64-bit) */
uint64_t
plat_virt_pa (uintptr_t va)
{
  regs r = {{ 65, (unsigned) va }};
  if (swix_regs (OS_Memory, &r))
    return 0;
  return r.r[0] | ((uint64_t) r.r[1] << 32);
}

void cache_clean_range (const void *start, const void *end);
void cache_inval_range (const void *start, const void *end);

/* (0.15) Above the kernel's own threshold (its Cache_RangeThreshold
   ARMop), the whole data cache is cleaned (and invalidated) by set/way
   with the kernel's ARMops instead of line by line: a 1080p frame is
   131,072 lines, against about 17,000 in the Pi 4's L1 and L2. YSFlight
   spent most of each frame here (0.14). Older RISC OS (no OS_MMUControl
   2): line by line, as before. */
typedef void armop_fn (void);
static armop_fn *armop_clean_all, *armop_clean_inval_all;
static uint32_t cache_threshold;
uint32_t pi4_cache_whole;		/* times the whole cache was done */

static armop_fn *
armop (unsigned index)
{
  regs r = {{ 2u | index << 8 }};
  if (swix_regs (OS_MMUControl, &r))
    return 0;
  return (armop_fn *) r.r[0];
}

void
pi4_cache_init (void)
{
  armop_fn *threshold = armop (3);
  armop_clean_all = armop (1);
  armop_clean_inval_all = armop (0);
  if (!threshold || !armop_clean_all || !armop_clean_inval_all)
    {
      armop_clean_all = armop_clean_inval_all = 0;
      return;
    }
  uint32_t (*get) (void) = (uint32_t (*) (void)) threshold;
  cache_threshold = get ();
  if (!cache_threshold || cache_threshold > (16u << 20))
    cache_threshold = 1u << 20;
}

uint32_t
pi4_cache_threshold (void)
{
  return armop_clean_all ? cache_threshold : 0;
}

void
plat_cache_clean (const void *p, uint32_t bytes)
{
  if (armop_clean_all && bytes >= cache_threshold)
    {
      armop_clean_all ();
      pi4_cache_whole++;
      return;
    }
  cache_clean_range (p, (const char *) p + bytes);
}

/* (0.14) clean and invalidate, not just invalidate: memory imported from
   the program may be written by the CPU elsewhere while the GPU works
   (a cached screen: other tasks redraw their windows). Invalidating would
   throw those writes away; cleaning lines the GPU wrote changes nothing,
   as they are clean since the submit. */
void
plat_cache_inval (const void *p, uint32_t bytes)
{
  if (armop_clean_inval_all && bytes >= cache_threshold)
    {
      armop_clean_inval_all ();
      pi4_cache_whole++;
      return;
    }
  cache_inval_range (p, (const char *) p + bytes);
}

/* (0.25) A new BO's pages zeroed in 64-byte bursts of STM: the mapping
   is usually Normal non-cacheable, where each burst goes to memory as
   whole write-buffer lines (memset's single words did not). */
void
plat_zero_pages (void *p, uint32_t bytes)
{
  uint32_t *w = p, *end = (uint32_t *) ((uint8_t *) p + bytes);
  if (!bytes)
    return;
  __asm__ volatile ("mov r2, #0\n\tmov r3, #0\n\tmov r4, #0\n\tmov r5, #0\n\t"
		    "mov r6, #0\n\tmov r7, #0\n\tmov r8, #0\n\tmov r12, #0\n"
		    "1:\tstmia %0!, {r2-r8, r12}\n\t"
		    "stmia %0!, {r2-r8, r12}\n\t"
		    "cmp %0, %1\n\t"
		    "blo 1b"
		    : "+r" (w)
		    : "r" (end)
		    : "r2", "r3", "r4", "r5", "r6", "r7", "r8", "r12", "cc", "memory");
}

void
plat_pages_release (struct v3d_dev *v3d, uint32_t first, uint32_t n)
{
  (void) v3d;
  pmp_unmap_release (&pi4.bo, first, n);
}

uint64_t
plat_page_pa (struct v3d_dev *v3d, uint32_t page)
{
  (void) v3d;
  return pmp_pa (&pi4.bo, page);
}

ERROR_BLOCK (err_pt, 0x823E09, "There isn't 4 MB of contiguous memory for the GPU's page table");

/* The BO area (0.09): pi4_area_mb (the Size load option or
   V3DRender$Size, 256 MB if neither), halved down to 16 MB while the
   address space can't take it. Only address space: pages are claimed
   for BOs as they are made. The page table: 4 MB contiguous (OS_Memory
   12), and a scratch page. */
oserror *
pi4_memory (struct v3d_dev *v3d)
{
  oserror *e = 0;
  for (uint32_t mb = pi4_area_mb; mb >= 16; mb /= 2)
    if (!(e = pmp_create (&pi4.bo, "V3DRender buffers", mb * 256)))
      break;
  if (e)
    return e;
  e = pmp_create (&pi4.pt, "V3DRender page table", 1025);
  if (e)
    return e;
  /* above 1 GB if there is room (0.12), else anywhere */
  uint32_t page = high_run (1024, 12);
  if (page == DA_RELEASE)
    {
      regs r = {{ 12, 4u << 20, 12 }};
      if (swix_regs (OS_Memory, &r))
	return ERR (err_pt);
      page = r.r[3];
    }
  e = pmp_claim_map (&pi4.pt, 0, 1024, page, PAGE_AP_PRIVILEGED);
  if (!e)
    e = pmp_claim_map (&pi4.pt, 1024, 1, DA_RELEASE, PAGE_AP_PRIVILEGED);
  if (e)
    return e;
  memset (pi4.pt.base, 0, 1025 * 4096);
  v3d->pt = (uint32_t *) pi4.pt.base;
  v3d->pt_pa = pmp_pa (&pi4.pt, 0);
  v3d->scratch_pa = pmp_pa (&pi4.pt, 1024);
  /* (OS_Memory 12 promised contiguous pages: check it) */
  if (pmp_pa (&pi4.pt, 1023) != v3d->pt_pa + 1023 * 4096)
    return ERR (err_pt);
  v3d->area = pi4.bo.base;
  v3d->area_pages = pi4.bo.pages;
  return 0;
}

void
pi4_memory_free (void)
{
  if (pi4.pt.area)
    pmp_unmap_release (&pi4.pt, 0, 1025);
  pmp_remove (&pi4.pt);
  pmp_remove (&pi4.bo);
}

/* ---- the rest of v3d_plat.h ---- */

uint32_t
plat_time_cs (void)
{
  regs r = {{ 0 }};
  swix_regs (OS_ReadMonotonicTime, &r);
  return r.r[0];
}

/* (0.13) the ARM's generic timer (CNTVCT; 54 MHz on a Pi 4, from the
   firmware's CNTFRQ), readable with interrupts off and in the interrupt
   handler; centiseconds if CNTFRQ was never set */
uint64_t
plat_time_ns (void)
{
  static uint32_t freq;
  if (!freq)
    {
      uint32_t f;
      __asm__ volatile ("mrc p15, 0, %0, c14, c0, 0" : "=r" (f));
      freq = f ? f : ~0u;
    }
  if (freq == ~0u)
    return (uint64_t) plat_time_cs () * 10000000u;
  uint32_t lo, hi;
  __asm__ volatile ("isb\n\tmrrc p15, 1, %0, %1, c14" : "=r" (lo), "=r" (hi));
  uint64_t t = (uint64_t) hi << 32 | lo;
  return t / freq * 1000000000u + t % freq * 1000000000u / freq;
}

void
plat_barrier (void)
{
  __asm__ volatile ("dsb sy" ::: "memory");
}

unsigned
plat_irq_off (void)
{
  unsigned cpsr;
  __asm__ volatile ("mrs %0, cpsr\n\tcpsid i" : "=r" (cpsr) :: "memory");
  return cpsr;
}

void
plat_irq_restore (unsigned cpsr)
{
  if (!(cpsr & 0x80))
    __asm__ volatile ("cpsie i" ::: "memory");
}

/* Waits for an interrupt (the GPU's, or the 100 Hz timer's) with
   interrupts on. */
int pi4_no_wfi;
uint32_t pi4_area_mb = 256;

/* where the page table is (for *V3DRender) */
uint64_t
pi4_pt_pa (void)
{
  return pi4.pt.area ? pmp_pa (&pi4.pt, 0) : 0;
}
void
plat_wait (void)
{
  unsigned s = plat_irq_off ();
  __asm__ volatile ("cpsie i" ::: "memory");
  if (!pi4_no_wfi)
    __asm__ volatile ("dsb sy\n\twfi" ::: "memory");
  if (s & 0x80)
    __asm__ volatile ("cpsid i" ::: "memory");
}

void *
plat_alloc (size_t bytes)
{
  void *p = rma_claim (bytes);
  if (p)
    memset (p, 0, bytes);
  return p;
}

void
plat_free (void *p)
{
  rma_free (p);
}

/* the last few messages, for *V3DRender (safe with interrupts off) */
struct pi4_log pi4_log[PI4_LOG];
unsigned pi4_log_next;

void
plat_log (struct v3d_dev *v3d, const char *msg, uint32_t value)
{
  (void) v3d;
  struct pi4_log *l = &pi4_log[pi4_log_next++ % PI4_LOG];
  l->msg = msg;
  l->value = value;
}
