/* v3d_2d.c: the desktop's rectangle copies on the GPU (0.17)

   The kernel offers a display driver its rectangle copies (GraphicsV 13,
   op 1: window drags, scrolls, OS_Plot block copies). The platform layer
   decodes the call; this does the copy as a render job: for each 64x64
   tile of the rectangle, load the tile buffer from the source (Load Tile
   Buffer General) and store it to the destination (Store Tile Buffer
   General). No shaders, no binning. Solid fills (op 2) likewise: the
   tile buffer cleared to the colour once, then stored to each tile.

   After Rob Heaton's V3DDriver (github.com/robheaton/riscos-v3d,
   GPL-2.0-or-later): the per-tile Load/Store render list and its header
   (cl_copy.c, cl_common.c, cl_packets.h), the one-pass tile order that
   makes overlapping copies (scrolls) right, the formats (RGBA8 for
   32bpp, BGR565 for the Pi's 16bpp) and his hardware findings. Written
   again here for V3DRender's queues, MMU and hang recovery. Different
   from V3DDriver:
   - one render job, no binning job: the list never reads tile lists;
   - the framestore is mapped into the GPU's MMU once a mode (big
     entries when aligned), and the list goes into one buffer kept for
     good: nothing is claimed or zeroed per copy;
   - no bands: with no binning there's no tile-memory limit;
   - the job goes through V3DRender's scheduler, so it never meets a 3D
     job on the hardware; while 3D work is queued or running, the copy is
     left to the CPU rather than waiting behind it;
   - a fill clears the tile buffer once (on the first tile) rather than
     on every tile: with nothing loaded, a store leaves the buffer as it
     was, so each later tile only stores (half the packets);
   - not waited for (async), up to four lists are in flight at once, in
     four parts of the buffer, so copies queue on the GPU while the CPU
     goes on.

   Copyright (C) 2026 Andrew Youll; the work it follows (C) 2026 Rob
   Heaton. GNU GPL version 2 or later. */

#include "v3d.h"

#define CL_BYTES (128u * 1024u)
#define TILE 64u

static int
ioctl_ (struct v3d_dev *v3d, int f, uint32_t request, void *arg)
{
  return v3d_ioctl (v3d, f, request, arg);
}

int
v3d_2d_start (struct v3d_dev *v3d)
{
  struct v3d_2d *t = &v3d->twod;
  if (t->file >= 0 && t->cl_cpu)
    return 0;
  int f = v3d_open (v3d, 0);		/* owner 0: no task closes it */
  if (f < 0)
    return f;
  struct drm_v3d_create_bo c = { .size = CL_BYTES };
  int ret = ioctl_ (v3d, f, DRM_IOCTL_V3D_CREATE_BO, &c);
  if (ret)
    {
      v3d_close (v3d, f);
      return ret;
    }
  t->file = f;
  t->cl_handle = c.handle;
  t->cl_gpu = c.offset;
  t->cl_bytes = CL_BYTES;
  t->cl_cpu = v3d_bo_cpu (v3d, v3d->files[f].handles[c.handle]);
  t->scr_handle = 0;
  t->pending = 0;
  return 0;
}

void
v3d_2d_stop (struct v3d_dev *v3d)
{
  struct v3d_2d *t = &v3d->twod;
  if (t->file < 0)
    return;
  v3d_2d_sync (v3d);
  v3d_close (v3d, t->file);		/* the screen's mapping and the list go */
  t->file = -1;
  t->cl_cpu = 0;
  t->scr_handle = 0;
}

void
v3d_2d_forget_screen (struct v3d_dev *v3d)
{
  struct v3d_2d *t = &v3d->twod;
  if (t->file < 0 || !t->scr_handle)
    return;
  v3d_2d_sync (v3d);
  struct drm_gem_close g = { .handle = t->scr_handle };
  ioctl_ (v3d, t->file, DRM_IOCTL_GEM_CLOSE, &g);
  t->scr_handle = 0;
  t->scr_bytes = 0;
}

/* The framestore: `bytes` from physical `pa`. Mapped from the 1 MB
   boundary below it, so that a 1 MB-aligned GPU address over a 1 MB-
   aligned physical one gives 1 MB entries; kept until it changes. */
int
v3d_2d_screen (struct v3d_dev *v3d, uint64_t pa, uint32_t bytes)
{
  struct v3d_2d *t = &v3d->twod;
  if (t->file < 0)
    return -V3D_EINVAL;
  if (t->scr_handle && pa == t->scr_pa && bytes <= t->scr_bytes)
    return 0;
  v3d_2d_forget_screen (v3d);
  uint64_t from = pa & ~(uint64_t) 0xFFFFF;
  uint64_t to = (pa + bytes + 0xFFFF) & ~(uint64_t) 0xFFFF;
  if (to - from > 0x40000000u)
    return -V3D_EINVAL;
  uint32_t h = v3d_handle_slot (v3d, t->file);
  if (!h)
    return -V3D_ENOMEM;
  int bo, ret = v3d_bo_import_phys (v3d, from, (uint32_t) (to - from), &bo);
  if (ret)
    return ret;
  v3d->bos[bo].file = (int16_t) t->file;
  v3d->files[t->file].handles[h] = (uint16_t) bo;
  t->scr_handle = h;
  t->scr_gpu = v3d_bo_offset (v3d, bo) + (uint32_t) (pa - from);
  t->scr_pa = pa;
  t->scr_bytes = bytes;
  return 0;
}

/* --- the render list ------------------------------------------------- */

struct cl
{
  uint8_t *p, *end;
};

static void
put (struct cl *c, uint64_t v, int bytes)
{
  if (c->p + bytes > c->end)
    {
      c->p = c->end + 1;		/* overflowed: noticed by the caller */
      return;
    }
  for (int i = 0; i < bytes; i++)
    *c->p++ = (uint8_t) (v >> (8 * i));
}

/* a / b with no library division (the module's libgcc would bring in
   raise() for a division by zero) */
static uint32_t
udiv (uint32_t a, uint32_t b)
{
  uint32_t q = 0, bit = 1;
  while (b < a && !(b & 0x80000000u))
    {
      b <<= 1;
      bit <<= 1;
    }
  while (bit)
    {
      if (a >= b)
	{
	  a -= b;
	  q |= bit;
	}
      b >>= 1;
      bit >>= 1;
    }
  return q;
}

/* Supertiles: as few as make fewer than 256 of them (Rob's
   v3d_get_super_tiles) */
static void
supertiles (uint32_t tx, uint32_t ty, uint32_t *sw, uint32_t *sh, uint32_t *fw, uint32_t *fh)
{
  uint32_t w = 1, h = 1;
  for (;;)
    {
      *fw = udiv (tx + w - 1, w);
      *fh = udiv (ty + h - 1, h);
      if (*fw * *fh < 256)
	break;
      if (w < h)
	w++;
      else
	h++;
    }
  *sw = w;
  *sh = h;
}

/* The list's header: the frame (the rectangle), one 32bpp 8-bit render
   target with its clear colour, and the supertiles; then the tiles. */
static void
header (struct cl *c, uint32_t width, uint32_t height, uint32_t tile_list, uint32_t rgba)
{
  uint32_t tx = (width + TILE - 1) / TILE, ty = (height + TILE - 1) / TILE;
  uint32_t sw, sh, fw, fh;
  supertiles (tx, ty, &sw, &sh, &fw, &fh);
  put (c, 121, 1);			/* Tile Rendering Mode Cfg (Common) */
  put (c, 0, 1);			/*   one render target */
  put (c, width, 2);
  put (c, height, 2);
  put (c, 2u << 7, 2);			/*   internal depth type */
  put (c, 0, 1);
  put (c, 121, 1);			/* (Clear Colors), render target 0 */
  put (c, 3u | (uint64_t) rgba << 8, 8);
  put (c, 121, 1);			/* (Color): RT0 32bpp, 8-bit unorm */
  put (c, 1u | 2u << 6, 8);
  put (c, 121, 1);			/* (ZS Clear Values): z 1.0 */
  put (c, 2u | (uint64_t) 0x3F800000u << 16, 8);
  put (c, 126, 1);			/* Tile List Initial Block Size */
  put (c, 1u << 2, 1);			/*   64 bytes, auto-chained */
  put (c, 123, 1);			/* Multicore Rendering Tile List Set Base */
  put (c, tile_list & ~63u, 4);
  put (c, 122, 1);			/* Multicore Rendering Supertile Cfg */
  put (c, sw - 1, 1);
  put (c, sh - 1, 1);
  put (c, fw, 1);
  put (c, fh, 1);
  put (c, (tx & 0xFFF) | (ty & 0xFFF) << 12, 3);
  put (c, 0, 1);			/*   one tile list, not multicore */
}

static uint32_t
finish (struct cl *c, uint8_t *cl)
{
  put (c, 13, 1);			/* End of Rendering */
  if (c->p > c->end)
    return 0;
  return (uint32_t) (c->p - cl);
}

/* RT0, raster, the format; height 0: the frame's */
static uint64_t
general (uint32_t format, uint32_t stride)
{
  return (uint64_t) (format & 0x3F) << 12 | (uint64_t) (stride & 0xFFFFF) << 28;
}

/* (0.18) Tiles written a word at a time: the list's buffer is mapped
   uncached (Normal, bufferable), where byte stores cost as much as word
   stores; each tile's packets are built once and stamped with its
   coordinates. A list starts 4-byte aligned, the header is 52 bytes and
   a copy's tile 32, so every tile is aligned; a fill's tile (19 bytes)
   and clearing tile (21) are padded with NOPs to 20 and 24. Built byte by
   byte if cl isn't aligned (the tests' odd buffers). */
#define COPY_TILE 32u
#define FILL_TILE 20u
#define CLEAR_TILE 24u

static void
put_tile (struct cl *c, const uint32_t *t, uint32_t words, uint32_t col, uint32_t row)
{
  uint32_t w0 = (t[0] & 0xFFu) | ((col & 0xFFF) | (row & 0xFFF) << 12) << 8;
  if (!((uintptr_t) c->p & 3))
    {
      volatile uint32_t *d = (volatile uint32_t *) (void *) c->p;
      d[0] = w0;
      for (uint32_t i = 1; i < words; i++)
	d[i] = t[i];
    }
  else
    {
      put (c, w0, 4);
      for (uint32_t i = 1; i < words; i++)
	put (c, t[i], 4);
      c->p -= 4 * words;
    }
  c->p += 4 * words;
}

/* one tile's packets into t (as little-endian words), its coordinates 0 */
static void
template (uint32_t *t, uint32_t words, void (*fill) (struct cl *, const void *), const void *arg)
{
  uint8_t b[33];			/* (one more: put's overflow mark) */
  struct cl c = { b, b + 32 };
  for (uint32_t i = 0; i < sizeof b; i++)
    b[i] = 1;				/* NOP */
  fill (&c, arg);
  for (uint32_t i = 0; i < words; i++)
    t[i] = (uint32_t) b[4 * i] | (uint32_t) b[4 * i + 1] << 8
      | (uint32_t) b[4 * i + 2] << 16 | (uint32_t) b[4 * i + 3] << 24;
}

struct copy_tile
{
  uint64_t g;
  uint32_t src, dst;
};

static void
copy_tile (struct cl *c, const void *arg)
{
  const struct copy_tile *t = arg;
  put (c, 124, 1);			/* Tile Coordinates (stamped per tile) */
  put (c, 0, 3);
  put (c, 30, 1);			/* Load Tile Buffer General: the source */
  put (c, t->g, 8);
  put (c, t->src, 4);
  put (c, 26, 1);			/* End of Loads */
  put (c, 29, 1);			/* Store Tile Buffer General: the destination */
  put (c, t->g, 8);
  put (c, t->dst, 4);
  put (c, 27, 1);			/* End of Tile Marker */
}

/* A copy's render list into cl (cap bytes): its length, or 0 if it
   doesn't fit. The frame is the rectangle; each tile's load and store
   address their tile from the frame's origin (src, dst), and the store
   clips at the frame's edge. order: V3D_2D_*_DESCENDING. */
uint32_t
v3d_2d_copy_rcl (uint8_t *cl, uint32_t cap, uint32_t gpu_base, uint32_t tile_list,
		 uint32_t src, uint32_t dst, uint32_t stride,
		 uint32_t width, uint32_t height, uint32_t format, uint32_t order)
{
  (void) gpu_base;
  struct cl c = { cl, cl + cap };
  uint32_t tx = (width + TILE - 1) / TILE, ty = (height + TILE - 1) / TILE;
  if (52u + (uint64_t) tx * ty * COPY_TILE + 1 > cap)
    return 0;
  header (&c, width, height, tile_list, 0);
  uint32_t t[COPY_TILE / 4];
  struct copy_tile ct = { general (format, stride), src, dst };
  template (t, COPY_TILE / 4, copy_tile, &ct);
  for (uint32_t ri = 0; ri < ty; ri++)
    {
      uint32_t row = (order & V3D_2D_ROWS_DESCENDING) ? ty - 1 - ri : ri;
      for (uint32_t ci = 0; ci < tx; ci++)
	put_tile (&c, t, COPY_TILE / 4,
		  (order & V3D_2D_COLS_DESCENDING) ? tx - 1 - ci : ci, row);
    }
  return finish (&c, cl);
}

/* (0.31) PRESENT's list: as a copy's, but the load and the store each
   with their own General word (formats, memory format, R/B swap, alpha
   forced to 1), from src to dst: the source's tile to the screen's */
struct blit_tile
{
  uint64_t lg, sg;
  uint32_t src, dst;
};

static void
blit_tile (struct cl *c, const void *arg)
{
  const struct blit_tile *t = arg;
  put (c, 124, 1);			/* Tile Coordinates (stamped per tile) */
  put (c, 0, 3);
  put (c, 30, 1);			/* Load Tile Buffer General: the source */
  put (c, t->lg, 8);
  put (c, t->src, 4);
  put (c, 26, 1);			/* End of Loads */
  put (c, 29, 1);			/* Store Tile Buffer General: the screen */
  put (c, t->sg, 8);
  put (c, t->dst, 4);
  put (c, 27, 1);			/* End of Tile Marker */
}

/* rows row0 .. row0+rows-1 of the frame's tiles (a big frame goes in
   several jobs, each with the whole frame's header: a UIF image can only
   be addressed from its start) */
uint32_t
v3d_present_rcl (uint8_t *cl, uint32_t cap, uint32_t tile_list,
		 uint32_t src, uint64_t load_g, uint32_t dst, uint64_t store_g,
		 uint32_t width, uint32_t height, uint32_t row0, uint32_t rows)
{
  struct cl c = { cl, cl + cap };
  uint32_t tx = (width + TILE - 1) / TILE, ty = (height + TILE - 1) / TILE;
  if (!width || !height || width > 0xFFFF || height > 0xFFFF || !rows
      || row0 >= ty || rows > ty - row0
      || 52u + (uint64_t) tx * rows * COPY_TILE + 1 > cap)
    return 0;
  header (&c, width, height, tile_list, 0);
  uint32_t t[COPY_TILE / 4];
  struct blit_tile bt = { load_g, store_g, src, dst };
  template (t, COPY_TILE / 4, blit_tile, &bt);
  for (uint32_t row = row0; row < row0 + rows; row++)
    for (uint32_t col = 0; col < tx; col++)
      put_tile (&c, t, COPY_TILE / 4, col, row);
  return finish (&c, cl);
}

struct fill_tile
{
  uint64_t g;
  uint32_t dst;
};

static void
clear_tile (struct cl *c, const void *arg)
{
  (void) arg;
  put (c, 124, 1);			/* Tile Coordinates 0, 0 */
  put (c, 0, 3);
  put (c, 26, 1);			/* End of Loads */
  put (c, 29, 1);			/* Store Tile Buffer General: none */
  put (c, 8, 8);
  put (c, 0, 4);
  put (c, 25, 1);			/* Clear Tile Buffers: all render targets, Z/S */
  put (c, 3, 1);
  put (c, 27, 1);			/* End of Tile Marker; then NOPs to 24 */
}

static void
fill_tile (struct cl *c, const void *arg)
{
  const struct fill_tile *t = arg;
  put (c, 124, 1);
  put (c, 0, 3);
  put (c, 26, 1);
  put (c, 29, 1);
  put (c, t->g, 8);
  put (c, t->dst, 4);
  put (c, 27, 1);			/* then a NOP to 20 */
}

/* A solid fill's render list: the first tile clears the tile buffer to
   the colour (Rob's clear: no store, Clear Tile Buffers); every tile then
   stores it. Nothing is loaded, so the buffer keeps the colour. */
uint32_t
v3d_2d_fill_rcl (uint8_t *cl, uint32_t cap, uint32_t tile_list, uint32_t dst,
		 uint32_t stride, uint32_t width, uint32_t height, uint32_t format,
		 uint32_t rgba)
{
  struct cl c = { cl, cl + cap };
  uint32_t tx = (width + TILE - 1) / TILE, ty = (height + TILE - 1) / TILE;
  if (52u + CLEAR_TILE + (uint64_t) tx * ty * FILL_TILE + 1 > cap)
    return 0;
  header (&c, width, height, tile_list, rgba);
  uint32_t t[CLEAR_TILE / 4];
  template (t, CLEAR_TILE / 4, clear_tile, 0);
  put_tile (&c, t, CLEAR_TILE / 4, 0, 0);
  struct fill_tile ft = { general (format, stride), dst };
  template (t, FILL_TILE / 4, fill_tile, &ft);
  for (uint32_t row = 0; row < ty; row++)
    for (uint32_t col = 0; col < tx; col++)
      put_tile (&c, t, FILL_TILE / 4, col, row);
  return finish (&c, cl);
}

/* The clear colour for a screen pixel: 32bpp as it is (red the low byte,
   as RGBA8 stores it); the Pi's 16bpp (BGR565: red the top 5 bits)
   widened to 8 bits a channel, round(v * 255 / max), which the store
   narrows back exactly (Rob's widen_565_to_8bit_rgba; his multiply-shift
   forms, checked for every value) */
uint32_t
v3d_2d_rgba (uint32_t pixel, uint32_t bytes_pp)
{
  if (bytes_pp == 4)
    return pixel;
  uint32_t b5 = pixel & 0x1F, g6 = (pixel >> 5) & 0x3F, r5 = (pixel >> 11) & 0x1F;
  return ((r5 * 527u + 23u) >> 6) | ((g6 * 259u + 33u) >> 6) << 8
    | ((b5 * 527u + 23u) >> 6) << 16 | 0xFFu << 24;
}

/* --- the copy ---------------------------------------------------- */

int
v3d_2d_sync (struct v3d_dev *v3d)
{
  struct v3d_2d *t = &v3d->twod;
  if (t->file < 0 || !t->pending)
    return 0;
  uint32_t resets = v3d->stats.resets;
  /* every list is in the one buffer: its last job is the last of ours */
  struct drm_v3d_wait_bo w = { .handle = t->cl_handle, .timeout_ns = 2000000000ull };
  uint64_t t0 = plat_time_ns ();
  int ret = ioctl_ (v3d, t->file, DRM_IOCTL_V3D_WAIT_BO, &w);
  t->us_wait += (uint32_t) (plat_time_ns () - t0) / 1000u;
  t->pending = 0;
  t->slot = 0;
  t->waits++;
  plat_barrier ();
  if (!ret && v3d->stats.resets != resets)
    ret = -V3D_EIO;			/* it was ended by a reset */
  if (ret)
    t->failed++;
  return ret;
}

/* nothing on the GPU or waiting but the file's own jobs */
static int
only_file (struct v3d_dev *v3d, int f)
{
  int ok = 1;
  unsigned s = plat_irq_off ();
  for (int q = 0; q < V3D_NQ && ok; q++)
    {
      if (q < V3D_HW_QUEUES && v3d->active[q] && v3d->active[q]->file != f)
	ok = 0;
      for (struct v3d_job *j = v3d->head[q]; j && ok; j = j->next)
	if (j->file != f)
	  ok = 0;
    }
  plat_irq_restore (s);
  return ok;
}

#define SLOT_BYTES ((CL_BYTES - 64u) / V3D_2D_SLOTS & ~63u)

/* Room for a list: 0, or the reason to leave the work to the CPU. Our
   own lists still running are fine (async): the new one queues behind
   them; anything else on the GPU and the CPU does it. */
static int
begin (struct v3d_dev *v3d)
{
  struct v3d_2d *t = &v3d->twod;
  if (t->pending && (t->pending >= V3D_2D_SLOTS || !only_file (v3d, t->file)))
    if (v3d_2d_sync (v3d))
      return -V3D_EIO;
  if (!t->pending && !v3d_idle (v3d))
    {
      t->busy++;			/* 3D work: the CPU does this one */
      return -V3D_EBUSY;
    }
  return 0;
}

static int
submit (struct v3d_dev *v3d, uint32_t at, uint32_t len, int wait)
{
  struct v3d_2d *t = &v3d->twod;
  plat_barrier ();			/* the list, and the CPU's screen writes */
  uint32_t handles[2] = { t->cl_handle, t->scr_handle };
  struct drm_v3d_submit_cl s = {
    .rcl_start = t->cl_gpu + at, .rcl_end = t->cl_gpu + at + len,
    .bo_handles = (uintptr_t) handles, .bo_handle_count = 2,
  };
  uint64_t t0 = plat_time_ns ();
  int ret = ioctl_ (v3d, t->file, DRM_IOCTL_V3D_SUBMIT_CL, &s);
  t->us_submit += (uint32_t) (plat_time_ns () - t0) / 1000u;
  if (ret)
    {
      t->failed++;
      return ret;
    }
  t->pending++;
  if (wait)
    ret = v3d_2d_sync (v3d);
  return ret;
}

typedef uint32_t build_fn (struct v3d_2d *t, uint8_t *cl, uint32_t cap, const void *arg);

/* Build into the next part of the buffer; a list too big for a part
   waits for everything and has the whole buffer */
static int
run (struct v3d_dev *v3d, build_fn *build, const void *arg, int wait)
{
  struct v3d_2d *t = &v3d->twod;
  int ret = begin (v3d);
  if (ret)
    return ret;
  uint32_t at = t->slot * SLOT_BYTES;
  uint64_t t0 = plat_time_ns ();
  uint32_t len = build (t, t->cl_cpu + at, SLOT_BYTES, arg);
  t->us_build += (uint32_t) (plat_time_ns () - t0) / 1000u;
  if (!len)
    {
      if (t->pending && v3d_2d_sync (v3d))
	return -V3D_EIO;
      at = 0;
      len = build (t, t->cl_cpu, t->cl_bytes - 64, arg);
      if (!len)
	return -V3D_ENOSPC;
      ret = submit (v3d, at, len, wait);
      if (!ret && t->pending)
	t->pending = V3D_2D_SLOTS;	/* the whole buffer is in use */
      return ret;
    }
  ret = submit (v3d, at, len, wait);
  if (!ret && t->pending)
    t->slot = (t->slot + 1) % V3D_2D_SLOTS;
  return ret;
}

/* the rectangle inside the mapped framestore */
static int
fits (struct v3d_2d *t, uint32_t off, uint32_t stride, uint32_t w, uint32_t h, uint32_t bpp)
{
  uint64_t span = (uint64_t) (h - 1) * stride + (uint64_t) w * bpp;
  return (uint64_t) w * bpp <= stride && off + span <= t->scr_bytes;
}

static uint32_t
build_copy (struct v3d_2d *t, uint8_t *cl, uint32_t cap, const void *arg)
{
  const struct v3d_2d_copy *c = arg;
  return v3d_2d_copy_rcl (cl, cap, t->cl_gpu, t->cl_gpu + t->cl_bytes - 64,
			  t->scr_gpu + c->src, t->scr_gpu + c->dst, c->stride,
			  c->width, c->height, c->bytes_pp == 4 ? 27 : 7, c->order);
}

int
v3d_2d_copy (struct v3d_dev *v3d, const struct v3d_2d_copy *c)
{
  struct v3d_2d *t = &v3d->twod;
  if (t->file < 0 || !t->scr_handle || !c->width || !c->height
      || (c->bytes_pp != 4 && c->bytes_pp != 2) || c->width > 0xFFFF || c->height > 0xFFFF
      || !fits (t, c->src, c->stride, c->width, c->height, c->bytes_pp)
      || !fits (t, c->dst, c->stride, c->width, c->height, c->bytes_pp))
    return -V3D_EINVAL;
  uint64_t t0 = plat_time_ns ();
  int ret = run (v3d, build_copy, c, c->wait);
  if (ret)
    return ret;
  t->copies++;
  if (c->order)
    t->scrolls++;
  t->tiles += ((c->width + TILE - 1) / TILE) * ((c->height + TILE - 1) / TILE);
  t->ns += (uint32_t) (plat_time_ns () - t0) / 1000u;
  return ret;
}

static uint32_t
build_fill (struct v3d_2d *t, uint8_t *cl, uint32_t cap, const void *arg)
{
  const struct v3d_2d_fill *f = arg;
  return v3d_2d_fill_rcl (cl, cap, t->cl_gpu + t->cl_bytes - 64, t->scr_gpu + f->dst,
			  f->stride, f->width, f->height, f->bytes_pp == 4 ? 27 : 7,
			  v3d_2d_rgba (f->pixel, f->bytes_pp));
}

int
v3d_2d_fill (struct v3d_dev *v3d, const struct v3d_2d_fill *f)
{
  struct v3d_2d *t = &v3d->twod;
  if (t->file < 0 || !t->scr_handle || !f->width || !f->height
      || (f->bytes_pp != 4 && f->bytes_pp != 2) || f->width > 0xFFFF || f->height > 0xFFFF
      || !fits (t, f->dst, f->stride, f->width, f->height, f->bytes_pp))
    return -V3D_EINVAL;
  uint64_t t0 = plat_time_ns ();
  int ret = run (v3d, build_fill, f, f->wait);
  if (ret)
    return ret;
  t->fills++;
  t->tiles += ((f->width + TILE - 1) / TILE) * ((f->height + TILE - 1) / TILE);
  t->ns += (uint32_t) (plat_time_ns () - t0) / 1000u;
  return ret;
}
