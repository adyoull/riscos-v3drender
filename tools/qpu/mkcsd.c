/* mkcsd.c: assembles V3DJob's compute test program with Mesa's own QPU
   packer (mesa/, MIT licence) and checks each word with Mesa's
   disassembler. Writes kit/src/csd_prog.h.

   The program (V3D 4.2, one thread): load a value and an address from
   the uniform stream, write the value there through the TMU (all 16
   lanes write the same word), wait for the write, end. Thread end is
   the last thread switch and its two delay slots, as Mesa ends shaders.

   SPDX-License-Identifier: GPL-2.0-or-later
   Copyright (c) 2026 Andrew Youll */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "qpu_instr.h"
#include "qpu_disasm.h"

static struct v3d_device_info dev = { .ver = 42 };

static struct v3d_qpu_instr
nop (void)
{
  struct v3d_qpu_instr i;
  memset (&i, 0, sizeof i);
  i.type = V3D_QPU_INSTR_TYPE_ALU;
  i.alu.add.op = V3D_QPU_A_NOP;
  i.alu.mul.op = V3D_QPU_M_NOP;
  i.alu.add.waddr = V3D_QPU_WADDR_NOP;
  i.alu.mul.waddr = V3D_QPU_WADDR_NOP;
  i.alu.add.magic_write = true;
  i.alu.mul.magic_write = true;
  return i;
}

int
main (int argc, char **argv)
{
  struct v3d_qpu_instr p[16];
  int n = 0;
  struct v3d_qpu_instr i;

  i = nop (); i.sig.ldunif = true; p[n++] = i;		/* r5 = the value */
  p[n++] = nop ();
  i = nop (); i.alu.mul.op = V3D_QPU_M_MOV;		/* tmud = r5 */
  i.alu.mul.waddr = V3D_QPU_WADDR_TMUD; i.alu.mul.a.mux = V3D_QPU_MUX_R5; p[n++] = i;
  i = nop (); i.sig.ldunif = true; p[n++] = i;		/* r5 = the address */
  p[n++] = nop ();
  i = nop (); i.alu.mul.op = V3D_QPU_M_MOV;		/* tmua = r5: the write */
  i.alu.mul.waddr = V3D_QPU_WADDR_TMUA; i.alu.mul.a.mux = V3D_QPU_MUX_R5; p[n++] = i;
  i = nop (); i.alu.add.op = V3D_QPU_A_TMUWT; p[n++] = i;	/* wait for it */
  i = nop (); i.sig.thrsw = true; p[n++] = i;		/* the last thread switch */
  p[n++] = nop ();
  p[n++] = nop ();

  FILE *f = fopen (argv[1], "w");
  fprintf (f, "/* csd_prog.h: made by tools/qpu/mkcsd (Mesa's QPU packer), V3D 4.2.\n"
	      "   Uniforms: the value, then the address to write it to. */\n"
	      "static const uint64_t csd_prog[] = {\n");
  for (int k = 0; k < n; k++)
    {
      uint64_t w;
      if (!v3d_qpu_instr_pack (&dev, &p[k], &w))
	{
	  fprintf (stderr, "instruction %d won't pack\n", k);
	  return 1;
	}
      const char *d = v3d_qpu_disasm (&dev, w);
      fprintf (f, "  0x%016llxull,\t/* %s */\n", (unsigned long long) w, d);
      printf ("%2d %016llx  %s\n", k, (unsigned long long) w, d);
    }
  fprintf (f, "};\n");
  fclose (f);
  (void) argc;
  return 0;
}
