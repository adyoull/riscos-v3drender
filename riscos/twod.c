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

/* Rob's measured limits (FillCopyBench, 2026-10-07): a copy smaller than
   2,048 bytes, or of fewer than 16 rows, is quicker on the CPU. A scroll
   (overlapping) costs the GPU the same as a copy. */
#define MIN_COPY_BYTES	2048u
#define MIN_COPY_ROWS	16u
/* and for fills (FillCopyBench, 2026-10-07): 16,384 bytes and 32 rows,
   and enough work a 64x64 tile: bytes + 15 x rows >= 4,500 x tiles (the
   GPU pays by the tile, the CPU by the byte and the row) */
#define MIN_FILL_BYTES	16384u
#define MIN_FILL_ROWS	32u
#define FILL_ROW_WEIGHT	15u
#define FILL_TILE_BYTES	4500u

struct twod_state twod;

extern void gfxv_entry (void);

static struct v3d_dev *v3d;

static void
forget (void)
{
  twod.screen_ok = 0;
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
  return twod.screen_why = 0;
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
  if (h < MIN_FILL_ROWS || bytes < MIN_FILL_BYTES)
    return 0;
  uint32_t tiles = ((w + 63) >> 6) * ((h + 63) >> 6);
  return bytes + FILL_ROW_WEIGHT * h >= tiles * FILL_TILE_BYTES;
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
      && (h < MIN_COPY_ROWS || (uint64_t) w * h * bytes_pp < MIN_COPY_BYTES))
    {
      twod.small++;			/* the CPU is quicker */
      return 0;
    }
  if (!usable ())
    return 0;
  bytes_pp = 1u << (twod.log2bpp - 3);
  if (h < MIN_COPY_ROWS || (uint64_t) w * h * bytes_pp < MIN_COPY_BYTES)
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

/* r[0..9]: the vector's registers. 1: claimed (r[4] = 0). */
int
gfxv_c (uint32_t *r)
{
  if (!v3d || r[4] >> 16 || twod.mode == TWOD_OFF)	/* driver 0, head 0 only (as BCMVideo) */
    return 0;
  uint32_t reason = r[4] & 0xFFFF;
  if (reason == 6)			/* SetDMAAddress: R0 = 3, where the VDU writes */
    {
      if (r[0] == 3)
	{
	  twod.vrender = r[1];
	  twod.vrender_known = 1;
	  forget ();
	}
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
    claimed = copy ((const uint32_t *) r[2]);
  else if (op == 2 && twod.fills)
    claimed = fill ((const uint32_t *) r[2]);
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

void
twod_init (struct v3d_dev *dev, void *pw)
{
  v3d = dev;
  twod.pw = pw;
  twod.fills = 1;
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
  v3d->twod.cl_cpu = 0;
  v3d->twod.scr_handle = 0;
  v3d->twod.pending = 0;
  v3d->twod.slot = 0;
  forget ();
  if (twod.mode != TWOD_OFF && v3d_2d_start (v3d))
    twod_set (TWOD_OFF);
}
