/* v3d_present.c: DRM_IOCTL_RISCOS_PRESENT (0.31)

   EGL's swap for a window on the RISC OS desktop: the program's finished
   frame (a BO) copied to the screen by the GPU, rectangle by rectangle
   (the Wimp's redraw rectangles), as render jobs on the program's own
   queue, so they run after its frame and before its next. No shaders,
   no binning: as the desktop's copies (v3d_2d.c), each 64x64 tile is
   loaded from the source (Load Tile Buffer General) and stored to the
   screen (Store Tile Buffer General); the formats are changed on the
   way (the load's and the store's own formats and R/B swaps).

   A raster source is loaded from any pixel. A UIF source (Mesa's tiled
   layout) can only be loaded from its start, so it is first untiled,
   whole, into a raster staging buffer, and the rectangles are copied
   from that.

   Independent of the desktop's 2D (*V3DRender 2D): its own file for its
   lists, its own mapping of the screen and its staging buffer, closed
   with the last program.

   Copyright (C) 2026 Andrew Youll. GNU GPL version 2 or later. */

#include "v3d.h"

#define CL_BYTES (256u * 1024u)
#define SLOT_BYTES ((CL_BYTES - 64u) / V3D_PRESENT_SLOTS & ~63u)
#define TILE 64u
#define TILE_BYTES 32u			/* a tile's packets (v3d_2d.c's COPY_TILE) */
#define HEAD_BYTES 64u			/* the frame's header (52) and the end */

/* the Load/Store General word's fields (Mesa's v3d_packet.xml) */
#define G_SWAP (1ull << 20)
#define G_ALPHA_1 (1ull << 18)		/* (load) alpha forced to 1 */
#define G_FORMAT(f) ((uint64_t) ((f) & 0x3F) << 12)
#define G_MEMORY(m) ((uint64_t) ((m) & 7) << 4)
#define G_STRIDE(s) ((uint64_t) ((s) & 0xFFFFF) << 28)	/* or height in UIF blocks */
#define MEM_RASTER 0
#define MEM_UIF 4
#define MEM_UIF_XOR 5
#define FMT_RGBA8 27
#define FMT_BGR565 7
#define FMT_RGBA5551 45

/* a / b without libgcc's divide (whose division by zero wants raise()) */
static uint32_t
udiv (uint32_t a, uint32_t b)
{
  uint32_t q = 0;
  while (a >= b)
    {
      uint32_t d = b, bit = 1;
      while (!(d & 0x80000000u) && (d << 1) <= a)
	d <<= 1, bit <<= 1;
      a -= d;
      q |= bit;
    }
  return q;
}

static int
ioctl_ (struct v3d_dev *v3d, int f, uint32_t request, void *arg)
{
  return v3d_ioctl (v3d, f, request, arg);
}

static int
start (struct v3d_dev *v3d)
{
  struct v3d_present *p = &v3d->present;
  if (p->file >= 0)
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
  p->file = f;
  p->cl_handle = c.handle;
  p->cl_gpu = c.offset;
  p->cl_cpu = v3d_bo_cpu (v3d, v3d->files[f].handles[c.handle]);
  p->slot = 0;
  p->at = 0;
  for (int i = 0; i < V3D_PRESENT_SLOTS; i++)
    p->slot_fence[i].seq = 0;
  p->scr_handle = p->stage_handle = 0;
  p->scr_bytes = p->stage_bytes = 0;
  return 0;
}

static void
forget (struct v3d_present *p)
{
  p->file = -1;
  p->cl_cpu = 0;
  p->scr_handle = p->stage_handle = 0;
  p->scr_bytes = p->stage_bytes = 0;
}

/* the last program closed: the jobs still running hold what they use */
void
v3d_present_stop (struct v3d_dev *v3d)
{
  struct v3d_present *p = &v3d->present;
  int f = p->file;
  if (f < 0)
    return;
  forget (p);
  v3d_close (v3d, f);
}

/* every file was closed (*V3DReset all) */
void
v3d_present_files_closed (struct v3d_dev *v3d)
{
  forget (&v3d->present);
}

/* every job PRESENT has queued finished (up to a second) */
static int
drain (struct v3d_dev *v3d)
{
  struct v3d_present *p = &v3d->present;
  int64_t deadline = v3d_now_ns () + 1000000000ll;
  return v3d_wait_fences (v3d, p->slot_fence, V3D_PRESENT_SLOTS, 1, deadline, 0);
}

static void
close_handle (struct v3d_dev *v3d, uint32_t *handle)
{
  if (*handle)
    {
      struct drm_gem_close g = { .handle = *handle };
      ioctl_ (v3d, v3d->present.file, DRM_IOCTL_GEM_CLOSE, &g);
      *handle = 0;
    }
}

/* the framestore mapped for the GPU (from the 1 MB boundary below it, as
   2D's), kept until the screen moves */
static int
map_screen (struct v3d_dev *v3d, const struct v3d_screen *s)
{
  struct v3d_present *p = &v3d->present;
  if (p->scr_handle && s->fs_pa == p->scr_pa && s->fs_bytes <= p->scr_bytes)
    return 0;
  if (p->scr_handle)
    {
      drain (v3d);			/* nothing left writing to the old one */
      close_handle (v3d, &p->scr_handle);
      p->scr_bytes = 0;
    }
  uint64_t from = s->fs_pa & ~(uint64_t) 0xFFFFF;
  uint64_t to = (s->fs_pa + s->fs_bytes + 0xFFFF) & ~(uint64_t) 0xFFFF;
  if (to - from > 0x40000000u)
    return -V3D_EINVAL;
  uint32_t h = v3d_handle_slot (v3d, p->file);
  if (!h)
    return -V3D_ENOMEM;
  int bo, ret = v3d_bo_import_phys (v3d, from, (uint32_t) (to - from), &bo);
  if (ret)
    return ret;
  v3d->bos[bo].file = (int16_t) p->file;
  v3d->files[p->file].handles[h] = (uint16_t) bo;
  p->scr_handle = h;
  p->scr_gpu = v3d_bo_offset (v3d, bo) + (uint32_t) (s->fs_pa - from);
  p->scr_pa = s->fs_pa;
  p->scr_bytes = s->fs_bytes;
  return 0;
}

/* the staging buffer, at least `bytes` (grown in MB) */
static int
staging (struct v3d_dev *v3d, uint32_t bytes)
{
  struct v3d_present *p = &v3d->present;
  if (p->stage_handle && bytes <= p->stage_bytes)
    return 0;
  close_handle (v3d, &p->stage_handle);	/* (jobs using it keep it) */
  p->stage_bytes = 0;
  struct drm_v3d_create_bo c = { .size = (bytes + 0xFFFFF) & ~0xFFFFFu };
  int ret = ioctl_ (v3d, p->file, DRM_IOCTL_V3D_CREATE_BO, &c);
  if (ret)
    return ret;
  p->stage_handle = c.handle;
  p->stage_gpu = c.offset;
  p->stage_bytes = c.size;
  return 0;
}

/* Room for a list of up to `need` bytes: its offset in the list buffer.
   The next part of the buffer is taken when this one is full, once its
   last job has finished. */
static int
room (struct v3d_dev *v3d, uint32_t need, uint32_t *off)
{
  struct v3d_present *p = &v3d->present;
  if (need > SLOT_BYTES)
    return -V3D_E2BIG;
  if (p->at + need > SLOT_BYTES)
    {
      p->slot = (p->slot + 1) % V3D_PRESENT_SLOTS;
      p->at = 0;
      int64_t deadline = v3d_now_ns () + 2000000000ll;
      int ret = v3d_wait_fences (v3d, &p->slot_fence[p->slot], 1, 1, deadline, 0);
      if (ret)
	return ret;
      p->slot_fence[p->slot].seq = 0;
    }
  *off = p->slot * SLOT_BYTES + p->at;
  return 0;
}

struct frame
{
  uint32_t src, dst;			/* GPU addresses of the frame's origin */
  uint64_t lg, sg;			/* the load's and the store's General words */
  uint32_t width, height;
  uint16_t bos[4];
  uint32_t nbos;
};

/* a frame's jobs, as many as its tiles need, on file f */
static int
frame_jobs (struct v3d_dev *v3d, int f, const struct frame *fr,
	    uint32_t in_sync, uint32_t out_sync)
{
  struct v3d_present *p = &v3d->present;
  uint32_t tx = (fr->width + TILE - 1) / TILE, ty = (fr->height + TILE - 1) / TILE;
  uint32_t per = udiv ((SLOT_BYTES - HEAD_BYTES) / TILE_BYTES, tx);	/* tile rows a job */
  if (!per)
    return -V3D_E2BIG;
  for (uint32_t row = 0; row < ty; row += per)
    {
      uint32_t rows = ty - row < per ? ty - row : per;
      uint32_t off, need = HEAD_BYTES + tx * rows * TILE_BYTES;
      int ret = room (v3d, need, &off);
      if (ret)
	return ret;
      uint32_t len = v3d_present_rcl (p->cl_cpu + off, need, p->cl_gpu + CL_BYTES - 64,
				      fr->src, fr->lg, fr->dst, fr->sg,
				      fr->width, fr->height, row, rows);
      if (!len)
	return -V3D_EINVAL;
      plat_barrier ();			/* the list in memory */
      ret = v3d_submit_render (v3d, f, p->cl_gpu + off, p->cl_gpu + off + len,
			       fr->bos, fr->nbos, in_sync, out_sync, &p->slot_fence[p->slot]);
      if (ret)
	return ret;
      p->at += (len + 63) & ~63u;
      p->jobs++;
    }
  return 0;
}

/* the source's format: bytes a pixel, the GPU's format, R/B swap */
static int
source_format (uint32_t fourcc, uint32_t *bpp, uint32_t *fmt, uint32_t *swap)
{
  switch (fourcc)
    {
    case DRM_FORMAT_ABGR8888:
    case DRM_FORMAT_XBGR8888:
      *bpp = 4, *fmt = FMT_RGBA8, *swap = 0;
      return 0;
    case DRM_FORMAT_ARGB8888:
    case DRM_FORMAT_XRGB8888:
      *bpp = 4, *fmt = FMT_RGBA8, *swap = 1;
      return 0;
    case DRM_FORMAT_RGB565:
      *bpp = 2, *fmt = FMT_BGR565, *swap = 0;	/* R in the top bits, as BGR565 */
      return 0;
    }
  return -V3D_EINVAL;
}

static int64_t
min64 (int64_t a, int64_t b)
{
  return a < b ? a : b;
}

struct clipped
{
  uint32_t dx, dy, sx, sy, w, h;
};

/* a rectangle clipped to the source (sw x sh) and the screen: 0 if
   nothing is left */
static int
clip (const struct drm_riscos_present_rect *r, uint32_t sw, uint32_t sh,
      const struct v3d_screen *s, struct clipped *c)
{
  int64_t dx = r->dst_x, dy = r->dst_y, sx = r->src_x, sy = r->src_y;
  int64_t w = r->width, h = r->height;
  if (sx < 0) dx -= sx, w += sx, sx = 0;
  if (sy < 0) dy -= sy, h += sy, sy = 0;
  if (dx < 0) sx -= dx, w += dx, dx = 0;
  if (dy < 0) sy -= dy, h += dy, dy = 0;
  w = min64 (w, min64 ((int64_t) s->width - dx, (int64_t) sw - sx));
  h = min64 (h, min64 ((int64_t) s->height - dy, (int64_t) sh - sy));
  if (w <= 0 || h <= 0)
    return 0;
  c->dx = (uint32_t) dx, c->dy = (uint32_t) dy, c->sx = (uint32_t) sx, c->sy = (uint32_t) sy;
  c->w = (uint32_t) w, c->h = (uint32_t) h;
  return 1;
}

static int
present (struct v3d_dev *v3d, int f, struct drm_riscos_present *a)
{
  struct v3d_present *p = &v3d->present;
  if (!v3d->present_screen)
    return -V3D_ENOTTY;
  if (a->flags & ~(DRM_RISCOS_PRESENT_SRC_UIF | DRM_RISCOS_PRESENT_VSYNC
		   | DRM_RISCOS_PRESENT_SRC_UIF_XOR) || a->pad)
    return -V3D_EINVAL;
  if (a->flags & DRM_RISCOS_PRESENT_VSYNC)
    return -V3D_EOPNOTSUPP;		/* not without waiting */
  if (a->rect_count > DRM_RISCOS_PRESENT_MAX_RECTS)
    return -V3D_EINVAL;

  /* the screen, as it is now */
  struct v3d_screen s;
  memset (&s, 0, sizeof s);
  int ret = v3d->present_screen (v3d, &s);
  if (ret)
    return ret;
  uint32_t dbpp, dfmt;
  switch (s.kind)
    {
    case V3D_SCREEN_RGBA8: dbpp = 4, dfmt = FMT_RGBA8; break;
    case V3D_SCREEN_565: dbpp = 2, dfmt = FMT_BGR565; break;
    case V3D_SCREEN_5551: dbpp = 2, dfmt = FMT_RGBA5551; break;
    default: return -V3D_EINVAL;
    }
  if (!s.width || !s.height || s.width > 0xFFFF || s.height > 0xFFFF
      || (uint64_t) s.width * dbpp > s.stride || s.stride > 0xFFFFF
      || (uint64_t) s.off + (uint64_t) s.height * s.stride > s.fs_bytes)
    return -V3D_EINVAL;
  if (s.cached)
    return -V3D_EBUSY;			/* the CPU's to do, this time */

  /* the source */
  int uif = (a->flags & (DRM_RISCOS_PRESENT_SRC_UIF | DRM_RISCOS_PRESENT_SRC_UIF_XOR)) != 0;
  uint32_t sbpp = 0, sfmt = 0, sswap = 0;
  int bo = 0;
  if (a->handle || a->rect_count)
    {
      bo = v3d_lookup_bo (v3d, f, a->handle);
      if (!bo)
	return -V3D_ENOENT;
      if (source_format (a->format, &sbpp, &sfmt, &sswap)
	  || !a->width || !a->height || a->width > 0xFFFF || a->height > 0xFFFF)
	return -V3D_EINVAL;
      uint64_t bytes = (uint64_t) v3d->bos[bo].npages * V3D_PAGE, need;
      uint32_t v = a->uif_height_or_stride;
      if (uif)
	{
	  /* padded to whole UIF blocks (8 rows) and columns (4 blocks:
	     32 pixels at 4 bytes, 64 at 2) */
	  uint32_t col = sbpp == 4 ? 32 : 64;
	  if (v < a->height || (v & 7) || (v >> 3) > 0xFFFFF)
	    return -V3D_EINVAL;
	  need = (uint64_t) ((a->width + col - 1) & ~(col - 1)) * v * sbpp;
	}
      else
	{
	  if ((uint64_t) a->width * sbpp > v || v > 0xFFFFF)
	    return -V3D_EINVAL;
	  need = (uint64_t) (a->height - 1) * v + (uint64_t) a->width * sbpp;
	}
      if (need > bytes)
	return -V3D_EINVAL;
    }
  if (!a->rect_count)
    {
      p->probes++;
      return 0;
    }

  const struct drm_riscos_present_rect *r =
    (const struct drm_riscos_present_rect *) (uintptr_t) a->rects;
  struct clipped c;
  uint32_t left = 0;
  for (uint32_t i = 0; i < a->rect_count; i++)
    if (clip (&r[i], a->width, a->height, &s, &c))
      left++;
    else
      p->clipped_away++;
  if (!left)
    {
      v3d_signal_syncobj (v3d, f, a->out_sync);	/* nothing to draw: done */
      p->presents++;
      return 0;
    }

  ret = start (v3d);
  if (!ret)
    ret = map_screen (v3d, &s);
  if (ret)
    return ret;
  int cl_bo = v3d_lookup_bo (v3d, p->file, p->cl_handle);
  int scr_bo = v3d_lookup_bo (v3d, p->file, p->scr_handle);
  uint32_t src_gpu = v3d_bo_offset (v3d, bo), src_stride = a->uif_height_or_stride;
  /* (0.32) R/B swap only on a load in RGBA8. The GPU doesn't swap a
     3-channel format (BGR565) at all: Mesa's v3dv says so, and 0.31's
     store swap into a 5:6:5 TBGR screen did nothing (MESA's Pi test). So
     the source's swap and the screen's are combined and done on a load
     of 4 channels; an RGB565 source that needs one goes through the
     staging buffer first, as a UIF source does. */
  uint32_t swap = sswap ^ s.rb_swap;
  uint64_t lg = G_FORMAT (sfmt) | G_ALPHA_1;
  uint64_t sg = G_FORMAT (dfmt) | G_STRIDE (s.stride);
  uint32_t scr = p->scr_gpu + s.off;
  int src_bo = bo;

  /* UIF, or RGB565 to swap: copied whole to the staging buffer as RGBA8
     (alpha forced on the way in); the rectangles come from there */
  if (uif || (sfmt == FMT_BGR565 && swap))
    {
      ret = staging (v3d, a->width * 4u * a->height);
      if (ret)
	return ret;
      int stage_bo = v3d_lookup_bo (v3d, p->file, p->stage_handle);
      struct frame fr = {
	.src = src_gpu, .dst = p->stage_gpu,
	.lg = lg | (uif ? G_MEMORY ((a->flags & DRM_RISCOS_PRESENT_SRC_UIF_XOR) ? MEM_UIF_XOR : MEM_UIF)
		    | G_STRIDE (src_stride >> 3) : G_STRIDE (src_stride)),
	.sg = G_FORMAT (FMT_RGBA8) | G_STRIDE (a->width * 4u),
	.width = a->width, .height = a->height,
	.bos = { (uint16_t) cl_bo, (uint16_t) bo, (uint16_t) stage_bo }, .nbos = 3,
      };
      ret = frame_jobs (v3d, f, &fr, a->in_sync, 0);
      if (ret)
	return ret;
      p->untiled++;
      src_gpu = p->stage_gpu;
      src_stride = a->width * 4u;
      sbpp = 4;
      lg = G_FORMAT (FMT_RGBA8);
      src_bo = stage_bo;
    }
  lg |= G_STRIDE (src_stride) | (swap ? G_SWAP : 0);

  for (uint32_t i = 0; i < a->rect_count; i++)
    {
      if (!clip (&r[i], a->width, a->height, &s, &c))
	continue;
      struct frame fr = {
	.src = src_gpu + c.sy * src_stride + c.sx * sbpp,
	.dst = scr + c.dy * s.stride + c.dx * dbpp,
	.lg = lg, .sg = sg, .width = c.w, .height = c.h,
	.bos = { (uint16_t) cl_bo, (uint16_t) scr_bo, (uint16_t) src_bo }, .nbos = 3,
      };
      ret = frame_jobs (v3d, f, &fr, a->in_sync, a->out_sync);
      if (ret)
	return ret;
      p->rects++;
    }
  p->presents++;
  return 0;
}

int
v3d_present (struct v3d_dev *v3d, int f, struct drm_riscos_present *args)
{
  v3d_service (v3d);
  int ret = present (v3d, f, args);
  if (ret && ret != -V3D_ENOTTY)
    {
      v3d->present.refused++;
      v3d->present.last_error = (uint32_t) -ret;
    }
  return ret;
}
