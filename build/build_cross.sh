#!/bin/sh
# EXTNT cross build: a native NT 3.1 - XP (i386) extnt.sys with GCC and the
# mingw-w64 DDK headers, no Visual C++; then the tools.
#
# Needs: gcc-mingw-w64-i686 (ships ddk headers), binutils, python3.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
SRC="$ROOT/src"
OUT="${1:-/tmp/extnt-cross}"
mkdir -p "$OUT"

GCC=i686-w64-mingw32-gcc
DLLTOOL=i686-w64-mingw32-dlltool
NM=i686-w64-mingw32-nm
INC="-I$SRC -I/usr/i686-w64-mingw32/include/ddk -I/usr/i686-w64-mingw32/include"
# -march=i486: NT 3.1 runs on 80486 (and 386) CPUs; no P6 instructions.
FLAGS="-c -ffreestanding -fno-stack-protector -fno-builtin -march=i486 -mtune=i486 -fno-strict-aliasing \
 -D_X86_ -DWIN32 -D_WIN32_WINNT=0x0600 -DNTDDI_VERSION=0x06000000 -DEXN_CROSS -DEXT_KERNEL -w $EXTRACFLAGS"
ENGINE_FILES="extsup extmap extext exthash extdir extfile extea extjbd extold"

echo "Compiling driver sources..."
rm -f "$OUT"/*.o
for f in "$SRC"/ext*.c; do
    b=$(basename "${f%.c}")
    O=-O1
    case " $ENGINE_FILES " in *" $b "*) O=-O2;; esac
    $GCC $FLAGS $O $INC "$f" -o "$OUT/$b.o"
done
$GCC $FLAGS -O2 $INC "$HERE/extcrt.c" -o "$OUT/extcrt.o"

echo "Building the ntoskrnl import library from the symbols used..."
$NM "$OUT"/*.o 2>/dev/null | awk '/ U /{print $2}' | grep '^__imp_' \
    | sed 's/^__imp_//; s/^_//' | sort -u > "$OUT/need.txt"
{ echo "LIBRARY ntoskrnl.exe"; echo "EXPORTS"; cat "$OUT/need.txt"; } > "$OUT/ntoskrnl.def"
$DLLTOOL --kill-at -d "$OUT/ntoskrnl.def" -l "$OUT/libntoskrnl_nt31.a"

if [ -f "$NT31_EXPORTS" ]; then
    echo "Checking imports against NT 3.1 ntoskrnl..."
    sed 's/@[0-9]*$//' "$OUT/need.txt" | while read s; do
        grep -qx "$s" "$NT31_EXPORTS" || echo "  NOT EXPORTED BY NT 3.1: $s"
    done
fi

echo "Linking extnt.sys (native, subsystem 3.10, base 0x10000)..."
$GCC -shared -nostdlib -nostartfiles \
    -Wl,--subsystem,native -Wl,--major-subsystem-version,3 -Wl,--minor-subsystem-version,10 \
    -Wl,--major-os-version,1 -Wl,--minor-os-version,0 \
    -Wl,--major-image-version,1 -Wl,--minor-image-version,0 \
    -Wl,--image-base,0x10000 -Wl,--entry,_DriverEntry@8 -Wl,--file-alignment,0x200 \
    -o "$OUT/extnt.sys" "$OUT"/*.o "$OUT/libntoskrnl_nt31.a" -lgcc

cp "$OUT/extnt.sys" "$OUT/extnt.dbg"
i686-w64-mingw32-strip --strip-all "$OUT/extnt.sys"
python3 "$HERE/pechecksum.py" "$OUT/extnt.sys"

echo "Built: $OUT/extnt.sys"
[ -n "$DRIVER_ONLY" ] && exit 0

# ----------------------------------------------------------------------
# The user-mode tools: freestanding console programs (subsystem 3.10) so
# they run on NT 3.1 with no C-runtime DLL; toolcrt.c is the tiny runtime.
# ----------------------------------------------------------------------
EXEBASE="-O2 -w -ffreestanding -nostdlib -nostartfiles -march=i486 -mtune=i486 -e _mainCRTStartup \
 -Wl,--subsystem,console -Wl,--major-subsystem-version,3 -Wl,--minor-subsystem-version,10 \
 -Wl,--major-os-version,1 -Wl,--minor-os-version,0 -D_WIN32 -D_CONSOLE"
TOOLCRT="-include $HERE/toolcrt.h \
 -D_INC_STDIO -D_INC_STDLIB -D_INC_STRING -D_INC_CTYPE -D_INC_TIME \
 -D_STDIO_H_=1 -D_STDLIB_H=1 -D_STRING_H_=1 -D_CTYPE_H_=1 -D_TIME_H_=1 \
 -Dmain=appmain -Dprintf=ext_printf -Dfprintf=ext_fprintf -Dsprintf=ext_sprintf -Dsnprintf=ext_snprintf -Dvsnprintf=ext_vsnprintf \
 -Dmalloc=ext_malloc -Dfree=ext_free -Dcalloc=ext_calloc -Drealloc=ext_realloc \
 -Dstrtoul=ext_strtoul -Dtime=ext_time -Dlocaltime=ext_localtime -Dtoupper=ext_toupper -D_stricmp=ext_stricmp"
ENGINE="$SRC/extsup.c $SRC/extmap.c $SRC/extext.c $SRC/exthash.c $SRC/extdir.c $SRC/extfile.c $SRC/extea.c $SRC/extjbd.c $SRC/extold.c $ROOT/tools/extimg.c"
TINC="-I$SRC -I$ROOT/tools -I$ROOT/fmt -I$ROOT/chk -I$ROOT/boot -I$OUT"

echo "Linking extfmt.exe..."
$GCC $EXEBASE $TOOLCRT $TINC -o "$OUT/extfmt.exe" "$ROOT/fmt/extfmt.c" "$ROOT/fmt/extmkfs.c" $ENGINE "$HERE/toolcrt.c" -lkernel32 -lgcc
echo "Assembling the boot loader..."
gcc -E -P -x assembler-with-cpp "$ROOT/boot/extboot.S" > "$OUT/extboot.s"
as --32 -o "$OUT/extboot.o" "$OUT/extboot.s"
ld -m elf_i386 -Ttext 0x7C00 --oformat binary -o "$OUT/extboot.bin" "$OUT/extboot.o"
rm -f "$OUT/extboot.o" "$OUT/extboot.s"
[ "$(stat -c %s "$OUT/extboot.bin")" = 1024 ] || { echo "extboot.bin is not 1024 bytes"; exit 1; }
python3 "$HERE/bin2h.py" "$OUT/extboot.bin" ExtBootCode > "$OUT/extbootbin.h"
echo "Linking extboot.exe..."
$GCC $EXEBASE $TOOLCRT $TINC -o "$OUT/extboot.exe" "$ROOT/fmt/extboot.c" "$SRC/extshim.c" $ENGINE "$HERE/toolcrt.c" -lkernel32 -lgcc
echo "Linking extchk.exe..."
$GCC $EXEBASE $TOOLCRT $TINC -o "$OUT/extchk.exe" "$ROOT/chk/extchk.c" "$ROOT/chk/extchkc.c" $ENGINE "$HERE/toolcrt.c" -lkernel32 -lgcc
echo "Linking extutil.exe..."
$GCC $EXEBASE $TOOLCRT -I"$SRC" -o "$OUT/extutil.exe" "$ROOT/tools/extutil.c" "$HERE/toolcrt.c" -lntdll -lkernel32 -lgcc
echo "Linking extinst.exe..."
$GCC $EXEBASE $TOOLCRT -o "$OUT/extinst.exe" "$ROOT/inst/extinst.c" "$HERE/toolcrt.c" -ladvapi32 -lkernel32 -lgcc

if [ -f "$NT31_USER_EXPORTS" ]; then
    echo "Checking the tools' imports against NT 3.1..."
    for f in "$OUT"/*.exe; do
        i686-w64-mingw32-objdump -p "$f" | awk '/^\t[0-9a-f]+\t +[0-9]+ +[A-Za-z_]/{print $3}' | while read s; do
            grep -qx "$s" "$NT31_USER_EXPORTS" || echo "  $(basename "$f"): $s is not in NT 3.1"
        done
    done
fi
echo "Done."
