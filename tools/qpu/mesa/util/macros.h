#pragma once
#include <assert.h>
#include <stddef.h>
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define unreachable(s) __builtin_unreachable()
#define UNREACHABLE(s) __builtin_unreachable()
#define MAYBE_UNUSED __attribute__((unused))
#define ASSERTED __attribute__((unused))
#define BITFIELD64_MASK(b) ((b) == 64 ? ~0ull : (1ull << (b)) - 1)
#define BITFIELD_BIT(b) (1u << (b))
#define BITFIELD64_BIT(b) (1ull << (b))
#define UNUSED __attribute__((unused))
#define STATIC_ASSERT(c) _Static_assert(c, #c)
#define MAX2(a,b) ((a)>(b)?(a):(b))
#define MIN2(a,b) ((a)<(b)?(a):(b))
#define ATTRIBUTE_CONST __attribute__((const))
#define ATTRIBUTE_PURE __attribute__((pure))
#define BITFIELD64_RANGE(b, count) (BITFIELD64_MASK((b) + (count)) & ~BITFIELD64_MASK(b))
