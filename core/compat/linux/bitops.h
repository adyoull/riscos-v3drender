/* Just enough of Linux's <linux/bitops.h> and friends for v3d_regs.h,
   which is used exactly as it is in the Linux kernel.
   SPDX-License-Identifier: GPL-2.0-or-later
   Copyright (c) 2026 Andrew Youll. */
#ifndef V3DR_COMPAT_BITOPS_H
#define V3DR_COMPAT_BITOPS_H
#include <stdint.h>
typedef uint32_t u32;
typedef uint64_t u64;
#define BIT(n) (1u << (n))
#define GENMASK(h, l) (((~0u) >> (31 - (h))) & ((~0u) << (l)))
#define WARN_ON(x) ((void) (x))
#endif
