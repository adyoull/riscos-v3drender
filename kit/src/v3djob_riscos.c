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
