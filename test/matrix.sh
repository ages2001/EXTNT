#!/bin/bash
# EXTNT: build images with many mke2fs feature combinations (populated by
# mke2fs -d), then through exttool: compare the tree, modify it, stress it,
# crash after a commit and replay; e2fsck -fn must be clean after each step.
T=${T:-/tmp/claude-0/extbuild/exttool}; W=${W:-/tmp/claude-0/extbuild/matrix}
mkdir -p $W; cd $W || exit 1
if [ ! -d mtree ]; then
  mkdir -p mtree/a/b/c mtree/many
  for i in $(seq 1 400); do echo "n$i" > mtree/many/file_$i; done
  head -c 2000000 /dev/urandom > mtree/a/b/c/blob; echo small > mtree/a/s.txt
  ln -s a/s.txt mtree/fl; ln -s "$(printf 'p%.0s' {1..90})" mtree/sl; ln mtree/a/s.txt mtree/hl
fi
echo x > one.txt; head -c 150000 /dev/urandom > two.bin
fail=0
chk() { e2fsck -fn $1 > e.txt 2>&1 || { echo "   [$2] e2fsck:"; sed -n '2,12p' e.txt; fail=1; return 1; }; }
while IFS='|' read -r name opts size; do
  [ -z "$name" ] && continue
  I=$name.img; rm -f $I
  if ! mke2fs -q -F $opts -d mtree $I ${size:-64M} >mk.txt 2>&1; then echo "== $name: mke2fs failed: $(tail -1 mk.txt)"; continue; fi
  e2fsck -fyD $I >/dev/null 2>&1
  case $name in *jcsum*) printf 'jo -c\njc\n' > jc.cmd; debugfs -w -f jc.cmd $I >/dev/null 2>&1;; esac
  ro=$("$T" $I info 2>/dev/null | head -1); res=""
  c=$("$T" $I cmp mtree 2>&1 | tail -1); [ "$c" = "cmp: 0 difference(s)" ] || { res="$res read:$c"; fail=1; }
  if "$T" $I mkdir /probe >/dev/null 2>&1; then
    "$T" $I put two.bin /many/new.bin >/dev/null && "$T" $I mv /a/b /many/b >/dev/null && "$T" $I rm /many/file_7 >/dev/null \
      && { "$T" $I setea /a/s.txt user k v >/dev/null 2>&1 || [ "$name" = ext2-rev0 ]; } && "$T" $I symlink "$(printf 'q%.0s' {1..80})" /many/q >/dev/null \
      && "$T" $I truncate /many/b/c/blob 777777 >/dev/null || { res="$res modify-failed"; fail=1; }
    chk $I modify || res="$res modify-dirty"
    "$T" $I stress 5 200 >/dev/null || res="$res stress-failed"
    chk $I stress || res="$res stress-dirty"
    "$T" $I rmr /many >/dev/null || res="$res rmr-failed"
    chk $I rmr || res="$res rmr-dirty"
    "$T" -c $I put one.txt /crash.txt >/dev/null; "$T" $I recover >/dev/null 2>&1
    chk $I crash || res="$res crash-dirty"
    "$T" $I cat /crash.txt 2>/dev/null | grep -q x || { res="$res crash-file-missing"; fail=1; }
    echo "== $name: ${res:-OK (read+write+stress+crash)}"
  else
    echo "== $name: mounted read-only (as intended?) ${res:-read OK}"
  fi
done <<'LIST'
ext2-rev0|-t ext2 -r 0 -b 1024|32M
ext2-1k|-t ext2 -b 1024|
ext2-metabg|-t ext2 -O ^resize_inode,meta_bg -b 1024|
ext2-metabg-big|-t ext2 -O ^resize_inode,meta_bg -b 1024 -g 1024|200M
ext4-metabg-big|-t ext4 -O ^resize_inode,meta_bg -b 1024 -g 256|200M
ext2-4k-noidx|-t ext2 -O ^dir_index|
ext3-1k|-t ext3 -b 1024|
ext3-2k-512i|-t ext3 -b 2048 -I 512|
ext4-default|-t ext4|
ext4-jcsum|-t ext4|
ext3-jcsum|-t ext3|
ext4-1k|-t ext4 -b 1024|
ext4-metabg|-t ext4 -O meta_bg,^resize_inode|
ext4-nocsum-gdt|-t ext4 -O ^metadata_csum,uninit_bg,^64bit|
ext4-noflex|-t ext4 -O ^flex_bg|
ext4-noextent|-t ext4 -O ^extent,^64bit|
ext4-inline|-t ext4 -O inline_data|
ext4-1kinode|-t ext4 -I 1024|
ext4-sparse2|-t ext4 -O sparse_super2|
ext4-csumseed|-t ext4 -O metadata_csum_seed|
ext4-largedir|-t ext4 -O large_dir|
ext4-fastcommit|-t ext4 -O fast_commit|
ext4-orphanfile|-t ext4 -O orphan_file|
ext4-eainode|-t ext4 -O ea_inode|
ext4-hugefile-off|-t ext4 -O ^huge_file|
ext4-64k|-t ext4 -b 65536|256M
ext4-quota|-t ext4 -O quota|
ext4-bigalloc|-t ext4 -O bigalloc|
ext4-casefold|-t ext4 -O casefold|
ext4-encrypt|-t ext4 -O encrypt|
LIST
exit $fail
