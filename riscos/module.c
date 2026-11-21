/* module.c: V3DRender, the RISC OS module.

   SWIs (chunk &5AFC0, allocated 18 Nov 2026; 0.15 and before: &C3D40):
     V3DRender_Info   -> r0 = version * 100, r1 = V3D version (42),
			 r2 = HUB_IDENT1, r3 = clock (Hz), r4 = buffer
			 memory (bytes), r5 = of which in use, r6 = files
			 open, r7 -> the DRM driver's name ("v3d"),
			 r8 -> the statistics (struct v3d_stats in
			 core/v3d.h: words, read only, for tests)
     V3DRender_Open   r0 = flags (0) -> r0 = file (from 1)
     V3DRender_Close  r0 = file
     V3DRender_Ioctl  r0 = file, r1 = request, r2 -> its structure
			 -> r0 = 0, or a Linux errno negated (no error is
			 raised for those: libdrm gives them to Mesa)
     V3DRender_Reset  resets the GPU (jobs running are lost)

   A file is what Linux calls an open DRM file: its own BO handles and
   sync objects. Files are closed when the Wimp task that opened them
   ends, and RISC3D's contexts on this driver are files too.

   RISC3D: the module registers as a driver of the "DRM device" kind
   (RISC3D_Register r4 bit 0): a context is a file here, and the
   program's Mesa (its v3d driver, through libdrm) sends the work
   straight to V3DRender_Ioctl, without RISC3D's command stream.

   SPDX-License-Identifier: GPL-2.0-or-later
   Copyright (c) 2026 Andrew Youll */
#include "cmodule.h"
#include "v3d.h"
#include "v3d_regs.h"
#include "plat_pi4.h"
#include "twod.h"

#define VERSION 23		/* 0.23 */
#define SWI_BASE 0x5AFC0
#define OS_ClaimDeviceVector	0x4B
#define OS_ReleaseDeviceVector	0x4C
#define Wimp_ReadSysInfo	0x400F2
#define OS_SWINumberFromString	0x39
#define IRQ_DEVICE 10				/* iDev_GPU_VC3D */
#define HAL_IRQEnable 1
#define HAL_IRQDisable 2
#define HAL_IRQClear 3

void irq_entry (void);
void driver_entry (void);
void call_hal (unsigned arg, void *routine, unsigned sb);
const char *v3d_axi_client (uint32_t id);

static struct v3d_dev *dev;
static int twod_wanted;			/* (0.17) 2D at load: TWOD_* */
static void *private_word;
static void *hal_irqclear;
static unsigned hal_sb;
static int vector_claimed, registered, mapped_up, clock_raised;
static uint32_t clock_before;

ERROR_BLOCK (err_swi, 0x1E6, "SWI value out of range for module V3DRender");
ERROR_BLOCK (err_file, 0x823E00, "Not a V3DRender file");
ERROR_BLOCK (err_notpi4, 0x823E01, "V3DRender needs a Raspberry Pi 4, Pi 400 or Compute Module 4");
ERROR_BLOCK (err_v3ddriver, 0x823E02, "V3DDriver is loaded: V3DRender can't share the GPU with it (*RMKill V3DDriver first)");
ERROR_BLOCK (err_nogpu, 0x823E03, "The GPU didn't answer as a V3D 4.2");
ERROR_BLOCK (err_files, 0x823E04, "Too many V3DRender files are open");
ERROR_BLOCK (err_mem, 0x823E05, "Not enough memory for V3DRender");
ERROR_BLOCK (err_syntax, 0x823E0B, "Syntax: *V3DRender [2D on|async|off]");
ERROR_BLOCK (err_sync, 0x823E06, "V3DRender takes its work through V3DRender_Ioctl, not RISC3D_Sync");

static unsigned
this_task (void)
{
  regs r = {{ 5 }};
  return swix_regs (Wimp_ReadSysInfo, &r) ? 0 : r.r[0];
}

static oserror *
hal (unsigned entry)
{
  regs r = {{ IRQ_DEVICE, 0, 0, 0, 0, 0, 0, 0, 0, entry }};
  return swix_regs (OS_Hardware, &r);
}

/* the interrupt (from irq_entry) */
void
irq_c (void)
{
  if (dev)
    v3d_irq (dev);
  call_hal (IRQ_DEVICE, hal_irqclear, hal_sb);
}

/* (0.22) why the screen can't be used, as text (a switch: the module
   isn't relocated, so no tables of pointers) */
static const char *
twod_why_text (uint32_t why)
{
  switch (why)
    {
    case 1: return "VDU variables unreadable";
    case 2: return "screen not 16 or 32bpp";
    case 3: return "screen geometry inconsistent";
    case 4: return "no framestore from GraphicsV 9";
    case 5: return "VDU and display banks differ";
    case 6: return "framestore not mappable";
    case 7: return "screen cached by the CPU";
    }
  return "?";
}

/* (0.12) BO pages (not imported ones) above and below 1 GB, and in
   1 MB and 64 KB GPU MMU entries */
static void
count_pages (uint32_t *high, uint32_t *low, uint32_t *big, uint32_t *super)
{
  *high = *low = *big = *super = 0;
  for (int bo = 1; bo < V3D_MAX_BOS; bo++)
    {
      struct v3d_bo *b = &dev->bos[bo];
      if (!b->used || b->imported)
	continue;
      for (uint32_t i = 0; i < b->npages; i++)
	{
	  uint32_t pte = dev->pt[b->first + i];
	  if (!(pte & (1u << 28)))
	    continue;
	  if ((pte & 0x0FFFFFFFu) >= 0x40000u)	/* page frame of 1 GB */
	    (*high)++;
	  else
	    (*low)++;
	  *super += (pte >> 31) & 1;		/* (0.13) in 1 MB entries */
	  *big += (pte >> 30) & 1;		/* and 64 KB ones */
	}
    }
}

/* (0.20) RISC3D's reason 4, "hardware": lines of label TAB value LF,
   for Configure's RISC3D window (RISC3D 0.09, DRIVER-GUIDE §5) */
struct text
{
  char *p, *end;
  uint32_t need;
};

static void
tput (struct text *t, const char *s)
{
  for (; *s; s++)
    {
      if (t->p < t->end)
	*t->p++ = *s;
      t->need++;
    }
}

static void
tdec (struct text *t, uint32_t v)
{
  char n[12];
  int i = 11;
  n[i] = 0;
  do
    {
      n[--i] = (char) ('0' + v % 10);
      v /= 10;
    }
  while (v && i);
  tput (t, n + i);
}

static void
hardware_text (char *buf, uint32_t size, uint32_t *need)
{
  struct text t = { buf, buf + (size ? size - 1 : 0), 0 };
  tput (&t, "GPU\tV3D ");
  tdec (&t, (unsigned) dev->ver / 10);
  tput (&t, ".");
  tdec (&t, (unsigned) dev->ver % 10);
  tput (&t, ", revision ");
  tdec (&t, (unsigned) dev->rev);
  tput (&t, " (the Pi 4's VideoCore VI)\n");
  uint32_t hz = 0, max = 0;
  if (!pi4_clock (&hz, &max))
    {
      tput (&t, "Clock\t");
      tdec (&t, hz / 1000000);
      tput (&t, " MHz now (up to ");
      tdec (&t, max / 1000000);
      tput (&t, " MHz, raised while programs draw)\n");
    }
  uint32_t high, low, big, super;
  count_pages (&high, &low, &big, &super);
  tput (&t, "GPU memory\t");
  tdec (&t, dev->pages_used * 4);
  tput (&t, " KB in use");
  if (!low)
    tput (&t, ", all above 1 GB");
  else
    {
      tput (&t, " (");
      tdec (&t, low * 4);
      tput (&t, " KB below 1 GB)");
    }
  tput (&t, ", of ");
  tdec (&t, dev->area_pages / 256);
  tput (&t, " MB\n");
  tput (&t, "Largest image\t4096 x 4096 (textures and render targets)\n");
  tput (&t, "Desktop 2D\t");
  tput (&t, twod.mode == TWOD_OFF ? "off (*V3DRender 2D on)" : twod.mode == TWOD_ASYNC
	? "on, not waited for" : "on: window moves, scrolls and fills on the GPU");
  tput (&t, "\n");
  if (size)
    *t.p = 0;
  *need = t.need + 1;
}

/* ---- RISC3D ---- */

/* (0.16) RISC3D_Register is found by name: RISC3D's chunk moved from
   the provisional &C3D00 to its allocated &5AF80, and either RISC3D may
   be loaded. The number used to register is kept for deregistering. */
static unsigned risc3d_register;

static void
register_risc3d (void)
{
  if (registered)
    return;
  regs n = {{ 0, (unsigned) "RISC3D_Register" }};
  if (swix_regs (OS_SWINumberFromString, &n))
    return;				/* RISC3D isn't loaded */
  risc3d_register = n.r[0];
  /* flags: bit 0 a DRM device; (0.20) bit 3 answers reason 4, hardware */
  regs r = {{ 0, (unsigned) driver_entry, 0, (unsigned) "V3DRender (the Pi 4's GPU)",
	      1 | 8, SWI_BASE + 3, (unsigned) "v3d" }};
  registered = !swix_regs (risc3d_register, &r);
}

static void
deregister_risc3d (void)
{
  if (!registered)
    return;
  regs r = {{ 1, (unsigned) driver_entry, 0 }};
  swix_regs (risc3d_register, &r);
  registered = 0;
}

static oserror *
file_open (unsigned *handle)
{
  v3d_service (dev);
  int f = v3d_open (dev, this_task ());
  if (f < 0)
    return f == -V3D_ENOMEM ? ERR (err_mem) : ERR (err_files);
  /* full speed while anything is open (Linux raises the clock while
     jobs run, and lowers it after a while idle) */
  if (!clock_raised && v3d_idle (dev))
    {
      uint32_t now;
      if (!pi4_clock (&clock_before, 0) && !pi4_set_clock (0, &now))
	clock_raised = 1;
    }
  *handle = (unsigned) f + 1;
  return 0;
}

static void
file_close (int f)
{
  v3d_close (dev, f);
  for (int i = 0; i < V3D_MAX_FILES; i++)
    if (dev->files[i].used && i != dev->twod.file)
      return;
  if (clock_raised && clock_before && v3d_idle (dev))
    {
      uint32_t now;
      pi4_set_clock (clock_before, &now);
      clock_raised = 0;
    }
}

/* (0.10) Recovery after a program failed: the GPU reset, every queued
   and running job ended (their fences signalled as failed), and with
   all, every file closed with its buffers and sync objects. */
static oserror *
recover (int all, int report)
{
  v3d_cancel_all (dev);
  int closed = 0;
  if (all)
    {
      for (int f = 0; f < V3D_MAX_FILES; f++)
	if (dev->files[f].used)
	  {
	    closed += f != dev->twod.file;
	    file_close (f);
	  }
      twod_files_closed ();		/* (0.17) its own file again */
    }
  if (report)
    {
      write0 ("The GPU was reset and its work ended.");
      if (all)
	{
	  write0 (" Files closed: ");
	  char n[12];
	  int i = 11;
	  n[i] = 0;
	  do
	    n[--i] = (char) ('0' + closed % 10);
	  while ((closed /= 10) && i);
	  write0 (n + i);
	}
      newline ();
    }
  return 0;
}

/* RISC3D's calls: r0 = 0 open -> r0 = our file; 1 close r1 = file;
   2 sync: not for this kind of driver; 3 describe (RISC3D 0.03, asked
   only of drivers registered with flags bit 1): r0 = 0, on a GPU;
   4 hardware (0.20; RISC3D 0.09, flags bit 3): r1 -> buffer, r2 = size
   -> r2 = size needed, the text ended with a 0 */
oserror *
driver_call (regs *r)
{
  switch (r->r[0])
    {
    case 0:
      {
	unsigned h;
	oserror *e = file_open (&h);
	if (e)
	  return e;
	r->r[0] = h;
	r->r[1] = 0;
	r->r[2] = 0;
	return 0;
      }
    case 1:
      file_close ((int) r->r[1] - 1);
      return 0;
    case 3:				/* describe (RISC3D 0.03): on a GPU */
      r->r[0] = 0;
      return 0;
    case 4:				/* (0.20) hardware (RISC3D 0.09) */
      {
	uint32_t need;
	v3d_service (dev);
	hardware_text ((char *) r->r[1], r->r[2], &need);
	r->r[2] = need;
	return 0;
      }
    }
  return ERR (err_sync);
}

/* ---- the module ---- */

/* (0.23) Pages the OS is moving (Service_PagesUnsafe(64) / PagesSafe(64)):
   page blocks of 3 words (page number, logical, physical) or 5 (page
   number, logical low/high, physical low/high). A 64-bit pair surrounds
   the 32-bit one, which then lists only some of the same pages. */
static int in64;

static uint64_t
block_pa (const uint32_t *e, int words)
{
  return words == 5 ? (uint64_t) e[3] | (uint64_t) e[4] << 32 : (uint64_t) e[2];
}

static void
pages_unsafe (const uint32_t *blk, uint32_t n, int words)
{
  if (!n || !blk)
    return;
  uint64_t *pa = plat_alloc (n * sizeof *pa);
  if (!pa)
    {
      dev->hold = 1;			/* can't tell: hold everything until safe */
      return;
    }
  for (uint32_t i = 0; i < n; i++)
    pa[i] = block_pa (blk + i * words, words);
  v3d_pages_unsafe (dev, pa, n);
  plat_free (pa);
}

static void
pages_safe (const uint32_t *before, const uint32_t *after, uint32_t n, int words)
{
  uint64_t *pa = n ? plat_alloc (2 * n * sizeof *pa) : 0;
  if (!pa)
    {
      v3d_pages_safe (dev, 0, 0, 0);	/* at least start jobs again */
      return;
    }
  for (uint32_t i = 0; i < n; i++)
    {
      pa[i] = block_pa (before + i * words, words);
      pa[n + i] = block_pa (after + i * words, words);
    }
  v3d_pages_safe (dev, pa, pa + n, n);
  plat_free (pa);
}

oserror *
mod_swi (unsigned number, regs *r)
{
  int f = (int) r->r[0] - 1;
  switch (number)
    {
    case 0:
      {
	uint32_t hz = 0;
	pi4_clock (&hz, 0);
	int n = 0;
	for (int i = 0; i < V3D_MAX_FILES; i++)
	  n += dev->files[i].used && i != dev->twod.file;
	r->r[0] = VERSION;
	r->r[1] = (unsigned) dev->ver;
	r->r[2] = dev->ident[1];
	r->r[3] = hz;
	r->r[4] = dev->area_pages * V3D_PAGE;
	r->r[5] = dev->pages_used * V3D_PAGE;
	r->r[6] = (unsigned) n;
	r->r[7] = (unsigned) "v3d";
	r->r[8] = (unsigned) &dev->stats;	/* read only: struct v3d_stats */
	return 0;
      }
    case 1:
      return file_open (&r->r[0]);
    case 2:
      if (!v3d_file_valid (dev, f))
	return ERR (err_file);
      file_close (f);
      return 0;
    case 3:
      if (!v3d_file_valid (dev, f))
	return ERR (err_file);
      r->r[0] = (unsigned) v3d_ioctl (dev, f, r->r[1], (void *) r->r[2]);
      return 0;
    case 4:
      /* V3DRender_Reset (0.10): every job ended and the GPU reset;
	 r0 bit 0 set: every file closed too (what *V3DReset all does) */
      return recover ((r->r[0] & 1) != 0, 0);
    case 5:
      /* V3DRender_Debug (0.07): r0 = 0: r1 = GPU address -> r0 = its PTE
	 (0: none); r0 = 1: -> r0 = VA width, r1 = PA width (bits) */
      if (r->r[0] == 0)
	{
	  uint32_t page = r->r[1] >> V3D_MMU_PAGE_SHIFT;
	  r->r[0] = page < dev->area_pages ? dev->pt[page] : 0;
	  return 0;
	}
      if (r->r[0] == 1)
	{
	  r->r[0] = (unsigned) dev->va_width;
	  r->r[1] = (unsigned) dev->pa_width;
	  return 0;
	}
      if (r->r[0] == 2)
	{
	  /* (0.13) r1 = a GPU address: the next BO made starts there if
	     that space is free (the kit's replay of a *V3DDump) */
	  dev->place_at = r->r[1] >> V3D_MMU_PAGE_SHIFT;
	  return 0;
	}
      return ERR (err_swi);
    }
  return ERR (err_swi);
}

void
mod_service (regs *r)
{
  if (!dev)
    return;
  switch (r->r[1])
    {
    case 0x53:				/* Service_WimpCloseDown: r2 = task */
      for (int f = 0; f < V3D_MAX_FILES; f++)
	if (dev->files[f].used && dev->files[f].owner && dev->files[f].owner == r->r[2])
	  file_close (f);
      break;
    case 0x45:				/* Service_PreReset */
      v3d_irq_disable (dev);
      break;
    case 0xE3:				/* (0.23) Service_PagesUnsafe64 */
      pages_unsafe ((const uint32_t *) r->r[2], r->r[3], 5);
      in64 = 1;
      break;
    case 0x8E:				/* Service_PagesUnsafe: inside a 64 one, a subset */
      if (!in64)
	pages_unsafe ((const uint32_t *) r->r[2], r->r[3], 3);
      break;
    case 0x8F:				/* Service_PagesSafe */
      if (!in64)
	pages_safe ((const uint32_t *) r->r[3], (const uint32_t *) r->r[4], r->r[2], 3);
      break;
    case 0xE4:				/* Service_PagesSafe64 */
      pages_safe ((const uint32_t *) r->r[3], (const uint32_t *) r->r[4], r->r[2], 5);
      in64 = 0;
      break;
    case 0x4D:				/* Service_PreModeChange (0.17) */
      twod_mode_changing ();
      break;
    case 0x46:				/* Service_ModeChange */
      twod_mode_changed ();
      break;
    case 0xDA:				/* Service_ModulePostInit: r2 -> title */
      {
	const char *t = (const char *) r->r[2], *want = "RISC3D";
	int i = 0;
	while (t && want[i] && t[i] == want[i])
	  i++;
	if (t && !want[i] && t[i] < ' ')
	  {
	    registered = 0;			/* a new RISC3D knows nothing of us */
	    register_risc3d ();
	  }
	break;
      }
    }
}

static int
module_present (const char *name)
{
  regs r = {{ 18, (unsigned) name }};
  return !swix_regs (OS_Module, &r);
}

oserror *
mod_final (int fatal)
{
  (void) fatal;
  deregister_risc3d ();
  twod_final ();			/* (0.17) GraphicsV released first */
  if (dev)
    {
      v3d_fini (dev);
      /* the next driver (V3DDriver, say) may expect the MMU off */
      v3d_wr (dev, V3D_BANK_HUB, V3D_MMU_CTL, 0);
      v3d_wr (dev, V3D_BANK_HUB, V3D_MMUC_CONTROL, 0);
    }
  if (vector_claimed)
    {
      hal (HAL_IRQDisable);
      regs r = {{ IRQ_DEVICE, (unsigned) irq_entry, (unsigned) private_word }};
      swix_regs (OS_ReleaseDeviceVector, &r);
      vector_claimed = 0;
    }
  if (dev && clock_raised && clock_before)
    {
      uint32_t now;
      pi4_set_clock (clock_before, &now);
    }
  pi4_memory_free ();
  pi4_dump_free ();
  plat_free (dev);
  dev = 0;
  return 0;
}

static uint32_t parse_dec (const char *s);

/* a / b without the library's division (it needs raise) */
static uint32_t
div32 (uint32_t a, uint32_t b)
{
  uint32_t q = 0, r = 0;
  if (!b)
    return 0;
  for (int i = 31; i >= 0; i--)
    {
      r = r << 1 | ((a >> i) & 1);
      if (r >= b)
	{
	  r -= b;
	  q |= 1u << i;
	}
    }
  return q;
}

oserror *
mod_init (const char *tail, int instance, void *pw)
{
  (void) instance;
  private_word = pw;
  while (tail && *tail == ' ')
    tail++;
  /* load options (0.09): NoWFI, Size <MB>; else V3DRender$Size (MB) */
  pi4_no_wfi = 0;
  pi4_area_mb = 0;
  twod_wanted = TWOD_OFF;
  for (const char *t = tail; t && *t >= ' ';)
    {
      while (*t == ' ')
	t++;
      if (*t == '-')
	t++;
      if ((t[0] | 32) == 'n' && (t[1] | 32) == 'o')
	pi4_no_wfi = 1;
      else if (t[0] == '2' && (t[1] | 32) == 'd')
	twod_wanted = (t[2] | 32) == 'a' || (t[3] | 32) == 'a' ? TWOD_ASYNC : TWOD_ON;
      else if ((t[0] | 32) == 's' && (t[1] | 32) == 'i' && (t[2] | 32) == 'z' && (t[3] | 32) == 'e')
	{
	  t += 4;
	  while (*t == ' ' || *t == '=')
	    t++;
	  pi4_area_mb = parse_dec (t);
	}
      while (*t > ' ')
	t++;
    }
  if (!pi4_area_mb)
    {
      char buf[16];
      regs r = {{ (unsigned) "V3DRender$Size", (unsigned) buf, sizeof buf - 1, 0, 0 }};
      if (!swix_regs (OS_ReadVarVal, &r))
	{
	  buf[r.r[2] < sizeof buf ? r.r[2] : sizeof buf - 1] = 0;
	  pi4_area_mb = parse_dec (buf);
	}
    }
  if (!twod_wanted)
    {
      /* (0.17) V3DRender$2D: on, async or off */
      char buf[8];
      regs r = {{ (unsigned) "V3DRender$2D", (unsigned) buf, sizeof buf - 1, 0, 0 }};
      if (!swix_regs (OS_ReadVarVal, &r))
	{
	  buf[r.r[2] < sizeof buf ? r.r[2] : sizeof buf - 1] = 0;
	  twod_wanted = (buf[0] | 32) == 'a' ? TWOD_ASYNC
	    : (buf[0] | 32) == 'o' && (buf[1] | 32) == 'n' ? TWOD_ON : TWOD_OFF;
	}
    }
  if (!pi4_area_mb)
    pi4_area_mb = 256;
  if (pi4_area_mb < 16)
    pi4_area_mb = 16;
  if (pi4_area_mb > 2048)
    pi4_area_mb = 2048;

  if (module_present ("V3DDriver"))
    return ERR (err_v3ddriver);
  if (pi4_soc () != 3)
    return ERR (err_notpi4);
  oserror *e = pi4_map ();
  if (e)
    return e;
  e = pi4_power_on (&mapped_up);
  if (e)
    return e;

  dev = plat_alloc (sizeof *dev);
  if (!dev)
    return ERR (err_mem);
  dev->ver = V3D_GEN_42;
  if (v3d_hw_probe (dev))
    {
      plat_free (dev);
      dev = 0;
      return ERR (err_nogpu);
    }
  pi4_cache_init ();
  e = pi4_memory (dev);
  if (e)
    goto fail;
  if (v3d_init (dev))
    {
      e = ERR (err_mem);
      goto fail;
    }

  regs h = {{ 0, 0, 0, 0, 0, 0, 0, 0, 1, HAL_IRQClear }};	/* OS_Hardware 1 */
  e = swix_regs (OS_Hardware, &h);
  if (e)
    goto fail;
  hal_irqclear = (void *) h.r[0];
  hal_sb = h.r[1];
  regs c = {{ IRQ_DEVICE, (unsigned) irq_entry, (unsigned) pw }};
  e = swix_regs (OS_ClaimDeviceVector, &c);
  if (e)
    goto fail;
  vector_claimed = 1;
  e = hal (HAL_IRQEnable);
  if (e)
    goto fail;

  register_risc3d ();
  twod_init (dev, pw);
  if (twod_wanted != TWOD_OFF)
    twod_set (twod_wanted);		/* (0.17) the desktop's copies */
  return 0;

fail:
  mod_final (1);
  return e;
}

/* ---- *commands ---- */

static void
line (const char *label, uint32_t v)
{
  write0 (label);
  write_dec (v);
  newline ();
}

static void
hex (const char *label, uint32_t v)
{
  write0 (label);
  write0 ("&");
  write_hex8 (v);
}

/* the registers when a job was found hung (0.05) */
static void
show_hang (const char *what, const struct v3d_hang *h)
{
  if (h->q == ~0u)
    return;
  write0 (what);
  write0 (": ");
  /* (a switch, not a table of pointers: the module isn't relocated) */
  switch (h->q)
    {
    case V3D_BIN: write0 ("bin"); break;
    case V3D_RENDER: write0 ("render"); break;
    case V3D_TFU: write0 ("TFU"); break;
    case V3D_CSD: write0 ("compute"); break;
    default: write0 ("?"); break;
    }
  write0 (" job");
  if (h->start || h->end)
    {
      hex (", list ", h->start);
      hex (" to ", h->end);
    }
  newline ();
  hex ("  CT0 CS ", h->ct0cs); hex (" CA ", h->ct0ca); hex (" EA ", h->ct0ea); hex (" RA ", h->ct0ra);
  newline ();
  hex ("  CT1 CS ", h->ct1cs); hex (" CA ", h->ct1ca); hex (" EA ", h->ct1ea); hex (" RA ", h->ct1ra);
  newline ();
  hex ("  PCS ", h->pcs); hex (" BFC ", h->bfc); hex (" RFC ", h->rfc); hex (" ERR_STAT ", h->err_stat);
  newline ();
  hex ("  FDBGO ", h->fdbgo); hex (" FDBGB ", h->fdbgb); hex (" FDBGR ", h->fdbgr); hex (" FDBGS ", h->fdbgs);
  newline ();
  hex ("  BPCA ", h->bpca); hex (" BPCS ", h->bpcs); hex (" BPOA ", h->bpoa); hex (" BPOS ", h->bpos);
  newline ();
  if (h->words_at)
    {
      hex ("  at ", h->words_at);
      write0 (":");
      for (int i = 0; i < 8; i++)
	hex (" ", h->words[i]);
      newline ();
    }
}

static const char *
skip0 (const char *s)
{
  while (*s)
    s++;
  return s + 1;
}

static uint32_t
parse_dec (const char *s)
{
  uint32_t v = 0;
  while (*s == ' ')
    s++;
  while (*s >= '0' && *s <= '9')
    v = v * 10 + (uint32_t) (*s++ - '0');
  return v;
}

oserror *
mod_command (int index, const char *tail, int argc)
{
  if (!dev)
    return 0;
  if (index == 0 && argc > 0)
    {
      /* *V3DRender 2D on|async|off (0.17) */
      while (*tail == ' ')
	tail++;
      if (tail[0] != '2' || (tail[1] | 32) != 'd')
	return ERR (err_syntax);
      tail += 2;
      while (*tail == ' ')
	tail++;
      if ((tail[0] | 32) == 'a' && (tail[1] | 32) == 'l')	/* 2D alongside on|off (0.21) */
	{
	  while (*tail > ' ')
	    tail++;
	  while (*tail == ' ')
	    tail++;
	  if ((tail[0] | 32) != 'o' || ((tail[1] | 32) != 'n' && (tail[1] | 32) != 'f'))
	    return ERR (err_syntax);
	  twod.alongside = (tail[1] | 32) == 'n';
	  return 0;
	}
      if ((tail[0] | 32) == 'z')		/* 2D zero: the counters (0.19) */
	{
	  twod_zero ();
	  return 0;
	}
      if ((tail[0] | 32) == 'l')		/* 2D limits [5 numbers] (0.18) */
	{
	  uint32_t v[5];
	  int n = 0;
	  while (*tail > ' ')
	    tail++;
	  for (;;)
	    {
	      while (*tail == ' ')
		tail++;
	      if (*tail < '0' || *tail > '9')
		break;
	      if (n == 5)
		return ERR (err_syntax);
	      v[n++] = parse_dec (tail);
	      while (*tail > ' ')
		tail++;
	    }
	  if (*tail >= ' ' || (n != 0 && n != 5))
	    return ERR (err_syntax);
	  twod_limits (v, n);
	  return 0;
	}
      if ((tail[0] | 32) == 'f')		/* 2D fills on|off */
	{
	  while (*tail > ' ')
	    tail++;
	  while (*tail == ' ')
	    tail++;
	  if ((tail[0] | 32) != 'o' || ((tail[1] | 32) != 'n' && (tail[1] | 32) != 'f'))
	    return ERR (err_syntax);
	  twod.fills = (tail[1] | 32) == 'n';
	  return 0;
	}
      int mode = (tail[0] | 32) == 'a' ? TWOD_ASYNC
	: (tail[0] | 32) == 'o' && (tail[1] | 32) == 'n' ? TWOD_ON
	: (tail[0] | 32) == 'o' && (tail[1] | 32) == 'f' ? TWOD_OFF : -1;
      if (mode < 0)
	return ERR (err_syntax);
      if (mode != TWOD_OFF && twod.mode != TWOD_OFF)
	twod_set (TWOD_OFF);		/* (from on to async, or back) */
      return twod_set (mode);
    }
  if (index == 2)
    {
      /* *V3DDump <file>: the first hung job's dump (0.06) */
      uint32_t bytes;
      void *d = pi4_dump (&bytes);
      if (!d)
	{
	  write0 ("No job has hung since V3DRender was loaded.");
	  newline ();
	  return 0;
	}
      regs r = {{ 10, (unsigned) tail, 0xFFD, 0, (unsigned) d, (unsigned) d + bytes }};
      return swix_regs (OS_File, &r);
    }
  if (index == 4)
    {
      /* *V3DReset [all] (0.10) */
      int all = argc > 0 && (tail[0] | 32) == 'a';
      return recover (all, 1);
    }
  if (index == 3)
    {
      /* *V3DPower [cycle] (0.08) */
      if (argc > 0)
	{
	  int open = 0;
	  for (int i = 0; i < V3D_MAX_FILES; i++)
	    open += dev->files[i].used && i != dev->twod.file;
	  if (open || !v3d_idle (dev))
	    {
	      write0 ("The GPU is in use: close the programs using it first.");
	      newline ();
	      return 0;
	    }
	  oserror *e = pi4_power_cycle ();
	  if (e)
	    return e;
	  v3d_gpu_reset_now (dev);		/* the reset, gates, MMU and interrupts */
	  write0 ("The V3D's power domain was turned off and on again.");
	  newline ();
	}
      uint32_t dom, g = pi4_pm_grafx ();
      oserror *e = pi4_domain (&dom);
      write0 ("Firmware V3D power domain: ");
      write0 (e ? e->errmess : dom == 1 ? "on" : dom == 0 ? "off" : "not known to the firmware");
      newline ();
      hex ("PM_GRAFX ", g);
      static const char names[] = "POWUP\0POWOK\0ISPOW\0MEMREP\0MRDONE\0ISFUNC\0RSTN\0";
      const char *n = names;
      write0 (":");
      for (int b = 0; b < 7; b++, n = skip0 (n))
	if (g & (1u << b))
	  {
	    write0 (" ");
	    write0 (n);
	  }
      if (g & (1u << 12))
	write0 (" ENAB");
      newline ();
      return 0;
    }
  if (index == 1)
    {
      uint32_t hz, max = 0;
      if (argc > 0)
	{
	  if (!v3d_idle (dev))
	    return 0;
	  oserror *e = pi4_set_clock (parse_dec (tail) * 1000000, &hz);
	  if (e)
	    return e;
	  clock_raised = 0;
	}
      oserror *e = pi4_clock (&hz, &max);
      if (e)
	return e;
      write0 ("V3D clock: ");
      write_dec (hz / 1000000);
      write0 (" MHz (maximum ");
      write_dec (max / 1000000);
      write0 (" MHz)");
      newline ();
      return 0;
    }

  v3d_service (dev);
  struct v3d_stats *s = &dev->stats;
  write0 ("V3D ");
  write_dec ((unsigned) dev->ver / 10);
  write0 (".");
  write_dec ((unsigned) dev->ver % 10);
  write0 (" rev ");
  write_dec ((unsigned) dev->rev);
  write0 (", HUB_IDENT1 &");
  write_hex8 (dev->ident[1]);
  write0 (mapped_up ? " (already running at load)" : " (powered up at load)");
  newline ();
  uint32_t hz = 0;
  pi4_clock (&hz, 0);
  line ("Clock (MHz):           ", hz / 1000000);
  uint32_t arm = 0, arm_max = 0;
  if (!pi4_arm_clock (&arm, &arm_max))
    {
      write0 ("ARM clock (MHz):       ");
      write_dec (arm / 1000000);
      write0 (" (the firmware's most: ");
      write_dec (arm_max / 1000000);
      write0 (")");
      newline ();
    }
  line ("Buffer memory (KB):    ", dev->area_pages * 4);
  line ("  in use (KB):         ", dev->pages_used * 4);
  /* (0.12) how much of it is above 1 GB, out of the VideoCore's reach */
  uint32_t high, low, big, super;
  count_pages (&high, &low, &big, &super);
  line ("  above 1 GB (KB):     ", high * 4);
  line ("  below 1 GB (KB):     ", low * 4);
  line ("  in 1 MB entries (KB):", super * 4);
  line ("  in 64 KB entries (KB):", big * 4);
  uint64_t pt = pi4_pt_pa ();
  write0 ("Page table at:         &");
  if (pt >> 32)
    {
      write_hex8 ((uint32_t) (pt >> 32));
      write0 ("_");
    }
  write_hex8 ((uint32_t) pt);
  write0 (pt >= 0x40000000u ? " (above 1 GB)" : " (below 1 GB)");
  newline ();
  int n = 0;
  for (int i = 0; i < V3D_MAX_FILES; i++)
    n += dev->files[i].used && i != dev->twod.file;
  line ("Files open:            ", (unsigned) n);
  line ("Bin jobs:              ", s->jobs[V3D_BIN]);
  line ("Render jobs:           ", s->jobs[V3D_RENDER]);
  line ("TFU jobs:              ", s->jobs[V3D_TFU]);
  line ("Compute jobs:          ", s->jobs[V3D_CSD]);
  /* (0.13) time on the GPU, as Linux's fdinfo */
  write0 ("GPU time (ms):         bin ");
  write_dec ((uint32_t) (s->busy_ns[V3D_BIN] / 1000000u));
  write0 (", render ");
  write_dec ((uint32_t) (s->busy_ns[V3D_RENDER] / 1000000u));
  write0 (", TFU ");
  write_dec ((uint32_t) (s->busy_ns[V3D_TFU] / 1000000u));
  write0 (", compute ");
  write_dec ((uint32_t) (s->busy_ns[V3D_CSD] / 1000000u));
  newline ();
  line ("Interrupts:            ", s->irqs);
  line ("  with nothing to do:  ", s->irqs_empty);
  line ("Binner overflows:      ", s->overflows);
  line ("  with no memory left: ", s->overflow_misses);
  line ("Timeouts:              ", s->timeouts);
  line ("Resets:                ", s->resets);
  line ("  jobs run again after:", s->requeued);
  /* (0.15) the CPU's cache work for cached and imported BOs */
  write0 ("Cache maintenance:     ");
  write_dec (s->cache_cleans);
  write0 (" cleans, ");
  write_dec (s->cache_invals);
  write0 (" invalidates (");
  write_dec (s->cache_deferred);
  write0 (" put off), ");
  write_dec ((uint32_t) (s->cache_ns / 1000000u));
  write0 (" ms; whole cache ");
  write_dec (pi4_cache_whole);
  write0 (" times (from ");
  write_dec (pi4_cache_threshold () / 1024);
  write0 (" KB)");
  newline ();
  /* (0.17) the desktop's copies and fills */
  write0 ("Desktop copies (2D):   ");
  write0 (twod.mode == TWOD_OFF ? (twod.stopped ? "off (stopped after the GPU failed)" : "off")
	  : twod.mode == TWOD_ASYNC ? "on, not waited for" : "on");
  if (twod.mode != TWOD_OFF && !twod.fills)
    write0 (", fills off");
  if (twod.mode != TWOD_OFF && twod.alongside)
    write0 (", alongside programs");
  if (twod.mode != TWOD_OFF && twod.screen_ok && twod.screen_why)
    {
      write0 (" (not used: ");
      write0 (twod_why_text (twod.screen_why));
      write0 (")");
    }
  newline ();
  if (twod.offered || twod.fills_offered)
    {
      write0 ("  copies by the GPU:   ");
      write_dec (twod.claimed);
      write0 (" of ");
      write_dec (twod.offered);
      write0 (" (");
      write_dec (twod.scrolls);
      write0 (" scrolls; ");
      write_dec (twod.small);
      write0 (" small)");
      newline ();
      write0 ("  fills by the GPU:    ");
      write_dec (twod.fills_claimed);
      write0 (" of ");
      write_dec (twod.fills_offered);
      write0 (" (");
      write_dec (twod.fills_small);
      write0 (" small, ");
      write_dec (twod.fills_pattern);
      write0 (" patterned)");
      newline ();
      write0 ("  GPU work:            ");
      write_dec (dev->twod.tiles);
      write0 (" tiles, ");
      write_dec ((uint32_t) (dev->twod.ns / 1000000u));
      write0 (" ms; waited ");
      write_dec (dev->twod.waits);
      write0 (" times; ");
      write_dec (twod.bank_flips);
      write0 (" bank changes");
      newline ();
      /* (0.19) where an op's time goes, in microseconds an op */
      {
	uint32_t n = twod.claimed + twod.fills_claimed, gpu_us = 0;
	if (n)
	  {
	    write0 ("  us an op (claimed):  ");
	    write_dec (div32 (twod.us_claimed, n));
	    write0 (" in GraphicsV: list ");
	    write_dec (div32 (dev->twod.us_build, n));
	    write0 (", submit ");
	    write_dec (div32 (dev->twod.us_submit, n));
	    write0 (", wait ");
	    write_dec (div32 (dev->twod.us_wait, n));
	    (void) gpu_us;
	    newline ();
	  }
	uint32_t left = twod.offered + twod.fills_offered - n;
	if (left)
	  {
	    write0 ("  us an op (left):     ");
	    write_dec (div32 (twod.us_left, left));
	    newline ();
	  }
      }
      if (twod.lim[0] != 1024 || twod.lim[1] != 8 || twod.lim[2] != 4096
	  || twod.lim[3] != 16 || twod.lim[4] != 0)
	{
	  write0 ("  limits (changed):    copies ");
	  write_dec (twod.lim[0]);
	  write0 (" bytes, ");
	  write_dec (twod.lim[1]);
	  write0 (" rows; fills ");
	  write_dec (twod.lim[2]);
	  write0 (" bytes, ");
	  write_dec (twod.lim[3]);
	  write0 (" rows, ");
	  write_dec (twod.lim[4]);
	  write0 (" a tile");
	  newline ();
	}
      write0 ("  left to the CPU:     ");
      write_dec (dev->twod.busy);
      write0 (" GPU busy with 3D, ");
      write_dec (twod.no_screen);
      write0 (" screen, ");
      write_dec (twod.disabled);
      write0 (" acceleration off, ");
      write_dec (twod.irqs_off);
      write0 (" interrupts off, ");
      write_dec (twod.prog_open);
      write0 (" a program using the GPU, ");
      write_dec (twod.errors);
      write0 (" errors");
      newline ();
    }
  if (s->page_moves)
    {
      write0 ("Pages moved by the OS: ");
      write_dec (s->pages_moved);
      write0 (" imported pages, ");
      write_dec (s->page_moves);
      write0 (" times (the GPU waited)");
      newline ();
    }
  line ("MMU errors:            ", s->mmu_errors);
  if (s->mmu_errors)
    {
      write0 ("  last: client ");
      write0 (v3d_axi_client (s->mmu_last_client));
      write0 (", page address &");
      write_hex8 (s->mmu_last_addr);
      write0 (", status &");
      write_hex8 (s->mmu_last_sts);
      newline ();
    }
  show_hang ("First hang", &s->first_hang);
  if (s->timeouts > 1)
    show_hang ("Last hang", &s->last_hang);
  for (unsigned i = pi4_log_next > PI4_LOG ? pi4_log_next - PI4_LOG : 0; i < pi4_log_next; i++)
    {
      write0 ("Log: ");
      write0 (pi4_log[i % PI4_LOG].msg);
      write0 (" &");
      write_hex8 (pi4_log[i % PI4_LOG].value);
      newline ();
    }
  return 0;
}
