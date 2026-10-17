#!/bin/bash
# boottest.sh: extfmt /BOOT + a synthetic Windows tree + filler + extboot, for
# each ext flavour; the FAT view (read with mtools) must equal the ext files,
# and e2fsck/extchk must find the volume clean.
B=${B:-/tmp/claude-0/extbuild}; W=${W:-/tmp/claude-0/extbuild/boottest}
export MTOOLS_SKIP_CHECK=1
rm -rf $W; mkdir -p $W/t/WINNT/System32/drivers $W/t/WINNT/System32/config $W/t/WINNT/Fonts $W/t/WINNT/inf $W/fill
cd $W/t
head -c 156496 /dev/urandom > ntldr; head -c 26816 /dev/urandom > NTDETECT.COM
printf '[boot loader]\r\ndefault=multi(0)disk(0)rdisk(0)partition(1)\\WINNT\r\n' > boot.ini
for f in ntoskrnl.exe hal.dll kdcom.dll bootvid.dll c_1252.nls l_intl.nls; do head -c $((RANDOM*20+1000)) /dev/urandom > WINNT/System32/$f; done
for i in $(seq 1 150); do head -c $((RANDOM+100)) /dev/urandom > WINNT/System32/drivers/drv$i.sys; done
head -c 3000000 /dev/urandom > WINNT/System32/config/SYSTEM; head -c 2000000 /dev/urandom > WINNT/System32/config/system.alt
head -c 5000 /dev/urandom > WINNT/Fonts/vgaoem.fon; head -c 900 /dev/urandom > WINNT/inf/biosinfo.inf
for i in $(seq 1 60); do head -c $((RANDOM*30)) /dev/urandom > $W/fill/f$i.bin; done
rc=0
for v in "4 200" "3 200" "2 150" "4 300 /B:4096" "4 120 /O:^extent" "3 400 /B:2048"; do
  set -- $v; T=$1; MB=$2; shift 2
  cd $W; rm -f p.img d.img
  truncate -s ${MB}M p.img
  $B/extfmt p.img /T:$T /BOOT /Y /Q "$@" >/dev/null
  $B/exttool p.img copyin $W/fill /fill >/dev/null          # the start of the volume taken first
  $B/exttool p.img copyin $W/t / >/dev/null
  python3 - "$MB" <<'P'
import struct,sys
mb=int(sys.argv[1]); p=open('p.img','rb').read()
d=bytearray(512); d[446:462]=bytes([0x80,0,0,0,0x83,0,0,0])+struct.pack('<II',63,len(p)//512); d[510:512]=b'\x55\xaa'
f=open('d.img','wb'); f.write(d); f.seek(63*512); f.write(p)
P
  out=$($B/extboot d.img 2>&1) || { echo "== $v: extboot failed: $out"; rc=1; continue; }
  rm -rf fv; mkdir fv; mcopy -s -n -i d.img@@32256 '::/*' fv/ 2>/dev/null
  bad=0; n=0
  for f in $(cd fv; find . -type f); do
    n=$((n+1)); src=$(cd t; find . -ipath "$f" | head -1)
    cmp -s "fv/$f" "t/$src" || { bad=$((bad+1)); echo "   differs: $f"; }
  done
  dd if=d.img of=p.img bs=512 skip=63 status=none
  e=$(e2fsck -fn p.img >/dev/null 2>&1; echo $?); c=$($B/extchk p.img /Q >/dev/null 2>&1; echo $?)
  [ $n = 163 ] && [ $bad = 0 ] && [ $e = 0 ] && [ $c = 0 ] && s=OK || { s=FAIL; rc=1; }
  echo "== ext$T ${MB}M $*: $s ($n files in the view, $bad differ, e2fsck $e, extchk $c)"
done
exit $rc
