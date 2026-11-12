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

ERROR_BLOCK (err_gate, 0x81D347, "A V3D bus gate didn't stop, so the GPU isn't being reset: restart the computer");
ERROR_BLOCK (err_open, 0x81D348, "The V3D's bus gates didn't open: restart the computer");

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

/* Power the V3D up, unless it is up already (a reload: never reset it). */
oserror *
pi4_power_on (int *was_up)
{
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
	    list[i * 3 + 1] = list[i * 3], list[i * 3 + 2] = ap | PAGE_NOT_CACHEABLE | PAGE_LOCK;
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

int
plat_pages_claim (struct v3d_dev *v3d, uint32_t first, uint32_t n)
{
  (void) v3d;
  return pmp_claim_map (&pi4.bo, first, n, DA_RELEASE, PAGE_AP_USER_RW) ? -1 : 0;
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

ERROR_BLOCK (err_pt, 0x81D349, "There isn't 4 MB of contiguous memory for the GPU's page table");

/* The BO area: as big as the address space allows, up to 1 GB. The page
   table: 4 MB contiguous (OS_Memory 12), and a scratch page. */
oserror *
pi4_memory (struct v3d_dev *v3d)
{
  oserror *e = 0;
  for (uint32_t mb = 1024; mb >= 128; mb /= 2)
    if (!(e = pmp_create (&pi4.bo, "V3DRender buffers", mb * 256)))
      break;
  if (e)
    return e;
  e = pmp_create (&pi4.pt, "V3DRender page table", 1025);
  if (e)
    return e;
  regs r = {{ 12, 4u << 20, 12 }};
  if (swix_regs (OS_Memory, &r))
    return ERR (err_pt);
  e = pmp_claim_map (&pi4.pt, 0, 1024, r.r[3], PAGE_AP_PRIVILEGED);
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
