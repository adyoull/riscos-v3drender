/* stdint.h for the module (the tool chain's freestanding headers have
   none): GCC's own type macros.
   SPDX-License-Identifier: GPL-2.0-or-later
   Copyright (c) 2026 Andrew Youll */
#ifndef V3DR_STDINT_H
#define V3DR_STDINT_H
typedef __INT8_TYPE__ int8_t;
typedef __UINT8_TYPE__ uint8_t;
typedef __INT16_TYPE__ int16_t;
typedef __UINT16_TYPE__ uint16_t;
typedef __INT32_TYPE__ int32_t;
typedef __UINT32_TYPE__ uint32_t;
typedef __INT64_TYPE__ int64_t;
typedef __UINT64_TYPE__ uint64_t;
typedef __UINTPTR_TYPE__ uintptr_t;
#define INT64_MAX __INT64_MAX__
#endif
