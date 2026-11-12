/* v3djob.h: what V3DJob needs from the machine (v3djob_riscos.c on
   RISC OS, test/host/jobhost.c on the build machine). Errors are
   messages; 0 is success.
   SPDX-License-Identifier: GPL-2.0-or-later
   Copyright (c) 2026 Andrew Youll */
#ifndef V3DJOB_H
#define V3DJOB_H
#include <stdint.h>
const char *sys_info (unsigned *version, unsigned *v3d, unsigned *hz);
const char *sys_open (int *file);
void sys_close (int file);
int sys_ioctl (int file, uint32_t request, void *arg, const char **err);
unsigned now_cs (void);
void sys_status (const char *also_to);	/* *V3DRender, and to a file */
#endif
