/* clbuild.c: see clbuild.h.

   Packets are packed field by field, little-endian bit order, as Mesa's
   generated packers do: field (start, size) puts its value at bit
   "start" of the bytes after the opcode. Fields marked minus_one in
   v3d_packet.xml are given here already less one.

   SPDX-License-Identifier: GPL-2.0-or-later
   Copyright (c) 2026 Andrew Youll */
#include "clbuild.h"

#define TILE 64				/* 32 bpp, 1 RT, no MSAA, no double buffer */
#define INITIAL_BLOCK 128		/* V3D_TILE_ALLOC_INITIAL_BLOCK_SIZE */
#define MAX_SUPERTILES 256

/* opcodes (v3d_packet.xml, V3D 4.2) */
enum
{
  FLUSH = 4, START_TILE_BINNING = 6, WAIT_ON_SEMAPHORE = 8, END_OF_RENDERING = 13,
  RETURN_FROM_SUB_LIST = 18, FLUSH_VCD_CACHE = 19,
  START_ADDRESS_OF_GENERIC_TILE_LIST = 20, BRANCH_TO_IMPLICIT_TILE_LIST = 21,
  SUPERTILE_COORDINATES = 23, CLEAR_TILE_BUFFERS = 25, END_OF_LOADS = 26,
  END_OF_TILE_MARKER = 27, STORE_TILE_BUFFER_GENERAL = 29, SET_INSTANCEID = 54,
  PRIM_LIST_FORMAT = 56, OCCLUSION_QUERY_COUNTER = 92,
  TILE_BINNING_MODE_CFG = 120, TILE_RENDERING_MODE_CFG = 121,
  MULTICORE_RENDERING_SUPERTILE_CFG = 122,
  MULTICORE_RENDERING_TILE_LIST_SET_BASE = 123, TILE_COORDINATES = 124,
  TILE_COORDINATES_IMPLICIT = 125, TILE_LIST_INITIAL_BLOCK_SIZE = 126,
};

struct cl { uint8_t *p; uint32_t n; };
struct field { uint32_t start, size; uint64_t value; };

static void
emit (struct cl *cl, uint8_t opcode, uint32_t bytes, const struct field *f, int nf)
{
  uint8_t *out = cl->p + cl->n;
  out[0] = opcode;
  for (uint32_t i = 1; i < bytes; i++)
    out[i] = 0;
  for (int k = 0; k < nf; k++)
    for (uint32_t b = 0; b < f[k].size; b++)
      if ((f[k].value >> b) & 1)
	{
	  uint32_t bit = f[k].start + b;
	  out[1 + bit / 8] |= (uint8_t) (1u << (bit % 8));
	}
  cl->n += bytes;
}

#define EMIT(cl, op, bytes, ...) \
  do { const struct field _f[] = { { 0, 0, 0 }, __VA_ARGS__ }; \
       emit (cl, op, bytes, _f, sizeof _f / sizeof _f[0]); } while (0)
#define EMIT0(cl, op) emit (cl, op, 1, 0, 0)

void
clear_job_sizes (uint32_t width, uint32_t height, uint32_t *alloc, uint32_t *state)
{
  uint32_t tiles = ((width + TILE - 1) / TILE) * ((height + TILE - 1) / TILE);
  uint32_t a = (tiles * INITIAL_BLOCK + 4095) & ~4095u;
  *alloc = a + 8192;			/* the PTB's first two 4K chunks */
  *state = tiles * 256;
}

void
clear_job_build (struct clear_job *j)
{
  struct cl bcl = { j->bcl, 0 }, rcl = { j->rcl, 0 }, gen = { j->gen, 0 };
  j->tiles_x = (j->width + TILE - 1) / TILE;
  j->tiles_y = (j->height + TILE - 1) / TILE;

  /* ---- the BCL (start_binning, bcl_epilogue) ---- */
  if (j->hang)
    {
      /* for the reset test: the binner waits for a semaphore nothing
	 releases, so its current address stays put and V3DRender's
	 progress check sees no progress */
      EMIT0 (&bcl, WAIT_ON_SEMAPHORE);
      j->bcl_len = bcl.n;
    }
  else
    {
      EMIT (&bcl, TILE_BINNING_MODE_CFG, 9,
	    { 2, 2, INITIAL_BLOCK >> 7 },	/* tile allocation initial block size */
	    { 4, 2, 64 >> 7 },			/* tile allocation block size */
	    { 8, 4, 0 },			/* number of render targets - 1 */
	    { 12, 2, 0 },			/* maximum BPP: 32 */
	    { 32, 16, j->width - 1 },
	    { 48, 16, j->height - 1 });
      EMIT0 (&bcl, FLUSH_VCD_CACHE);
      EMIT (&bcl, OCCLUSION_QUERY_COUNTER, 5, { 0, 32, 0 });
      EMIT0 (&bcl, START_TILE_BINNING);
      EMIT0 (&bcl, FLUSH);
      j->bcl_len = bcl.n;
    }

  /* ---- the generic tile list (v3d_rcl_emit_generic_per_tile_list) ---- */
  EMIT0 (&gen, TILE_COORDINATES_IMPLICIT);
  EMIT0 (&gen, END_OF_LOADS);			/* nothing to load */
  EMIT (&gen, PRIM_LIST_FORMAT, 2, { 0, 6, 4 });	/* triangles */
  EMIT (&gen, SET_INSTANCEID, 5, { 0, 32, 0 });
  EMIT (&gen, BRANCH_TO_IMPLICIT_TILE_LIST, 2, { 0, 8, 0 });
  EMIT (&gen, STORE_TILE_BUFFER_GENERAL, 13,
	{ 0, 4, 0 },				/* render target 0 */
	{ 4, 3, 0 },				/* raster */
	{ 12, 6, 27 },				/* rgba8 */
	{ 28, 20, j->stride },			/* byte stride (raster) */
	{ 64, 32, j->target });
  EMIT (&gen, CLEAR_TILE_BUFFERS, 2, { 0, 1, 1 }, { 1, 1, 1 });
  EMIT0 (&gen, END_OF_TILE_MARKER);
  EMIT0 (&gen, RETURN_FROM_SUB_LIST);
  j->gen_len = gen.n;

  /* ---- the RCL (emit_rcl, emit_render_layer) ---- */
  EMIT (&rcl, TILE_RENDERING_MODE_CFG, 9,		/* common */
	{ 0, 4, 0 },				/* sub-id */
	{ 4, 4, 0 },				/* render targets - 1 */
	{ 8, 16, j->width },
	{ 24, 16, j->height },
	{ 40, 2, 0 },				/* maximum BPP: 32 */
	{ 46, 1, 1 });				/* early-Z disable */
  EMIT (&rcl, TILE_RENDERING_MODE_CFG, 9,		/* clear colours part 1 */
	{ 0, 4, 3 },
	{ 4, 4, 0 },				/* render target 0 */
	{ 8, 32, j->colour });
  EMIT (&rcl, TILE_RENDERING_MODE_CFG, 9,		/* colour */
	{ 0, 4, 1 },
	{ 4, 2, 0 },				/* RT 0: 32 bpp */
	{ 6, 4, 2 },				/* RT 0: internal type 8 */
	{ 10, 2, 0 });				/* RT 0: no clamp */
  EMIT (&rcl, TILE_RENDERING_MODE_CFG, 9,		/* Z/S clear values: ends the config */
	{ 0, 4, 2 },
	{ 8, 8, 0 },
	{ 16, 32, 0x3f800000 });		/* Z = 1.0 */
  EMIT (&rcl, TILE_LIST_INITIAL_BLOCK_SIZE, 2,
	{ 0, 2, INITIAL_BLOCK >> 7 }, { 2, 1, 1 });	/* auto-chained */
  EMIT (&rcl, MULTICORE_RENDERING_TILE_LIST_SET_BASE, 5,
	{ 6, 26, j->tile_alloc >> 6 });

  /* supertiles: as few as the hardware allows (Mesa's loop) */
  uint32_t sw = 1, sh = 1, fw, fh;
  for (;;)
    {
      fw = (j->tiles_x + sw - 1) / sw;
      fh = (j->tiles_y + sh - 1) / sh;
      if (fw < MAX_SUPERTILES && fh < MAX_SUPERTILES && fw * fh <= MAX_SUPERTILES)
	break;
      if (sw < sh)
	sw++;
      else
	sh++;
    }
  j->supertile_w = sw;
  j->supertile_h = sh;
  EMIT (&rcl, MULTICORE_RENDERING_SUPERTILE_CFG, 9,
	{ 0, 8, sw - 1 }, { 8, 8, sh - 1 },
	{ 16, 8, fw }, { 24, 8, fh },
	{ 32, 12, j->tiles_x }, { 44, 12, j->tiles_y },
	{ 61, 3, 0 });				/* bin tile lists - 1 */

  /* the initial clear, twice for GFXH-1742 */
  for (int i = 0; i < 2; i++)
    {
      EMIT (&rcl, TILE_COORDINATES, 4, { 0, 12, 0 }, { 12, 12, 0 });
      EMIT0 (&rcl, END_OF_LOADS);
      EMIT (&rcl, STORE_TILE_BUFFER_GENERAL, 13, { 0, 4, 8 });	/* none */
      if (i == 0)
	EMIT (&rcl, CLEAR_TILE_BUFFERS, 2, { 0, 1, 1 }, { 1, 1, 1 });
      EMIT0 (&rcl, END_OF_TILE_MARKER);
    }
  EMIT0 (&rcl, FLUSH_VCD_CACHE);
  EMIT (&rcl, START_ADDRESS_OF_GENERIC_TILE_LIST, 9,
	{ 0, 32, j->gen_gpu }, { 32, 32, j->gen_gpu + j->gen_len });
  for (uint32_t y = 0; y < fh; y++)
    for (uint32_t x = 0; x < fw; x++)
      EMIT (&rcl, SUPERTILE_COORDINATES, 3, { 0, 8, x }, { 8, 8, y });
  EMIT0 (&rcl, END_OF_RENDERING);
  j->rcl_len = rcl.n;
}
