#!/bin/bash
# build.sh: builds V3DRender into out/.
#   out/V3DRender,ffa   the module (RISC OS 5, Raspberry Pi 4)
#   out/sim             the core's host test, with a simulated V3D (run it)
# RISCOS_TOOLCHAIN: riscos-crossdev's toolchain (1.11).
#
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 Andrew Youll
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p out/obj
CORE="core/v3d_hw.c core/v3d_sched.c core/v3d_bo.c core/v3d_ioctl.c"
INC="-Iinclude -Icore -Icore/compat"

# the host test, with the address and undefined-behaviour checkers
gcc -std=gnu11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined $INC \
  -o out/sim test/host/sim.c $CORE
out/sim

T=${RISCOS_TOOLCHAIN:?set RISCOS_TOOLCHAIN}/bin/arm-riscos-gnueabihf-
MC="-std=gnu11 -O2 -marm -march=armv7-a -mno-unaligned-access -fpie -fvisibility=hidden \
 -mgeneral-regs-only -ffreestanding -fno-common -fno-stack-protector \
 -fno-asynchronous-unwind-tables -fno-unwind-tables -fno-builtin \
 -nostdinc -isystem $(${T}gcc -print-file-name=include) -Wall -Wextra -Werror -Icommon -Iriscos $INC"
ML="-nostdlib -static -Wl,--no-dynamic-linker -Wl,-T,common/module.ld -Wl,--build-id=none -Wl,-z,norelro"
${T}as -I common -o out/obj/header.o riscos/header.s
objs="out/obj/header.o"
for c in common/cmodule.c riscos/module.c riscos/plat_pi4.c $CORE; do
  o=out/obj/$(basename "${c%.c}").o
  ${T}gcc $MC -c -o "$o" "$c"
  objs="$objs $o"
done
${T}gcc $MC $ML -o out/obj/V3DRender.elf $objs -lgcc
${T}gcc $MC $ML -Wl,--defsym=__link_base=0x10000 -o out/obj/V3DRender-64k.elf $objs -lgcc
${T}objcopy -O binary out/obj/V3DRender.elf out/obj/V3DRender-0.bin
${T}objcopy -O binary out/obj/V3DRender-64k.elf out/obj/V3DRender-1.bin
python3 -B common/check-pic.py out/obj/V3DRender-0.bin out/obj/V3DRender-1.bin 0x10000 \
  0x$(${T}nm out/obj/V3DRender.elf | awk '$3=="__got_start"{print $1}') \
  0x$(${T}nm out/obj/V3DRender.elf | awk '$3=="__got_end"{print $1}')
cp out/obj/V3DRender-0.bin "out/V3DRender,ffa"

# the module itself in Unicorn, with a stand-in Pi 4 (pip install unicorn)
gcc -Iinclude -Icore -Icore/compat -o out/obj/regs test/riscos/regs.c
out/obj/regs > out/obj/regs.json
if python3 -c "import unicorn" 2>/dev/null; then
  python3 -B test/riscos/test_module.py "out/V3DRender,ffa" out/obj/regs.json | tail -1
else
  echo "(unicorn isn't installed: the module's emulator test was skipped)"
fi

# the compute test's GPU program, assembled with Mesa's QPU packer (must
# match the copy in kit/src)
gcc -D_GNU_SOURCE -std=gnu11 -Itools/qpu/mesa -o out/mkcsd tools/qpu/mkcsd.c \
  tools/qpu/mesa/qpu_instr.c tools/qpu/mesa/qpu_pack.c tools/qpu/mesa/qpu_disasm.c
out/mkcsd out/csd_prog.h > /dev/null
cmp out/csd_prog.h kit/src/csd_prog.h

# the kit's first GPU jobs: the control lists checked against Mesa's
# packet definitions, then V3DJob run against the core with a GPU that
# carries out its frames, then built for RISC OS
mkdir -p out/cl
gcc -std=gnu11 -Wall -Wextra -Werror -Ikit/src -o out/cltest test/host/cltest.c kit/src/clbuild.c
out/cltest out/cl > /dev/null
python3 -B test/host/cldecode.py test/host/v3d_packet.xml out/cl | tail -1
gcc -std=gnu11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined $INC -Ikit/src \
  -o out/jobhost kit/src/v3djob.c kit/src/clbuild.c test/host/jobhost.c $CORE
out/jobhost | grep -E "passed|failed"
out/jobhost hang | grep -E "passed|failed"
AC="-std=gnu11 -O2 -mfpu=vfpv3 -mfloat-abi=hard -Wall -Wextra -Werror"
${T}gcc $AC -Iinclude -Ikit/src -static -o out/obj/v3djob.elf kit/src/v3djob.c \
  kit/src/v3djob_riscos.c kit/src/clbuild.c
${T}strip out/obj/v3djob.elf
"${RISCOS_TOOLCHAIN}/bin/elf2aif" -e out/obj/v3djob.elf "out/obj/V3DJob,ff8" > /dev/null

# the Pi test kit
cp "out/V3DRender,ffa" out/obj/
python3 tools/mkkit.py out/V3DRenderTest.zip V3DRenderTest kit/ReadMe,fff kit/Check,feb \
  kit/HangTest,feb kit/V3DCheck,fd1 "out/obj/V3DJob,ff8" "out/obj/V3DRender,ffa" "kit/RISC3D,ffa"
ls -la "out/V3DRender,ffa" out/V3DRenderTest.zip
# (delivered kits are numbered: V3DRenderTest-2.zip was 0.02's)
