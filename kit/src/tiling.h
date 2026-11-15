/* tiling.h: where a pixel lands in the V3D's tiled image layouts, from
   Mesa's broadcom/common/v3d_tiling.c (MIT licence, Copyright (C) 2014-2017
   Broadcom): v3d_get_lt_pixel_offset and v3d_get_uif_pixel_offset, for
   4-byte pixels. Used to check the TFU's output.
   SPDX-License-Identifier: MIT */
#ifndef TILING_H
#define TILING_H
#include <stdint.h>

/* 4-byte pixels: a utile (64 bytes) is 4x4 pixels */
#define UTILE_W 4
#define UTILE_H 4

static inline uint32_t
utile_pixel_offset (uint32_t x, uint32_t y)
{
  return x * 4 + y * UTILE_W * 4;
}

/* lineartile: images one utile wide or tall */
static inline uint32_t
lt_pixel_offset (uint32_t x, uint32_t y)
{
  return 64 * (x / UTILE_W + y / UTILE_H)
	 + utile_pixel_offset (x & (UTILE_W - 1), y & (UTILE_H - 1));
}

static inline uint32_t
uif_pixel_offset (uint32_t image_h, uint32_t x, uint32_t y, int do_xor)
{
  const uint32_t log2_mb_w = 3, log2_mb_h = 3;	/* macroblocks of 8x8 */
  uint32_t mb_x = x >> log2_mb_w, mb_y = y >> log2_mb_h;
  uint32_t mb_pixel_x = x - (mb_x << log2_mb_w), mb_pixel_y = y - (mb_y << log2_mb_h);
  if (do_xor && (mb_x / 4) & 1)
    mb_y ^= 0x10;
  uint32_t mb_h = ((image_h + 7) & ~7u) >> log2_mb_h;
  uint32_t mb_id = ((mb_x / 4) * ((mb_h - 1) * 4)) + mb_x + mb_y * 4;
  uint32_t top = mb_pixel_y < UTILE_H, left = mb_pixel_x < UTILE_W;
  return mb_id * 256 + (!top) * 128 + (!left) * 64
	 + utile_pixel_offset (mb_pixel_x & (UTILE_W - 1), mb_pixel_y & (UTILE_H - 1));
}
#endif
