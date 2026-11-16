/* v3djob_riscos.c: V3DJob's calls on RISC OS: V3DRender's SWIs (by
   number, provisional), the monotonic clock and *V3DRender.
   SPDX-License-Identifier: GPL-2.0-or-later
   Copyright (c) 2026 Andrew Youll */
#include <stdio.h>
#include <string.h>
#include <kernel.h>
#include <swis.h>
#include "v3djob.h"

#define V3DRender_Info  0xC3D40
#define V3DRender_Open  0xC3D41
#define V3DRender_Close 0xC3D42
#define V3DRender_Ioctl 0xC3D43

const char *
sys_info (unsigned *version, unsigned *v3d, unsigned *hz)
{
  _kernel_oserror *e = _swix (V3DRender_Info, _OUT (0) | _OUT (1) | _OUT (3), version, v3d, hz);
  return e ? e->errmess : 0;
}

const char *
sys_open (int *file)
{
  _kernel_oserror *e = _swix (V3DRender_Open, _IN (0) | _OUT (0), 0, file);
  return e ? e->errmess : 0;
}

void
sys_close (int file)
{
  _swix (V3DRender_Close, _IN (0), file);
}

int
sys_ioctl (int file, uint32_t request, void *arg, const char **err)
{
  int r = -1;
  _kernel_oserror *e = _swix (V3DRender_Ioctl, _INR (0, 2) | _OUT (0), file, request, arg, &r);
  *err = e ? e->errmess : 0;
  return r;
}

unsigned
now_cs (void)
{
  unsigned t = 0;
  _swix (OS_ReadMonotonicTime, _OUT (0), &t);
  return t;
}

#define RISC3D_Info  0xC3D00
#define RISC3D_Open  0xC3D02
#define RISC3D_Close 0xC3D03

uint32_t
sys_pte (uint32_t gpu)
{
  unsigned pte = 0;
  if (_swix (0xC3D45, _INR (0, 1) | _OUT (0), 0, gpu, &pte))
    return 0;
  return pte;
}

int
sys_reset (int all)
{
  return _swix (0xC3D44, _IN (0), all) ? -1 : 0;
}

unsigned
sys_ram_mb (void)
{
  unsigned pages = 0, size = 0;
  if (_swix (OS_Memory, _IN (0) | _OUT (1) | _OUT (2), 8 | 1 << 8, &pages, &size))
    return 0;
  return (unsigned) ((unsigned long long) pages * size >> 20);
}

int
sys_place_at (uint32_t gpu)
{
  return _swix (0xC3D45, _INR (0, 1), 2, gpu) ? -1 : 0;
}

int
sys_pa_width (void)
{
  unsigned va = 0, pa = 0;
  if (_swix (0xC3D45, _IN (0) | _OUT (0) | _OUT (1), 1, &va, &pa))
    return 0;
  return (int) pa;
}

const uint32_t *
sys_stats (void)
{
  static const uint32_t none[32];
  const uint32_t *p = none;
  if (_swix (V3DRender_Info, _OUT (8), &p) || !p)
    return none;
  return p;
}

const char *
sys_risc3d_info (const char **name, unsigned *flags, unsigned *ioctl_swi, const char **drm)
{
  unsigned version;
  _kernel_oserror *e = _swix (RISC3D_Info, _OUT (0) | _OUT (3) | _OUT (4) | _OUT (5) | _OUT (6),
			      &version, name, flags, ioctl_swi, drm);
  if (!e && version < 2)
    return "it is older than 0.02";
  return e ? e->errmess : 0;
}

const char *
sys_risc3d_open (int *context, int *file)
{
  _kernel_oserror *e = _swix (RISC3D_Open, _IN (0) | _OUT (0) | _OUT (1), 0, context, file);
  return e ? e->errmess : 0;
}

void
sys_risc3d_close (int context)
{
  _swix (RISC3D_Close, _IN (0), context);
}

void
sys_status (const char *also_to)
{
  char cmd[300];
  _kernel_oscli ("V3DRender");
  if (also_to && strlen (also_to) < 250)
    {
      sprintf (cmd, "V3DRender { >> %s }", also_to);
      _kernel_oscli (cmd);
    }
}
