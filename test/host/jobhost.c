/* jobhost.c: runs the kit's V3DJob on the build machine, against
   V3DRender's core and a simulated GPU that carries out the frames it
   sends: a render job reads its control list back out of the buffer
   memory (GPU address = offset), finds the image's size, clear colour
   and store (address, stride, format) and writes the pixels, as the V3D
   would for a frame that only clears. A bin job that starts with "wait
   on semaphore" never finishes, so the hang test exercises the reset.

   SPDX-License-Identifier: GPL-2.0-or-later
   Copyright (c) 2026 Andrew Youll */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "v3d.h"
#include "v3d_regs.h"
#include "v3djob.h"

static uint32_t regs[V3D_NBANKS][0x4000 / 4];
static int busy[V3D_HW_QUEUES], hung;
static uint32_t time_cs = 1000, resets;
static uint8_t *area;
static const uint32_t area_pages = 16384;	/* 64 MB */
static uint32_t pt[1 << 20];
static struct v3d_dev *dev;

uint32_t v3d_rd (struct v3d_dev *v, int b, uint32_t o) { (void) v; return regs[b][o / 4]; }
uint64_t plat_page_pa (struct v3d_dev *v, uint32_t p) { (void) v; return 0x100000000ull + p * 4096ull; }
int plat_pages_claim (struct v3d_dev *v, uint32_t f, uint32_t n) { (void) v; memset (area + f * 4096, 0xAA, n * 4096); return 0; }
void plat_pages_release (struct v3d_dev *v, uint32_t f, uint32_t n) { (void) v; (void) f; (void) n; }
void plat_gpu_reset (struct v3d_dev *v) { (void) v; resets++; hung = 0; memset (busy, 0, sizeof busy); }
uint32_t plat_time_cs (void) { return time_cs; }
void plat_barrier (void) { }
unsigned plat_irq_off (void) { return 0; }
void plat_irq_restore (unsigned s) { (void) s; }
void *plat_alloc (size_t n) { return calloc (1, n); }
void plat_free (void *p) { free (p); }
void plat_log (struct v3d_dev *v, const char *m, uint32_t x) { (void) v; printf ("    [log] %s %x\n", m, x); }

static uint8_t *
va (uint32_t a)
{
  if (a >= area_pages * 4096 || pt[a >> 12] == 0)
    {
      printf ("SIM: the GPU read or wrote an unmapped address &%x\n", a);
      exit (2);
    }
  return area + a;
}

static uint64_t
bits (const uint8_t *p, int start, int size)
{
  uint64_t v = 0;
  for (int i = 0; i < size; i++)
    v |= (uint64_t) ((p[(start + i) / 8] >> ((start + i) % 8)) & 1) << i;
  return v;
}

/* the render job: the frame */
static void
render (uint32_t start, uint32_t end)
{
  uint32_t w = 0, h = 0, colour = 0, gen = 0, gen_end = 0, tiles = 0;
  for (uint32_t a = start; a < end;)
    {
      uint8_t *p = va (a), op = p[0];
      if (op == 121 && (p[1] & 15) == 0)
	w = (uint32_t) bits (p + 1, 8, 16), h = (uint32_t) bits (p + 1, 24, 16);
      if (op == 121 && (p[1] & 15) == 3)
	colour = (uint32_t) bits (p + 1, 8, 32);
      if (op == 20)
	gen = (uint32_t) bits (p + 1, 0, 32), gen_end = (uint32_t) bits (p + 1, 32, 32);
      if (op == 23)
	tiles++;
      static const uint8_t len[128] = { [4] = 1, [6] = 1, [8] = 1, [13] = 1, [18] = 1, [19] = 1,
	[20] = 9, [21] = 2, [23] = 3, [25] = 2, [26] = 1, [27] = 1, [29] = 13, [54] = 5,
	[56] = 2, [92] = 5, [120] = 9, [121] = 9, [122] = 9, [123] = 5, [124] = 4,
	[125] = 1, [126] = 2 };
      if (op >= 128 || !len[op])
	{
	  printf ("SIM: unknown opcode %u in the RCL\n", op);
	  exit (2);
	}
      a += len[op];
    }
  /* the store in the generic list */
  for (uint32_t a = gen; a < gen_end; a++)
    {
      uint8_t *p = va (a);
      if (p[0] == 29 && bits (p + 1, 0, 4) == 0)
	{
	  uint32_t stride = (uint32_t) bits (p + 1, 28, 20), addr = (uint32_t) bits (p + 1, 64, 32);
	  if (bits (p + 1, 12, 6) != 27 || bits (p + 1, 4, 3) != 0)
	    printf ("SIM: not a raster RGBA8 store\n"), exit (2);
	  for (uint32_t y = 0; y < h; y++)
	    for (uint32_t x = 0; x < w; x++)
	      memcpy (va (addr + y * stride + x * 4), &colour, 4);
	  if (!tiles)
	    printf ("SIM: no supertiles: nothing would be rendered\n"), exit (2);
	  return;
	}
    }
  printf ("SIM: no store to render target 0\n");
  exit (2);
}

void
v3d_wr (struct v3d_dev *v, int bank, uint32_t off, uint32_t val)
{
  (void) v;
  if (bank == V3D_BANK_CORE0 && off == V3D_CTL_INT_CLR)
    {
      regs[bank][V3D_CTL_INT_STS / 4] &= ~val;
      return;
    }
  if (bank == V3D_BANK_HUB && off == V3D_HUB_INT_CLR)
    {
      regs[bank][V3D_HUB_INT_STS / 4] &= ~val;
      return;
    }
  if (bank == V3D_BANK_HUB && off == V3D_MMUC_CONTROL)
    val &= ~V3D_MMUC_CONTROL_FLUSHING;
  if (bank == V3D_BANK_HUB && off == V3D_MMU_CTL)
    val &= ~V3D_MMU_CTL_TLB_CLEARING;
  if (bank == V3D_BANK_CORE0 && off == V3D_CTL_L2TCACTL)
    val &= ~(V3D_L2TCACTL_TMUWCF | V3D_L2TCACTL_L2TFLS);
  regs[bank][off / 4] = val;
  if (bank == V3D_BANK_CORE0 && off == V3D_CLE_CT0QEA)
    {
      busy[V3D_BIN] = 1;
      hung = va (regs[bank][V3D_CLE_CT0QBA / 4])[0] == 8;	/* wait on semaphore */
    }
  if (bank == V3D_BANK_CORE0 && off == V3D_CLE_CT1QEA)
    {
      render (regs[bank][V3D_CLE_CT1QBA / 4], val);
      busy[V3D_RENDER] = 1;
    }
}

void
plat_wait (void)
{
  time_cs++;
  static const uint32_t bit[2] = { V3D_INT_FLDONE, V3D_INT_FRDONE };
  for (int q = 0; q < 2; q++)
    if (busy[q] && !(q == V3D_BIN && hung))
      {
	busy[q] = 0;
	regs[V3D_BANK_CORE0][V3D_CTL_INT_STS / 4] |= bit[q];
	v3d_irq (dev);
      }
}

/* ---- v3djob.h ---- */

const char *
sys_info (unsigned *version, unsigned *v3d, unsigned *hz)
{
  if (!dev)
    {
      regs[V3D_BANK_HUB][V3D_HUB_IDENT1 / 4] = 0x000E1124;
      regs[V3D_BANK_HUB][V3D_MMU_DEBUG_INFO / 4] = (6 << 8) | (2 << 4);
      area = calloc (area_pages, 4096);
      dev = calloc (1, sizeof *dev);
      dev->ver = 42;
      dev->pt = pt;
      dev->pt_pa = 0x80000000ull;
      dev->scratch_pa = 0x80400000ull;
      dev->area = area;
      dev->area_pages = area_pages;
      if (v3d_hw_probe (dev) || v3d_init (dev))
	return "init failed";
    }
  *version = 2;
  *v3d = (unsigned) dev->ver;
  *hz = 500000000;
  return 0;
}

const char *
sys_open (int *file)
{
  int f = v3d_open (dev, 0);
  *file = f + 1;
  return f < 0 ? "open failed" : 0;
}

void sys_close (int file) { v3d_close (dev, file - 1); }

int
sys_ioctl (int file, uint32_t request, void *arg, const char **err)
{
  *err = 0;
  return v3d_ioctl (dev, file - 1, request, arg);
}

unsigned now_cs (void) { return time_cs; }

void
sys_status (const char *also_to)
{
  (void) also_to;
  printf ("[sim] render jobs %u, bin jobs %u, timeouts %u, resets %u, pages in use %u\n",
	  dev->stats.jobs[V3D_RENDER], dev->stats.jobs[V3D_BIN], dev->stats.timeouts,
	  resets, dev->pages_used);
}
