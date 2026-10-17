#!/bin/bash
# EXTNT checker test: damage images in known ways (debugfs, dd), then
#   e2fsck -fn must see the damage, extchk must see it, extchk /F must
#   repair it so that e2fsck -fn and extchk find nothing afterwards.
B=${B:-/tmp/claude-0/extbuild}; C=$B/extchk; W=$B/chk; mkdir -p $W; cd $W || exit 1
if [ ! -d tree ]; then
  mkdir -p tree/a/b/c tree/many tree/x
  for i in $(seq 1 120); do echo "file $i" > tree/many/f$i; done
  head -c 300000 /dev/urandom > tree/a/b/c/blob; echo small > tree/a/s.txt; echo other > tree/x/o.txt
  head -c 70000 /dev/urandom > tree/a/mid.bin
fi
fail=0; n=0
ino() { debugfs -R "stat $2" $1 2>/dev/null | sed -n 's/^Inode: \([0-9]*\).*/\1/p'; }
blk() { debugfs -R "bmap $2 $3" $1 2>/dev/null | tail -1; }
run() {   # name type damage-commands...
  local name=$1 type=$2; shift 2
  local I=$name.img; rm -f $I
  mke2fs -q -F -t $type ${MKOPT:-} -d tree $I 32M >/dev/null 2>&1
  for c in "$@"; do eval "$c"; done
  n=$((n+1))
  e2fsck -fn $I >e1.txt 2>&1; e1=$?
  $C $I > c1.txt 2>&1; c1=$?
  $C $I /F > c2.txt 2>&1; c2=$?
  e2fsck -fn $I > e2.txt 2>&1; e2=$?
  $C $I > c3.txt 2>&1; c3=$?
  st="ok"
  [ $e1 -eq 0 ] && st="(e2fsck saw nothing)"
  [ $e1 -ne 0 ] && [ $c1 -eq 0 ] && { st="MISSED"; fail=1; }
  [ $e2 -ne 0 ] && { st="$st NOT-REPAIRED"; fail=1; }
  [ $c3 -ne 0 ] && { st="$st RECHECK-FAILS"; fail=1; }
  printf "%-28s %-24s extchk:%s/%s e2fsck-after:%s\n" "$name" "$st" $c1 $c2 $e2
  if [ "$st" != "ok" ] && [ -n "$VERB" ]; then echo "--- e1"; sed -n 2,8p e1.txt; echo "--- c1"; head -8 c1.txt; echo "--- c2"; head -12 c2.txt; echo "--- e2"; sed -n 2,12p e2.txt; fi
}
D() { debugfs -w -R "$2" $1 >/dev/null 2>&1; }
for type in ext2 ext3 ext4; do
  run $type-freeb $type 'D $I "freeb $(blk $I /a/mid.bin 3)"'
  run $type-setb $type 'D $I "setb 7000"'
  run $type-freei $type 'D $I "freei <$(ino $I /x/o.txt)>"'
  run $type-seti $type 'D $I "seti <900>"'
  run $type-links $type 'D $I "sif /a/s.txt links_count 5"'
  run $type-dirlinks $type 'D $I "sif /a links_count 9"'
  run $type-unlink $type 'D $I "unlink /a/s.txt"'
  run $type-unlinkdir $type 'D $I "unlink /a/b"'
  run $type-clri $type 'D $I "clri /x/o.txt"'
  run $type-extralink $type 'D $I "link /a/s.txt /x/s2"'
  run $type-gdcount $type 'D $I "set_bg 0 free_blocks_count 3"' 'D $I "set_bg 0 checksum calc"'
  run $type-sbcount $type 'D $I "ssv free_blocks_count 12"' 'D $I "ssv free_inodes_count 3"'
  run $type-dotdot $type 'D $I "link /a/b /x/b"' 'D $I "unlink /a/b"'
  run $type-nolf $type 'D $I "rmdir lost+found"' 'D $I "unlink /x/o.txt"'
  run $type-mode $type 'D $I "sif /x/o.txt mode 0170644"'
  run $type-size $type 'D $I "sif /a/mid.bin size 10"'
  run $type-iblocks $type 'D $I "sif /a/mid.bin blocks 3"'
  run $type-dirgarbage $type 'dd if=/dev/urandom of=$I bs=$(dumpe2fs -h $I 2>/dev/null | awk "/^Block size/{print \$3}") seek=$(blk $I /x 0) count=1 conv=notrunc status=none'
  run $type-dtime $type 'D $I "sif /a/s.txt dtime 12345"'
done
run ext2-badptr ext2 'D $I "sif /a/mid.bin block[2] 99999999"'
run ext2-dup ext2 'D $I "sif /x/o.txt block[0] $(blk $I /a/mid.bin 1)"'
run ext3-badind ext3 'D $I "sif /a/b/c/blob block[IND] 99999999"'
run ext4-badextent ext4 'D $I "sif /a/b/c/blob block[5] 0xdead"'
run ext4-badroot ext4 'D $I "sif /a/mid.bin block[0] 0x1234"'
run ext4-icsum ext4 'D $I "sif /a/s.txt checksum 0x1234"'
run ext4-dup ext4 'D $I "sif /x/o.txt block[5] $(blk $I /a/mid.bin 1)"'
echo "$n cases"; exit $fail
