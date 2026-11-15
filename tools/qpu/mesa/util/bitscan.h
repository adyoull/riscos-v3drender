#pragma once
static inline int ffs_(unsigned v){return __builtin_ffs(v);}
#define u_bit_scan(m) ({int _i=__builtin_ctz(*(m)); *(m)&=~(1u<<_i); _i;})
