/* cltest.c: builds the kit's clear jobs on the build machine and writes
   the lists out, for cldecode.py to check against Mesa's v3d_packet.xml.
   SPDX-License-Identifier: GPL-2.0-or-later
   Copyright (c) 2026 Andrew Youll */
#include <stdio.h>
#include <stdlib.h>
#include "clbuild.h"

static void
put (const char *dir, const char *name, int w, const uint8_t *p, uint32_t n)
{
  char path[256];
  snprintf (path, sizeof path, "%s/%d-%s.bin", dir, w, name);
  FILE *f = fopen (path, "wb");
  fwrite (p, 1, n, f);
  fclose (f);
}

int
main (int argc, char **argv)
{
  (void) argc;
  static const int sizes[][2] = { { 128, 128 }, { 1920, 1080 }, { 100, 37 } };
  for (int s = 0; s < 3; s++)
    {
      static uint8_t bcl[4096], rcl[65536], gen[4096];
      struct clear_job j = { .width = sizes[s][0], .height = sizes[s][1],
			     .stride = sizes[s][0] * 4, .colour = 0x44332211,
			     .target = 0x00400000, .tile_alloc = 0x00800000,
			     .tile_state = 0x00900000, .bcl = bcl, .rcl = rcl,
			     .gen = gen, .bcl_gpu = 0x00201000, .rcl_gpu = 0x00211000,
			     .gen_gpu = 0x00221000 };
      clear_job_build (&j);
      uint32_t a, st;
      clear_job_sizes (j.width, j.height, &a, &st);
      printf ("%d %d tiles %u %u supertile %u %u alloc %u state %u lens %u %u %u\n",
	      j.width, j.height, j.tiles_x, j.tiles_y, j.supertile_w, j.supertile_h,
	      a, st, j.bcl_len, j.rcl_len, j.gen_len);
      put (argv[1], "bcl", j.width, bcl, j.bcl_len);
      put (argv[1], "rcl", j.width, rcl, j.rcl_len);
      put (argv[1], "gen", j.width, gen, j.gen_len);
    }
  return 0;
}
