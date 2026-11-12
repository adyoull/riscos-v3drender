/* plat_pi4.h: the Pi 4 platform's own calls, for module.c.
   SPDX-License-Identifier: GPL-2.0-or-later
   Copyright (c) 2026 Andrew Youll */
#ifndef PLAT_PI4_H
#define PLAT_PI4_H
#include <stdint.h>

struct pmp { unsigned area; uint8_t *base; uint32_t pages; };

int pi4_soc (void);			/* 3 = BCM2711; -1 = no answer */
oserror *pi4_map (void);
oserror *pi4_power_on (int *was_up);
int pi4_v3d_is_up (void);
oserror *pi4_memory (struct v3d_dev *v3d);
void pi4_memory_free (void);
oserror *pi4_clock (uint32_t *hz, uint32_t *max_hz);
oserror *pi4_set_clock (uint32_t hz, uint32_t *now);
void pmp_handler (void);		/* the veneer (header.s) */
extern int pi4_no_wfi;

#define PI4_LOG 8
struct pi4_log { const char *msg; uint32_t value; };
extern struct pi4_log pi4_log[PI4_LOG];
extern unsigned pi4_log_next;

#endif
