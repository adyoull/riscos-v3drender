/* module.c: V3DRender, the RISC OS module.

   SWIs (chunk &C3D40, PROVISIONAL):
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

#define VERSION 8			/* 0.08 */
#define SWI_BASE 0xC3D40
#define OS_ClaimDeviceVector	0x4B
#define OS_ReleaseDeviceVector	0x4C
#define Wimp_ReadSysInfo	0x400F2
#define RISC3D_Register		0xC3D01		/* provisional */
#define IRQ_DEVICE 10				/* iDev_GPU_VC3D */
#define HAL_IRQEnable 1
#define HAL_IRQDisable 2
#define HAL_IRQClear 3

void irq_entry (void);
void driver_entry (void);
void call_hal (unsigned arg, void *routine, unsigned sb);
const char *v3d_axi_client (uint32_t id);

static struct v3d_dev *dev;
static void *private_word;
static void *hal_irqclear;
static unsigned hal_sb;
static int vector_claimed, registered, mapped_up, clock_raised;
static uint32_t clock_before;

ERROR_BLOCK (err_swi, 0x1E6, "SWI value out of range for module V3DRender");
ERROR_BLOCK (err_file, 0x81D340, "Not a V3DRender file");
ERROR_BLOCK (err_notpi4, 0x81D341, "V3DRender needs a Raspberry Pi 4, Pi 400 or Compute Module 4");
ERROR_BLOCK (err_v3ddriver, 0x81D342, "V3DDriver is loaded: V3DRender can't share the GPU with it (*RMKill V3DDriver first)");
ERROR_BLOCK (err_nogpu, 0x81D343, "The GPU didn't answer as a V3D 4.2");
ERROR_BLOCK (err_files, 0x81D344, "Too many V3DRender files are open");
ERROR_BLOCK (err_mem, 0x81D345, "Not enough memory for V3DRender");
ERROR_BLOCK (err_sync, 0x81D346, "V3DRender takes its work through V3DRender_Ioctl, not RISC3D_Sync");

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

/* ---- RISC3D ---- */

static void
register_risc3d (void)
{
  if (registered)
    return;
  regs r = {{ 0, (unsigned) driver_entry, 0, (unsigned) "V3DRender (the Pi 4's GPU)",
	      1, SWI_BASE + 3, (unsigned) "v3d" }};
  registered = !swix_regs (RISC3D_Register, &r);
}

static void
deregister_risc3d (void)
{
  if (!registered)
    return;
  regs r = {{ 1, (unsigned) driver_entry, 0 }};
  swix_regs (RISC3D_Register, &r);
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
    if (dev->files[i].used)
      return;
  if (clock_raised && clock_before && v3d_idle (dev))
    {
      uint32_t now;
      pi4_set_clock (clock_before, &now);
      clock_raised = 0;
    }
}

/* RISC3D's calls: r0 = 0 open -> r0 = our file; 1 close r1 = file;
   2 sync: not for this kind of driver; 3 describe (RISC3D 0.03, asked
   only of drivers registered with flags bit 1): r0 = 0, on a GPU */
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
    }
  return ERR (err_sync);
}

/* ---- the module ---- */

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
	  n += dev->files[i].used;
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
      v3d_gpu_reset_now (dev);
      return 0;
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

oserror *
mod_init (const char *tail, int instance, void *pw)
{
  (void) instance;
  private_word = pw;
  while (tail && *tail == ' ')
    tail++;
  pi4_no_wfi = tail && (*tail == 'n' || *tail == 'N');	/* "NoWFI" */

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
  if (index == 3)
    {
      /* *V3DPower [cycle] (0.08) */
      if (argc > 0)
	{
	  int open = 0;
	  for (int i = 0; i < V3D_MAX_FILES; i++)
	    open += dev->files[i].used;
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
  line ("Buffer memory (KB):    ", dev->area_pages * 4);
  line ("  in use (KB):         ", dev->pages_used * 4);
  int n = 0;
  for (int i = 0; i < V3D_MAX_FILES; i++)
    n += dev->files[i].used;
  line ("Files open:            ", (unsigned) n);
  line ("Bin jobs:              ", s->jobs[V3D_BIN]);
  line ("Render jobs:           ", s->jobs[V3D_RENDER]);
  line ("TFU jobs:              ", s->jobs[V3D_TFU]);
  line ("Compute jobs:          ", s->jobs[V3D_CSD]);
  line ("Interrupts:            ", s->irqs);
  line ("  with nothing to do:  ", s->irqs_empty);
  line ("Binner overflows:      ", s->overflows);
  line ("  with no memory left: ", s->overflow_misses);
  line ("Timeouts:              ", s->timeouts);
  line ("Resets:                ", s->resets);
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
