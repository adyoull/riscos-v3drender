/* csd_prog.h: made by tools/qpu/mkcsd (Mesa's QPU packer), V3D 4.2.
   Uniforms: the value, then the address to write it to. */
static const uint64_t csd_prog[] = {
  0x3c403186bb800000ull,	/* nop                           ; nop                         ; ldunif */
  0x3c003186bb800000ull,	/* nop                           ; nop */
  0x3c0032c6bbf40000ull,	/* nop                           ; mov tmud, r5 */
  0x3c403186bb800000ull,	/* nop                           ; nop                         ; ldunif */
  0x3c003186bb800000ull,	/* nop                           ; nop */
  0x3c003306bbf40000ull,	/* nop                           ; mov tmua, r5 */
  0x3c003186bb815000ull,	/* tmuwt -                       ; nop */
  0x3c203186bb800000ull,	/* nop                           ; nop                         ; thrsw */
  0x3c003186bb800000ull,	/* nop                           ; nop */
  0x3c003186bb800000ull,	/* nop                           ; nop */
};
