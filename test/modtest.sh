#!/bin/bash
# EXTNT: modify Linux-made images (htree dirs, inline dirs/files, hard links,
# xattrs) through exttool, mirror the same changes on a local copy of the
# tree, then require e2fsck -fn clean and an identical tree.
set -u
T=${T:-/tmp/claude-0/extbuild/exttool}; W=${W:-/tmp/claude-0/extbuild}
cd "$W" || exit 1
fail=0
for n in ${IMGS:-ext2 ext3 ext4 ext4i ext4nc}; do
  I=M$n.img; cp L$n.img $I; rm -rf mt; cp -a tree mt
  r() { "$T" $I "$@" >/dev/null || { echo "  FAILED: $*"; fail=1; }; }
  echo new > newf.txt; head -c 70000 /dev/urandom > mid.bin
  for i in $(seq 1 40); do r put newf.txt /big/new_$i.txt; cp newf.txt mt/big/new_$i.txt; done
  for i in $(seq 1 200 3000); do r rm /big/f_$i.txt; rm mt/big/f_$i.txt; done
  r mv /big/f_2.txt /sub/moved.txt; mv mt/big/f_2.txt mt/sub/moved.txt
  r mkdir /sub/newdir; mkdir mt/sub/newdir
  r put mid.bin /sub/newdir/mid.bin; cp mid.bin mt/sub/newdir/mid.bin
  r mv /sub/deep /big/deep; mv mt/sub/deep mt/big/deep
  r rm /hardlink.bin; rm mt/hardlink.bin
  r setea /big/deep/blob.bin user second 2; r truncate /big/deep/blob.bin 1234567; truncate -s 1234567 mt/big/deep/blob.bin
  r mv /big/f_3.txt /big/renamed_in_place.txt; mv mt/big/f_3.txt mt/big/renamed_in_place.txt
  "$T" $I rmdir /sub/newdir >/dev/null 2>&1 && { echo "  rmdir of a non-empty dir SUCCEEDED"; fail=1; }
  out=$(e2fsck -fn $I 2>&1); rc=$?
  if [ $rc -ne 0 ]; then echo "== $n: e2fsck rc=$rc"; echo "$out" | sed -n '2,30p'; fail=1; else echo "== $n: e2fsck clean"; fi
  c=$("$T" $I cmp mt | tail -1); echo "   $c"; [ "$c" = "cmp: 0 difference(s)" ] || { "$T" $I cmp mt | head; fail=1; }
  # Linux must find our entries in the (now plain) directories
  debugfs -R "stat /big/new_7.txt" $I 2>/dev/null | grep -q "Type: regular" && echo "   debugfs finds new entries" || { echo "   debugfs lookup FAILED"; fail=1; }
  debugfs -R "ls -l /big/deep/.." $I >/dev/null 2>&1; dd=$(debugfs -R "stat /big/deep" $I 2>/dev/null | head -1)
  debugfs -R "ea_get /big/deep/blob.bin user.test" $I 2>/dev/null | grep -q hello && echo "   old xattr kept" || { echo "   old xattr LOST"; fail=1; }
done
exit $fail
