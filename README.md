# EXTNT - ext2 / ext3 / ext4 File System Driver for Windows NT

EXTNT is a from-scratch driver for the Linux **ext2, ext3 and ext4** file
systems - and the original 1992 **ext** - on Windows NT 3.1, 3.5, 3.51, 4.0,
2000, XP and XP x64. It reads and writes them, keeps them correct for Linux
(journal, checksums and all), and can **start Windows NT from an ext volume**.

It is built like its siblings [FATNT](https://github.com/ages2001/FATNT),
[NTFS3NT](https://github.com/ages2001/NTFS3NT) and
[HPFSNT](https://github.com/ages2001/HPFSNT): a kernel-free engine that knows
the on-disk format - built into the driver, the tools and a Linux test
harness - and an NT layer on top of it. One source tree gives one `extnt.sys`
per architecture, built with GCC; no Visual C++ or DDK is needed.

## At a glance

| | |
|---|---|
| **Version** | 1.1 |
| **File systems** | ext (1992), ext2 (revisions 0 and 1), ext3, ext4 |
| **Windows** | NT 3.1, 3.5, 3.51, 4.0, 2000, XP (x86); XP x64 / Server 2003 x64 (amd64) |
| **Access** | read/write |
| **Paging file** | yes, the boot volume included |
| **Starting Windows from it** | NT 3.1, 3.51, 4.0, 2000 |
| **Tools** | `extinst`, `extfmt`, `extchk`, `extboot`, `extutil` |

## Supported Windows versions

| System | Binaries | Data volumes | Start from ext |
|---|---|---|---|
| Windows NT 3.1, 3.5, 3.51, 4.0 | `bin\i386` | yes | yes (NT 3.1, 3.51, 4.0 tested) |
| Windows 2000, XP | `bin\i386` | yes | yes (2000 tested) |
| Windows XP x64, Server 2003 x64 | `bin\amd64` | yes | no - its loader does not take EXTNT as a boot driver |

## Features

**The format**

| | |
|---|---|
| ext (1992) | the pre-ext2 ext (magic `0x137D`), read/write with 1 KB blocks: the Minix-style inode table, indirect zones, variable-length directories. `mkext1` makes one, since no historical mkfs survives |
| ext2 | revision 0 (the original layout) and revision 1, block sizes from 1 KB to 64 KB |
| ext3 | the jbd2 journal: replayed at mount, every change committed through it, `journal_csum` v2 / v3 |
| ext4 | extents, `64bit`, `flex_bg`, `meta_bg`, `sparse_super2`, `huge_file`, `dir_nlink`, `extra_isize`, `metadata_csum` (with `csum_seed`), `uninit_bg` / `gdt_csum`, `inline_data`, `large_dir`, `ea_inode`, `orphan_file`, `fast_commit`, `bigalloc` (files take whole clusters, as on Linux) |
| directories | linear and htree (`dir_index`); names case-preserving, lookups case-insensitive or (optionally) case-sensitive |

**Files and volumes**

- Create, read, write, truncate, rename, delete (POSIX semantics for files
  still open), hard links, sparse files, files larger than 4 GB, the orphan
  list.
- Linux symbolic links are followed (or shown as links) and can be created
  with `extutil symlink`.
- NT extended attributes are stored as `user.*` xattrs; the hidden, system and
  archive attributes in `user.DOSATTRIB`; read-only comes from the mode bits.
  Owner and mode can be set with `extutil`.
- The volume label (UTF-8), a serial number from the UUID, free space, the
  dirty flag and Linux's clean / in-use state, lock and dismount.
- Linux partitions (type `83h`) get drive letters on NT 3.1 - 4.0 and with the
  Mount Manager of 2000 and later.

**The driver**

- Cache Manager integration (cached reads and writes, fast I/O, memory-mapped
  files and executables, lazy writing), paging I/O and paging files.
- Change notification (see *Limitations*), byte-range locks, share access,
  delete-on-close.
- One i386 binary for NT 3.1 - XP: the executive-resource API bound to the
  running kernel at load time, NT 3.1's structure sizes allowed for, and
  synchronous I/O that is safe on NT 3.1; every import is checked against
  NT 3.1's kernel at build time.

Volumes with features EXTNT cannot write safely - `quota`, `casefold`,
`encrypt`, `verity`, `project`, `mmp`, or an external journal - are mounted
read-only.

## Installing

`bin\i386` holds the driver and tools for Windows NT 3.1 - XP (32-bit),
`bin\amd64` the same for XP x64 / Server 2003 x64. Copy `extnt.sys` and
`extinst.exe` of the matching architecture to one folder and run, from an
administrator account:

```
extinst              install, started with the system
extinst /BOOT        boot start, for starting Windows from an ext volume
extinst /READONLY    mount ext volumes read-only
extinst /NOLETTERS   no drive letters for Linux partitions
extinst /CASE        case-sensitive names
extinst /SHOWDOT     do not hide ".names"
extinst /UID:n /GID:n   owner of new files
extinst /U           uninstall
```

A restart applies the change. On 64-bit Windows run the amd64 `extinst`; the
32-bit one refuses to run there.

## Settings

Under `HKLM\SYSTEM\CurrentControlSet\Services\Extnt` (all `REG_DWORD`;
`extinst` sets them):

| Value | Default | Meaning |
|---|---|---|
| `EnableWriteSupport` | 1 | 0 mounts every ext volume read-only |
| `HideDotFiles` | 1 | names starting with `.` get the hidden attribute |
| `FollowSymlinks` | 1 | opening a symbolic link opens its target |
| `CaseSensitive` | 0 | exact-case lookups only |
| `DefaultUid` / `DefaultGid` | 0 | owner of new files |
| `FileMode` / `DirMode` | 0644 / 0755 | mode of new files and directories |
| `AssignDriveLetters` | 1 | give Linux partitions drive letters (remembered as `Letter.HarddiskN.PartitionM`) |

## Tools

```
extfmt  X: [/T:2|3|4] [/V:label] [/B:size] [/C:size] [/I:size] [/N:bytes]
           [/J:MB] [/R:%] [/O:features] [/REV0] [/LAZY] [/BOOT] [/Y] [/Q]
extchk  X: [/F] [/V] [/Q]
extboot X: [/S:windir] [/FAT] [/Q]
extutil stat FILE | chmod MODE FILE | chown UID[:GID] FILE
        symlink TARGET LINK | readlink LINK
        ea FILE [/A] [NAME=VALUE | NAME= | /TEST]
        info X: | sync X:
```

Every tool takes a drive (`X:`), a partition of a disk
(`\\.\PhysicalDrive1 /P:2`) or an image file. They run on NT 3.1 and later
without a C runtime DLL, and the same sources build for Linux to work on
images.

- **extfmt** formats ext2, ext3 or ext4 (the default): block size (`/B:`),
  bigalloc clusters (`/C:64K`), inode size and density, journal size,
  reserved blocks, features added or removed with `/O:` (`^name` removes),
  the original revision 0 layout (`/REV0`), lazy inode tables (`/LAZY`) and a
  boot area (`/BOOT`). The partition type becomes `83h`.
- **extchk** checks and repairs (`/F`) in e2fsck's passes: the superblock and
  group descriptors (replaying the journal and the orphan list when
  repairing); every inode with its block map or extent tree, blocks claimed
  twice getting copies; every directory and its htree; connectivity and link
  counts, with lost files going to `lost+found`; the bitmaps, free counts and
  group summaries. Exit codes: 0 clean, 1 fixed, 4 problems left, 8 could not
  check.
- **extboot** makes an ext volume start Windows NT (below).
- **extutil** shows and changes what NT has no interface for: mode, owner,
  symbolic links, EAs, the superblock (`info`), and `sync` to flush a volume.

## Starting Windows NT from an ext volume

NTLDR reads only FAT, NTFS and (on NT 3.x) HPFS, so the files it needs also
get a **FAT16 view** kept inside the ext volume:

- sectors 0 - 1 (the 1 KB ext leaves free before its superblock) hold the
  EXTNT boot loader and a FAT16 BPB;
- the blocks of inode 5, ext's *boot loader inode*, hold the FAT, the root
  directory and the FAT subdirectories (`extfmt /BOOT` reserves them low on
  the disk);
- FAT cluster *n* **is** ext block *Base + n - 2*: a file in the view occupies
  the very blocks of the ext file, so the view always shows current contents.

The view holds NTLDR, NTDETECT.COM, BOOT.INI and the other loader files of the
root, and from the Windows directory the kernel, HAL, KD and BOOTVID, the
SYSTEM hive (and its `.ALT` / `.LOG` / `.SAV`), the NLS tables and fonts,
`INF\BIOSINFO.INF`, `BOOTSTAT.DAT`, `AppPatch\DRVMAIN.SDB` and every file of
`SYSTEM32\DRIVERS`. Boot files lying beyond the first 65518 blocks are moved
into reach. The FAT claims 65524 clusters, which Windows' own FAT driver
refuses to mount, so the view is only ever read by NTLDR and the volume always
goes to EXTNT.

EXTNT brings the view up to date whenever it stops writing the volume
(shutdown, lock, dismount): a boot file that changed, grew or was replaced - a
new driver, a registry that grew - shows its current blocks at the next start.

1. `extfmt X: /T:4 /BOOT` - format with a boot area (1 KB - 4 KB blocks give
   a window of 64 MB - 256 MB at the start of the volume).
2. Copy NTLDR, NTDETECT.COM, BOOT.INI and the Windows tree onto it.
3. Install EXTNT into that Windows as a boot driver: `extinst /BOOT` on it
   before copying, or the `Extnt` service with Start 0, ImagePath
   `System32\drivers\extnt.sys`, in its own group "EXTNT" appended to
   `ServiceGroupOrder` (so it loads after the FAT driver and claims the
   volume first). NT 3.1's registry cannot be edited offline: run
   `extinst /BOOT` on the running NT 3.1.
4. `extboot X:` - writes the boot loader and the view; on disk images it also
   marks the partition active.

The partition keeps its Linux type `83h`, and EXTNT gives it its drive letter
(usually C:) at mount, before the session manager runs (`extboot /FAT` sets
type `06h` instead). Linux mounts it as usual - pass `-t ext4` (or ext2 /
ext3), since `blkid` also sees the FAT signature. Run `extboot` again after
NTLDR, the kernel, a HAL or a boot driver was changed by a system without
EXTNT.

Tested: Windows NT 3.1, 3.51, 4.0 and 2000 start from ext2, ext3 and ext4
volumes, are used, shut down and start again with `e2fsck` clean.

## Building

On Linux with `gcc-mingw-w64-i686` / `gcc-mingw-w64-x86-64` (which ship the
DDK headers), `binutils` and `python3`:

```
sh build/build_cross.sh  OUTDIR     # i386: extnt.sys and the tools
sh build/build_amd64.sh  OUTDIR     # amd64: the same for XP x64
bash test/buildlinux.sh             # Linux: exttool (engine test tool) and the tools
```

`NT31_EXPORTS` / `NT31_USER_EXPORTS` point the i386 build at export lists of
NT 3.1's kernel and user DLLs, to check every import;
`EXTRACFLAGS=-DEXN_DBGPORT` builds a driver that traces to the QEMU debug
console (port `E9h`). Ready binaries are in `bin\i386` and `bin\amd64`.

## Testing

- `test/matrix.sh` - 30 mke2fs feature combinations: read, write, stress,
  crash after a commit and replay; `e2fsck -fn` clean after each step.
- `test/modtest.sh`, `test/writetest.sh` - changes to Linux-made images (htree
  and inline directories, hard links, xattrs) compared with the same changes
  made by Linux.
- `test/chktest.sh` - 64 kinds of damage repaired by `extchk /F`, then checked
  by `e2fsck`.
- `test/boottest.sh` - `extfmt /BOOT`, a Windows-like tree, `extboot`; the FAT
  view (read with mtools) must equal the ext files.
- Under QEMU: NT 3.1, 3.51, 4.0, 2000 and XP x64 with ext data volumes; NT 3.1,
  3.51, 4.0 and 2000 started from ext.

## Limitations

- `quota`, `casefold`, `encrypt`, `verity`, `project`, `mmp` and external
  journals: read-only.
- POSIX permissions are stored and shown, not enforced; NT applies its own
  security.
- The universal i386 binary holds change notification pending and completes
  it at cleanup (NT 3.1 has no `FsRtlNotifyFull` package), so File Manager and
  Explorer refresh on demand but never spin; the amd64 build delivers live
  notification.
- Opportunistic locks are not granted.
- Windows XP x64 cannot start from an ext volume (data volumes work).

## License

GNU General Public License v3.0, see [LICENSE](LICENSE).
