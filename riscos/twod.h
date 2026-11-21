/* twod.h: GraphicsV for the desktop's rectangle copies (0.17) */
#ifndef TWOD_H
#define TWOD_H

#include <stdint.h>

#define TWOD_OFF	0
#define TWOD_ON		1		/* each copy waited for */
#define TWOD_ASYNC	2		/* left running until GraphicsV asks */

/* why the screen can't be used */
#define TWOD_NO_VDU		1
#define TWOD_DEPTH		2		/* not 16 or 32bpp */
#define TWOD_GEOMETRY		3
#define TWOD_NO_FRAMESTORE	4		/* GraphicsV 9 unanswered */
#define TWOD_BANK		5		/* VDU and display banks differ */
#define TWOD_NO_MAP		6
#define TWOD_CACHED		7		/* the CPU caches the screen */

struct twod_state
{
  int mode, async, claimed_vector, busy;
  void *pw;
  int screen_ok, screen_why;
  uint32_t log2bpp, stride, xlim, ylim, base_off;
  int vrender_known;
  uint32_t vrender, cache_suspended;
  uint32_t offered, claimed, scrolls, small, irqs_off, disabled, no_screen, errors, last_error;
  int fills;				/* fills too (*V3DRender 2D fills on|off) */
  uint32_t stopped;			/* turned off by a GPU failure */
  uint32_t fs_pa, fs_size, vis;		/* (0.18) the framestore, for bank flips */
  int mapped;				/* screen() got as far as mapping it */
  uint32_t bank_flips;
  uint32_t lim[5];			/* (0.18) *V3DRender 2D limits */
  uint32_t us_claimed, us_left;		/* (0.19) in GraphicsV: ops claimed, ops left */
  int alongside;			/* (0.21) 2D even while a program has the GPU open */
  uint32_t prog_open;			/* (0.21) left: a program had the GPU open */
  uint32_t fills_offered, fills_claimed, fills_small, fills_pattern;
};
extern struct twod_state twod;

struct v3d_dev;
void twod_limits (const uint32_t *v, int n);
void twod_zero (void);
void twod_init (struct v3d_dev *dev, void *pw);
oserror *twod_set (int mode);
void twod_final (void);
void twod_mode_changing (void);
void twod_mode_changed (void);
void twod_files_closed (void);
int gfxv_c (uint32_t *r);

#endif
