#!/bin/bash
# builds exttool for Linux into $OUT (default /tmp/claude-0/extbuild)
D=$(cd "$(dirname "$0")/.." && pwd); OUT=${OUT:-/tmp/claude-0/extbuild}; mkdir -p "$OUT"
cc -O1 -g -Wall -Wno-unused-variable -I"$D/src" -I"$D/tools" "$D"/src/ext{sup,map,ext,hash,dir,file,ea,jbd,old}.c \
   "$D/tools/extimg.c" "$D/tools/exttool.c" -o "$OUT/exttool" ${CFLAGS:-}
# the format/check/boot tools, for images
E="$D/src/extsup.c $D/src/extmap.c $D/src/extext.c $D/src/exthash.c $D/src/extdir.c $D/src/extfile.c $D/src/extea.c $D/src/extjbd.c $D/src/extold.c $D/tools/extimg.c"
I="-I$D/src -I$D/tools -I$D/fmt -I$D/chk -I$OUT"
gcc -E -P -x assembler-with-cpp "$D/boot/extboot.S" > "$OUT/extboot.s" && as --32 -o "$OUT/extboot.o" "$OUT/extboot.s" &&
  ld -m elf_i386 -Ttext 0x7C00 --oformat binary -o "$OUT/extboot.bin" "$OUT/extboot.o" &&
  python3 "$D/build/bin2h.py" "$OUT/extboot.bin" ExtBootCode > "$OUT/extbootbin.h"
cc -O1 -g -w $I $E "$D/fmt/extfmt.c" "$D/fmt/extmkfs.c" -o "$OUT/extfmt" ${CFLAGS:-}
cc -O1 -g -w $I $E "$D/chk/extchk.c" "$D/chk/extchkc.c" -o "$OUT/extchk" ${CFLAGS:-}
cc -O1 -g -Wall -Wno-unused-variable -Wno-pointer-sign $I $E "$D/fmt/extboot.c" "$D/src/extshim.c" -o "$OUT/extboot" ${CFLAGS:-}
cc -O1 -Wall -o "$OUT/mkext1" "$D/tools/mkext1.c" ${CFLAGS:-}
