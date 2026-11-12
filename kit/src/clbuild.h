/* clbuild.h: control lists for V3DRender's first GPU jobs (V3D 4.2).

   A frame that only clears its one colour buffer: the bin control list
   (BCL) sets up tile binning and draws nothing; the render control list
   (RCL) clears each 64x64 tile to a colour and stores it to a raster
   RGBA8 image. Packet by packet this is what Mesa's v3d driver emits for
   such a frame (v3dx_draw.c start_binning and bcl_epilogue, v3dx_rcl.c
   emit_rcl), with the packets' layouts from Mesa's v3d_packet.xml.

   All addresses are GPU addresses (BO offsets from V3DRender).

   SPDX-License-Identifier: GPL-2.0-or-later
   Copyright (c) 2026 Andrew Youll */
#ifndef CLBUILD_H
#define CLBUILD_H
#include <stdint.h>

struct clear_job
{
  /* in */
  uint32_t width, height, stride;	/* pixels, pixels, bytes */
  uint32_t colour;			/* R in the low byte .. A in the top */
  uint32_t target;			/* the image */
  uint32_t tile_alloc, tile_state;	/* the binner's memory */
  uint8_t *bcl, *rcl, *gen;		/* CPU addresses of the lists */
  uint32_t bcl_gpu, rcl_gpu, gen_gpu;	/* and their GPU addresses */
  int hang;				/* a BCL that never finishes */
  /* out */
  uint32_t bcl_len, rcl_len, gen_len;
  uint32_t tiles_x, tiles_y, supertile_w, supertile_h;
};

/* the binner's memory a frame needs (Mesa's v3d_tile_alloc_sizes, no draws) */
void clear_job_sizes (uint32_t width, uint32_t height,
		      uint32_t *tile_alloc_bytes, uint32_t *tile_state_bytes);
void clear_job_build (struct clear_job *j);

#endif
