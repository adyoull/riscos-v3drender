/* twod.c: GraphicsV for the desktop's rectangle copies (0.17)

   The kernel offers a display driver its rectangle copies as GraphicsV 13
   (Render), op 1. This claims GraphicsV and does the worthwhile ones on
   the GPU (core/v3d_2d.c), leaving everything else to the next claimant
   (BCMVideo, which leaves it to the kernel's CPU code).

   After Rob Heaton's V3DDriver (github.com/robheaton/riscos-v3d,
   GPL-2.0-or-later; gfxv_accel.c, module.c, screen_info.c, screen_base.h):
   the decoding of GraphicsV 13 1 and 2 (a solid fill: every ECF line's OR
   all ones, every EOR the same), the driver 0 / head 0 rule, finding the
   framestore with GraphicsV 9 and the write bank with GraphicsV 6 (R0 =
   3), forgetting both at GraphicsV 2 and Service_PreModeChange, and the
   size limits measured on his Pi 4. Written again for V3DRender.

   Copyright (C) 2026 Andrew Youll; the work it follows (C) 2026 Rob
   Heaton. GNU GPL version 2 or later. */

#include "cmodule.h"
#include "v3d.h"
#include "twod.h"

#define OS_Claim		0x1F
#define OS_Release		0x20
#define OS_ReadVduVariables	0x31
#define OS_CallAVector		0x34
#define OS_ScreenMode		0x65
#ifndef OS_DynamicArea
#define OS_DynamicArea		0x66
#endif
#define GRAPHICSV		42

/* (0.19) The limits, measured with V3DRender on the owner's Pi 4
   (native RISC OS 5, 1920x1200x32, ARM 1800 MHz; kit Time2D, 20 Nov 2026).
   V3DRender's own cost an op is ~5 us (list < 1, submit 1) and a GPU job
   ~5 us plus ~4 us a tile, so it is level with the CPU from about 16x16
   and ahead above that; big ones are memory-bound (GPU 1.3-2x). Copies
   from 1,024 bytes and 8 rows (Rob's V3DDriver: 2,048 and 16); fills
   from 4,096 bytes and 16 rows, with no rule a tile (Rob's: 16,384, 32,
   and bytes + 15 x rows >= 4,500 x tiles). *V3DRender 2D limits sets
   others. */
#define MIN_COPY_BYTES	1024u
#define MIN_COPY_ROWS	8u
#define MIN_FILL_BYTES	4096u
#define MIN_FILL_ROWS	16u
#define FILL_ROW_WEIGHT	15u
#define FILL_TILE_BYTES	0u

struct twod_state twod;

extern void gfxv_entry (void);

static struct v3d_dev *v3d;

static void
forget (void)
{
  twod.screen_ok = 0;
  twod.mapped = 0;
}

/* --- the screen ------------------------------------------------------ */

/* The geometry, the framestore (GraphicsV 9) and the address the VDU
   writes to: read again after every mode change or bank change. 0 or
   the reason the screen can't be used (kept until the next change). */
static int
screen (void)
{
  if (twod.screen_ok)
    return twod.screen_why;
  twod.screen_ok = 1;
  static const int vars[] = { 9, 6, 11, 12, 0x94, 0x95, 0x96, -1 };
  int v[7];
  regs r = {{ (unsigned) vars, (unsigned) v }};
  if (swix_regs (OS_ReadVduVariables, &r))
    return twod.screen_why = TWOD_NO_VDU;
  twod.log2bpp = (uint32_t) v[0];
  twod.stride = (uint32_t) v[1];
  twod.xlim = (uint32_t) v[2];
  twod.ylim = (uint32_t) v[3];
  uint32_t screen_start = (uint32_t) v[4], display_start = (uint32_t) v[5];
  uint32_t total = (uint32_t) v[6];
  if (twod.log2bpp != 5 && twod.log2bpp != 4)
    return twod.screen_why = TWOD_DEPTH;
  uint64_t vis = (uint64_t) (twod.ylim + 1) * twod.stride;
  if ((((uint64_t) twod.xlim + 1) << (twod.log2bpp - 3)) > twod.stride)
    return twod.screen_why = TWOD_GEOMETRY;
  regs g = {{ 0, 0, 0, 0, 9u, 0, 0, 0, 0, GRAPHICSV }};	/* driver 0, head 0 */
  if (swix_regs (OS_CallAVector, &g) || g.r[4] != 0)
    return twod.screen_why = TWOD_NO_FRAMESTORE;
  uint32_t fs_pa = g.r[0], fs_size = g.r[1];
  if (fs_size < total || vis > fs_size)
    return twod.screen_why = TWOD_GEOMETRY;
  twod.fs_pa = fs_pa;
  twod.fs_size = fs_size;
  twod.vis = (uint32_t) vis;
  /* where the VDU writes: GraphicsV 6's address if it gave one since the
     mode change (double buffering), else the framestore's start, but only
     if the VDU writes where the display shows (one bank) */
  uint32_t base;
  if (twod.vrender_known)
    {
      base = twod.vrender;
      if (base < fs_pa || (uint64_t) (base - fs_pa) + vis > fs_size)
	return twod.screen_why = TWOD_BANK;
    }
  else if (screen_start != display_start)
    return twod.screen_why = TWOD_BANK;
  else
    base = fs_pa;
  /* a screen the CPU caches: the GPU would read old pixels and the CPU
     would keep old ones over the GPU's; left to the CPU (screen memory is
     dynamic area 2; flag bit 5: not cacheable) */
  regs da = {{ 2, 2 }};
  if (swix_regs (OS_DynamicArea, &da) || !(da.r[4] & (1u << 5)))
    return twod.screen_why = TWOD_CACHED;
  if (v3d_2d_screen (v3d, fs_pa, fs_size))
    return twod.screen_why = TWOD_NO_MAP;
  twod.base_off = base - fs_pa;
  twod.mapped = 1;
  return twod.screen_why = 0;
}

/* (0.18) GraphicsV 6: the VDU moves to another bank (a program flipping
   between two screens, each frame). Same framestore, same mapping: only
   where the VDU writes changes, so no VDU variables, GraphicsV 9 or
   mapping to read again. */
static void
new_bank (uint32_t addr)
{
  if (twod.vrender_known && addr == twod.vrender)
    return;
  twod.vrender = addr;
  twod.vrender_known = 1;
  twod.bank_flips++;
  if (!twod.screen_ok || !twod.mapped
      || (twod.screen_why && twod.screen_why != TWOD_BANK && twod.screen_why != TWOD_CACHED))
    {
      forget ();
      return;
    }
  regs da = {{ 2, 2 }};			/* still not cached by the CPU */
  if (addr < twod.fs_pa || (uint64_t) (addr - twod.fs_pa) + twod.vis > twod.fs_size)
    twod.screen_why = TWOD_BANK;
  else if (swix_regs (OS_DynamicArea, &da) || !(da.r[4] & (1u << 5)))
    twod.screen_why = TWOD_CACHED;
  else
    {
      twod.base_off = addr - twod.fs_pa;
      twod.screen_why = 0;
    }
}

/* --- GraphicsV ------------------------------------------------------- */

static int
irqs_off (void)
{
  unsigned cpsr;
  __asm__ volatile ("mrs %0, cpsr" : "=r" (cpsr));
  return (cpsr & 0x80) != 0;
}

/* 1: the GPU may draw now; 0: leave it to the CPU (counted) */
static int
usable (void)
{
  if (irqs_off ())
    {
      twod.irqs_off++;
      return 0;
    }
  /* (0.21) Not while a program has the GPU open, unless asked
     (*V3DRender 2D alongside on). The web browser's 3D drawing went
     wrong with the desktop's fills on the GPU between its jobs: render
     jobs hung and were reset, and frames were lost (21 Nov 2026). The
     cause isn't found yet; until it is, the desktop's work stays on the
     CPU while any program uses the GPU. */
  if (!twod.alongside)
    for (int f = 0; f < V3D_MAX_FILES; f++)
      if (v3d->files[f].used && !v3d_internal_file (v3d, f))
	{
	  twod.prog_open++;
	  return 0;
	}
  /* the user's or a program's "no hardware acceleration", and a cached
     screen (the GPU would miss what the CPU has in its cache) */
  regs m = {{ 4, (unsigned) -1, (unsigned) -1 }};
  if (!swix_regs (OS_ScreenMode, &m))
    {
      if (m.r[1] & 4)
	{
	  twod.disabled++;
	  return 0;
	}
      if ((m.r[1] & 1) != twod.cache_suspended)
	{
	  twod.cache_suspended = m.r[1] & 1;
	  forget ();			/* the screen's cacheability changed */
	}
    }
  if (screen ())
    {
      twod.no_screen++;
      return 0;
    }
  return 1;
}

/* 2D off from inside GraphicsV: everything passed on from now. The
   vector isn't released here, in its own call (the kernel is walking
   its claimants); *V3DRender 2D or finalisation releases it. */
static void
stop_here (void)
{
  twod.mode = TWOD_OFF;
  twod.stopped++;
}

/* a GPU error: counted; a failed GPU turns 2D off, as V3DDriver does */
static void
failed (int ret)
{
  twod.errors++;
  twod.last_error = (uint32_t) -ret;
  if (ret == -V3D_EIO)
    stop_here ();
}

/* is a w x h rectangle worth a GPU fill? */
static int
fill_worth (uint32_t w, uint32_t h)
{
  uint32_t bytes = (w * h) << (twod.log2bpp - 3);
  if (h < twod.lim[3] || bytes < twod.lim[2])
    return 0;
  uint32_t tiles = ((w + 63) >> 6) * ((h + 63) >> 6);
  return bytes + FILL_ROW_WEIGHT * h >= tiles * twod.lim[4];
}

/* GraphicsV 13 2: left, top, right, bottom (inclusive, y up), the ECF
   (8 lines of OR and EOR words). 1: claimed. */
static int
fill (const uint32_t *p)
{
  uint32_t left = p[0], top = p[1], right = p[2], bottom = p[3];
  const uint32_t *ecf = (const uint32_t *) p[4];
  twod.fills_offered++;
  if (right < left || top < bottom || right - left > 0xFFFE || top - bottom > 0xFFFE)
    return 0;
  uint32_t w = right - left + 1, h = top - bottom + 1;
  /* the size first: most fills are small, and cost nothing to decline */
  if (twod.screen_ok && !twod.screen_why && !fill_worth (w, h))
    {
      twod.fills_small++;
      return 0;
    }
  /* solid: (old OR all ones) EOR e on every line, the same e, so the
     pixel is ~e whatever was there; at 16bpp both halves of the word */
  uint32_t e = ecf[1];
  for (int i = 0; i < 8; i++)
    if (ecf[2 * i] != 0xFFFFFFFFu || ecf[2 * i + 1] != e)
      {
	twod.fills_pattern++;
	return 0;
      }
  if (!usable ())
    return 0;
  if (!fill_worth (w, h))
    {
      twod.fills_small++;
      return 0;
    }
  if (twod.log2bpp == 4 && (e >> 16) != (e & 0xFFFF))
    {
      twod.fills_pattern++;
      return 0;
    }
  if (right > twod.xlim || top > twod.ylim)
    return 0;
  uint32_t bytes_pp = 1u << (twod.log2bpp - 3);
  struct v3d_2d_fill f = {
    .dst = twod.base_off + (twod.ylim - top) * twod.stride + left * bytes_pp,
    .stride = twod.stride, .width = w, .height = h, .bytes_pp = bytes_pp,
    .pixel = bytes_pp == 4 ? ~e : ~e & 0xFFFF, .wait = !twod.async,
  };
  int ret = v3d_2d_fill (v3d, &f);
  if (ret == -V3D_EBUSY)
    return 0;
  if (ret)
    {
      failed (ret);
      return 0;
    }
  twod.fills_claimed++;
  return 1;
}

/* 1: the copy is done (or under way, async): claim the call */
static int
copy (const uint32_t *p)
{
  uint32_t sx = p[0], sy = p[1], dx = p[2], dy = p[3];
  uint32_t w = p[4] + 1, h = p[5] + 1;
  twod.offered++;
  if (w > 0xFFFF || h > 0xFFFF || !w || !h)
    return 0;
  uint32_t bytes_pp = 1u << (twod.log2bpp - 3);
  if (twod.screen_ok && !twod.screen_why
      && (h < twod.lim[1] || (uint64_t) w * h * bytes_pp < twod.lim[0]))
    {
      twod.small++;			/* the CPU is quicker */
      return 0;
    }
  if (!usable ())
    return 0;
  bytes_pp = 1u << (twod.log2bpp - 3);
  if (h < twod.lim[1] || (uint64_t) w * h * bytes_pp < twod.lim[0])
    {
      twod.small++;
      return 0;
    }
  /* both rectangles on the screen (inclusive limits, wrap-safe) */
  if (sx > twod.xlim || w - 1 > twod.xlim - sx || dx > twod.xlim || w - 1 > twod.xlim - dx
      || sy > twod.ylim || h - 1 > twod.ylim - sy || dy > twod.ylim || h - 1 > twod.ylim - dy)
    return 0;
  /* GraphicsV's y is from the bottom; memory's from the top */
  uint32_t st = twod.ylim - (sy + h - 1), dt = twod.ylim - (dy + h - 1);
  struct v3d_2d_copy c = {
    .src = twod.base_off + st * twod.stride + sx * bytes_pp,
    .dst = twod.base_off + dt * twod.stride + dx * bytes_pp,
    .stride = twod.stride, .width = w, .height = h, .bytes_pp = bytes_pp,
    .wait = !twod.async,
  };
  /* overlapping: visit the tiles ahead of the move first (Rob's one-pass
     order); top-down rows, so moving down the screen is a larger row */
  if (sx < dx + w && dx < sx + w && sy < dy + h && dy < sy + h)
    c.order = (dx > sx ? V3D_2D_COLS_DESCENDING : 0) | (dt > st ? V3D_2D_ROWS_DESCENDING : 0);
  int overlap = sx < dx + w && dx < sx + w && sy < dy + h && dy < sy + h;
  int ret = v3d_2d_copy (v3d, &c);
  if (ret == -V3D_EBUSY)
    return 0;				/* 3D work on the GPU: the CPU does it */
  if (ret)
    {
      failed (ret);
      return 0;
    }
  twod.claimed++;
  if (overlap)
    twod.scrolls++;
  return 1;
}

static int gfxv_body (uint32_t *r);

/* r[0..9]: the vector's registers. 1: claimed (r[4] = 0).
   (0.26) Nothing in here claims or gives back RISC OS memory: that
   tells the Wimp memory moved in the middle of its redraw, the cause of
   Rob Heaton's V3DDriver freezing with the Task Manager open. What the
   driver would have done is left for a callback. */
int
gfxv_c (uint32_t *r)
{
  if (!v3d || r[4] >> 16 || twod.mode == TWOD_OFF)	/* driver 0, head 0 only (as BCMVideo) */
    return 0;
  int was = v3d->no_page_ops;
  v3d->no_page_ops = 1;
  int claimed = gfxv_body (r);
  v3d->no_page_ops = was;
  if (!was && v3d->page_work_due)
    v3d_later ();
  return claimed;
}

static int
gfxv_body (uint32_t *r)
{
  uint32_t reason = r[4] & 0xFFFF;
  if (reason == 6)			/* SetDMAAddress: R0 = 3, where the VDU writes */
    {
      if (r[0] == 3)
	new_bank (r[1]);
      return 0;
    }
  if (reason == 2)			/* SetMode: a new framestore is coming */
    {
      twod_mode_changing ();
      return 0;
    }
  if (reason != 13)
    return 0;
  if (twod.busy)
    return 0;				/* (re-entered: leave it) */
  twod.busy = 1;
  int claimed = 0;
  uint32_t flags = r[0], op = r[1];
  if (op == 1)
    {
      uint64_t t0 = plat_time_ns ();
      claimed = copy ((const uint32_t *) r[2]);
      *(claimed ? &twod.us_claimed : &twod.us_left) += (uint32_t) (plat_time_ns () - t0) / 1000u;
    }
  else if (op == 2 && twod.fills)
    {
      uint64_t t0 = plat_time_ns ();
      claimed = fill ((const uint32_t *) r[2]);
      *(claimed ? &twod.us_claimed : &twod.us_left) += (uint32_t) (plat_time_ns () - t0) / 1000u;
    }
  /* Synchronise (bit 0) after a claimed op; Synchronise if unclaimed
     (bit 1) after one left to the CPU or another driver (async only:
     otherwise nothing is ever left running) */
  if (twod.async && ((claimed && (flags & 1)) || (!claimed && (flags & 2)) || op == 0))
    if (v3d_2d_sync (v3d))
      {
	twod.errors++;
	stop_here ();
      }
  twod.busy = 0;
  if (claimed)
    r[4] = 0;
  return claimed;
}

/* --- on and off ------------------------------------------------------ */

void
twod_mode_changing (void)
{
  twod.vrender_known = 0;
  forget ();
  if (v3d)
    v3d_2d_forget_screen (v3d);
}

void
twod_mode_changed (void)
{
  forget ();
}

/* --- PRESENT's screen (0.31) ---------------------------------------- */

/* The screen as it is now, for DRM_IOCTL_RISCOS_PRESENT: read afresh
   each call, apart from the desktop's 2D (which may be off). Where the
   VDU writes (VDU variable 148, ScreenStart: OS_Byte 112's bank) is
   turned into its physical address, so any bank of the framestore works.
   ModeFlags bit 14: RGB order (TRGB); 16bpp NColour 65535 with ModeFlags
   bit 7 is 5:6:5, without it 1:5:5:5; 4,096 colours isn't taken. */
static int
present_screen (struct v3d_dev *dev, struct v3d_screen *s)
{
  (void) dev;
  static const int vars[] = { 0, 3, 9, 6, 11, 12, 148, -1 };
  int v[7];
  regs r = {{ (unsigned) vars, (unsigned) v }};
  if (swix_regs (OS_ReadVduVariables, &r))
    return -V3D_EINVAL;
  uint32_t flags = (uint32_t) v[0], ncolour = (uint32_t) v[1], log2bpp = (uint32_t) v[2];
  uint32_t rgb = (flags >> 14) & 1;
  if (log2bpp == 5)
    {
      s->kind = V3D_SCREEN_RGBA8;
      s->rb_swap = rgb;			/* TBGR: R, G, B bytes, RGBA8's order */
    }
  else if (log2bpp == 4 && ncolour == 65535 && (flags & (1u << 7)))
    {
      s->kind = V3D_SCREEN_565;
      s->rb_swap = !rgb;		/* BGR565 has R in the top bits: TRGB's (swapped on a load: 3 channels can't be) */
    }
  else if (log2bpp == 4 && ncolour == 65535)
    {
      s->kind = V3D_SCREEN_5551;
      s->rb_swap = rgb;			/* RGBA5551 has R in the bottom bits: TBGR's */
    }
  else
    return -V3D_EINVAL;
  if (flags & 1)			/* (teletext, non-graphics modes) */
    return -V3D_EINVAL;
  s->stride = (uint32_t) v[3];
  s->width = (uint32_t) v[4] + 1;
  s->height = (uint32_t) v[5] + 1;
  regs g = {{ 0, 0, 0, 0, 9u, 0, 0, 0, 0, GRAPHICSV }};	/* driver 0, head 0 */
  if (swix_regs (OS_CallAVector, &g) || g.r[4] != 0)
    return -V3D_EINVAL;
  s->fs_pa = g.r[0];
  s->fs_bytes = g.r[1];
  uint64_t pa = plat_virt_pa ((uintptr_t) (uint32_t) v[6]);
  if (!pa || pa < s->fs_pa || pa - s->fs_pa >= s->fs_bytes)
    return -V3D_EINVAL;
  s->off = (uint32_t) (pa - s->fs_pa);
  regs da = {{ 2, 2 }};			/* the screen's area: bit 5, not cacheable */
  s->cached = swix_regs (OS_DynamicArea, &da) || !(da.r[4] & (1u << 5));
  return 0;
}

void
twod_init (struct v3d_dev *dev, void *pw)
{
  v3d = dev;
  dev->present_screen = present_screen;	/* (0.31) */
  twod.pw = pw;
  twod.fills = 1;
  twod_limits (0, 0);
}

oserror *
twod_set (int mode)
{
  if (!v3d)
    return 0;
  if (mode == TWOD_OFF)
    {
      if (twod.claimed_vector)
	{
	  regs r = {{ GRAPHICSV, (unsigned) gfxv_entry, (unsigned) twod.pw }};
	  swix_regs (OS_Release, &r);
	  twod.claimed_vector = 0;
	}
      v3d_2d_stop (v3d);
      forget ();
      twod.mode = TWOD_OFF;
      return 0;
    }
  if (v3d_2d_start (v3d))
    return 0;
  twod.async = mode == TWOD_ASYNC;
  twod.mode = mode;
  twod.vrender_known = 0;		/* GraphicsV 6 wasn't followed while off */
  forget ();
  if (!twod.claimed_vector)
    {
      regs r = {{ GRAPHICSV, (unsigned) gfxv_entry, (unsigned) twod.pw }};
      oserror *e = swix_regs (OS_Claim, &r);
      if (e)
	{
	  v3d_2d_stop (v3d);
	  twod.mode = TWOD_OFF;
	  return e;
	}
      twod.claimed_vector = 1;
    }
  return 0;
}

void
twod_final (void)
{
  twod_set (TWOD_OFF);
  v3d = 0;
}

/* *V3DReset all closed every file, the 2D one too: open it again */
void
twod_files_closed (void)
{
  if (!v3d)
    return;
  v3d->twod.file = -1;
  v3d_present_files_closed (v3d);	/* (0.31) PRESENT's too */
  v3d->twod.cl_cpu = 0;
  v3d->twod.scr_handle = 0;
  v3d->twod.pending = 0;
  v3d->twod.slot = 0;
  forget ();
  if (twod.mode != TWOD_OFF && v3d_2d_start (v3d))
    twod_set (TWOD_OFF);
}

/* (0.18) the sizes below which the CPU does the work: the defaults (n = 0), or
   copy bytes, copy rows, fill bytes, fill rows, fill bytes a tile; for
   measuring where the GPU starts to win (the kit's Draw2D) */
void
twod_limits (const uint32_t *v, int n)
{
  static const uint32_t def[5] = { MIN_COPY_BYTES, MIN_COPY_ROWS, MIN_FILL_BYTES,
				    MIN_FILL_ROWS, FILL_TILE_BYTES };
  for (int i = 0; i < 5; i++)
    twod.lim[i] = i < n ? v[i] : def[i];
}

/* (0.19) *V3DRender 2D zero: the 2D counters and times from 0 */
void
twod_zero (void)
{
  twod.offered = twod.claimed = twod.scrolls = twod.small = 0;
  twod.irqs_off = twod.disabled = twod.no_screen = twod.errors = 0;
  twod.fills_offered = twod.fills_claimed = twod.fills_small = twod.fills_pattern = 0;
  twod.bank_flips = twod.us_claimed = twod.us_left = twod.prog_open = 0;
  if (v3d)
    {
      struct v3d_2d *t = &v3d->twod;
      t->copies = t->scrolls = t->fills = t->tiles = t->busy = t->failed = t->waits = 0;
      t->ns = 0;
      t->us_build = t->us_submit = t->us_wait = 0;
    }
}
