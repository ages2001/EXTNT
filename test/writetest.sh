#!/bin/bash
# EXTNT engine write test: runs a series of operations on copies of the
# reference images through exttool and checks the result with e2fsck -fn.
set -u
T=${T:-/tmp/claude-0/extbuild/exttool}
REF=${REF:-/tmp/claude-0/ext}
W=${W:-/tmp/claude-0/extbuild}
cd "$W" || exit 1
head -c 300000 /dev/urandom > big.bin; echo hello > small.txt
fail=0
for img in ${IMGS:-ref2 ref3 ref4}; do
  I=w$img.img; cp "$REF/$img.img" "$I"
  run() { "$T" "$I" "$@" || { echo "FAILED: $*"; fail=1; }; }
  run mkdir /d1; run mkdir /d1/sub; run put small.txt /d1/a.txt; run put big.bin /d1/sub/big.bin
  run symlink /d1/a.txt /fastlink; run symlink "$(printf 'x%.0s' {1..100})" /slowlink
  run ln /d1/a.txt /hard.txt; run setea /d1/a.txt user color blue; run mv /d1/sub /moved
  run truncate /moved/big.bin 5000; run put big.bin /d1/b.bin; run rm /d1/b.bin
  out=$(e2fsck -fn "$I" 2>&1); rc=$?
  if [ $rc -ne 0 ]; then echo "== $img: e2fsck rc=$rc"; echo "$out" | sed -n '2,40p'; fail=1; else echo "== $img: e2fsck clean"; fi
  "$T" "$I" get /moved/big.bin out.bin; cmp -s <(head -c 5000 big.bin) out.bin && echo "   data OK" || { echo "   DATA MISMATCH"; fail=1; }
  [ "$("$T" "$I" readlink /slowlink)" = "$(printf 'x%.0s' {1..100})" ] && echo "   slow symlink OK" || { echo "   symlink bad"; fail=1; }
  debugfs -R "ea_get /d1/a.txt user.color" "$I" 2>/dev/null | grep -q blue && echo "   xattr OK (debugfs)" || { echo "   xattr not seen by debugfs"; fail=1; }
done
exit $fail
