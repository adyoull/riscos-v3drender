/* v3d_plat.h: what the core needs from the machine it runs on. The
   RISC OS module (riscos/plat_pi4.c) and the host test (test/host)
   provide these.

   SPDX-License-Identifier: GPL-2.0-or-later
   Copyright (c) 2026 Andrew Youll */
#ifndef V3D_PLAT_H
#define V3D_PLAT_H

struct v3d_dev;

/* registers */
uint32_t v3d_rd (struct v3d_dev *v3d, int bank, uint32_t off);
void v3d_wr (struct v3d_dev *v3d, int bank, uint32_t off, uint32_t val);

/* BO pages: claim and map (writable by programs, not cached, bufferable)
   or unmap and release pages first .. first+n-1 of the area. */
int plat_pages_claim (struct v3d_dev *v3d, uint32_t first, uint32_t n);
void plat_pages_release (struct v3d_dev *v3d, uint32_t first, uint32_t n);
uint64_t plat_page_pa (struct v3d_dev *v3d, uint32_t page);	/* after claim */

/* the GPU's own reset line (Linux: reset_control_reset, or the bridge) */
void plat_gpu_reset (struct v3d_dev *v3d);

uint32_t plat_time_cs (void);		/* a monotonic centisecond clock */
void plat_barrier (void);		/* the CPU's writes are in memory */
unsigned plat_irq_off (void);
void plat_irq_restore (unsigned state);
void plat_wait (void);			/* until an interrupt, or a moment */
void *plat_alloc (size_t bytes);	/* zeroed; not with interrupts off */
void plat_free (void *p);
void plat_log (struct v3d_dev *v3d, const char *msg, uint32_t value);

#endif
