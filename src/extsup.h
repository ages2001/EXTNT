/*
 * EXTNT - the shared, kernel-free ext2/ext3/ext4 engine.
 *
 * Everything that understands the on-disk format lives in the ext*.c engine
 * files: superblock and group descriptors, block and inode bitmaps, inodes,
 * block mapping (indirect blocks and extent trees), directories (linear and
 * htree), symlinks, extended attributes, and the jbd2 journal (recovery and
 * writing). The same files build into the driver, the format/check/image
 * tools and the Linux test harness.
 *
 * Disk access goes through a small host interface (ExtHost*, below) that maps
 * runs of 512-byte sectors into memory: in the driver that is the metadata
 * cache, in user mode a simple sector cache. The engine serializes: one
 * thread inside it per volume for anything that changes metadata.
 */

#ifndef _EXTSUP_H_
#define _EXTSUP_H_

#include "extdisk.h"
#include "extjbd.h"

#ifndef EXT_STATUS_DEFINED
#define EXT_STATUS_DEFINED
typedef LONG EXT_STATUS;
#endif

#ifndef NT_SUCCESS
#define NT_SUCCESS(s) ((EXT_STATUS)(s) >= 0)
#endif

/* Status values (NTSTATUS codes) */
#define EXT_OK                  ((EXT_STATUS)0x00000000L)
#define EXT_NO_MORE_ENTRIES     ((EXT_STATUS)0x8000001AL)  /* STATUS_NO_MORE_ENTRIES */
#define EXT_NOT_FOUND           ((EXT_STATUS)0xC0000034L)  /* OBJECT_NAME_NOT_FOUND */
#define EXT_EXISTS              ((EXT_STATUS)0xC0000035L)  /* OBJECT_NAME_COLLISION */
#define EXT_DISK_FULL           ((EXT_STATUS)0xC000007FL)
#define EXT_NO_MEMORY           ((EXT_STATUS)0xC000009AL)
#define EXT_CORRUPT             ((EXT_STATUS)0xC0000032L)  /* DISK_CORRUPT_ERROR */
#define EXT_FILE_CORRUPT        ((EXT_STATUS)0xC0000102L)
#define EXT_BAD_NAME            ((EXT_STATUS)0xC0000033L)
#define EXT_NOT_EMPTY           ((EXT_STATUS)0xC0000101L)
#define EXT_IO_ERROR            ((EXT_STATUS)0xC0000185L)
#define EXT_READ_ONLY           ((EXT_STATUS)0xC00000A2L)
#define EXT_UNRECOGNIZED        ((EXT_STATUS)0xC000014FL)
#define EXT_TOO_LARGE           ((EXT_STATUS)0xC0000904L)
#define EXT_NOT_SUPPORTED       ((EXT_STATUS)0xC00000BBL)
#define EXT_NOT_A_DIRECTORY     ((EXT_STATUS)0xC0000103L)
#define EXT_IS_A_DIRECTORY      ((EXT_STATUS)0xC00000BAL)
#define EXT_NAME_TOO_LONG       ((EXT_STATUS)0xC0000106L)
#define EXT_NO_SUCH_EA          ((EXT_STATUS)0xC0000052L)  /* NO_EAS_ON_FILE */
#define EXT_EA_CORRUPT          ((EXT_STATUS)0x80000013L)
#define EXT_BUFFER_TOO_SMALL    ((EXT_STATUS)0xC0000023L)  /* BUFFER_TOO_SMALL */
#define EXT_ACCESS_DENIED       ((EXT_STATUS)0xC0000022L)
#define EXT_TOO_MANY_LINKS      ((EXT_STATUS)0xC0000265L)
#define EXT_NO_SPACE            ((EXT_STATUS)0xC0000907L)  /* EA/xattr value won't fit */

/* Largest file EXTNT handles (ext4 huge_file is larger, but NT file sizes
   and our mapping cap here) */
#define EXT_MAX_NAME            255

/* A mapped run of sectors */
typedef struct _EXT_BUF {
    PUCHAR      Data;
    ULONGLONG   Sector;
    ULONG       Count;
    PVOID       Host;
    BOOLEAN     Dirty;
} EXT_BUF, *PEXT_BUF;

#define EXT_MAP_READ            0x00
#define EXT_MAP_WRITE           0x01
#define EXT_MAP_NEW             0x03    /* write, old contents do not matter (zeroed) */
#define EXT_MAP_DATA            0x10    /* or'd in: file data, not metadata (never journaled) */
#define EXT_MAP_MODE(m)         ((m) & 0x0F)

#define EXT_SECTOR_SIZE         512
#define EXT_SECTOR_SHIFT        9

/* Per-volume engine state */
typedef struct _EXT_VOL {
    PVOID       HostContext;            /* the host's volume (VCB, image) */

    /* Geometry */
    ULONG       BlockSize;              /* bytes */
    ULONG       BlockShift;             /* log2(BlockSize) */
    ULONG       SectorsPerBlock;        /* BlockSize / 512 */
    ULONGLONG   BlocksCount;
    ULONGLONG   FreeBlocksCount;
    ULONG       InodesCount;
    ULONG       FreeInodesCount;
    ULONG       FirstDataBlock;
    ULONG       BlocksPerGroup;
    ULONG       InodesPerGroup;
    ULONG       GroupCount;
    USHORT      InodeSize;
    USHORT      DescSize;               /* group descriptor size */
    ULONG       FirstIno;
    ULONG       ItbPerGroup;            /* inode-table blocks per group */
    ULONG       ClusterBits;            /* bigalloc: log2(blocks per cluster), else 0 */
    ULONG       ClustersPerGroup;       /* bits in a block bitmap (= BlocksPerGroup without bigalloc) */
    ULONG       SectorsPerCluster;      /* what one allocation adds to i_blocks */

    ULONG       FeatureCompat;
    ULONG       FeatureIncompat;
    ULONG       FeatureRoCompat;
    BOOLEAN     Has64Bit;
    BOOLEAN     HasExtents;
    BOOLEAN     HasFiletype;
    BOOLEAN     HasMetaCsum;            /* crc32c everywhere */
    BOOLEAN     HasGdtCsum;             /* crc16 group descriptors */
    BOOLEAN     HasJournal;
    BOOLEAN     HasHtree;
    BOOLEAN     HasInlineData;
    BOOLEAN     HasFlexBg;
    BOOLEAN     HasMetaBg;
    ULONG       LogGroupsPerFlex;

    ULONG       ChecksumSeed;           /* crc32c(~0, uuid) or sb->ChecksumSeed */
    UCHAR       Uuid[16];
    ULONG       HashSeed[4];
    UCHAR       DefHashVersion;

    char        VolumeName[17];
    ULONG       SerialNumber;           /* derived from the uuid */

    ULONG       JournalInum;
    ULONG       MkfsTime;

    BOOLEAN     ReadOnly;
    BOOLEAN     ReadOnlyForced;         /* unsupported ro-compat feature */
    BOOLEAN     Dirty;                  /* state has errors / mounted not clean */
    BOOLEAN     NeedsRecovery;
    BOOLEAN     OldExt;                 /* the original 1992 ext (read-only, see extold.c) */

    /* The superblock, kept in memory and written back on change */
    PEXT_SUPERBLOCK Super;              /* allocated, BlockSize or 1024 bytes */
    ULONG       SuperSector;            /* sector of byte offset 1024 */

    /* Group descriptors, kept in memory */
    PUCHAR      GroupDescs;             /* GroupCount * DescSize */
    PULONG      GroupDirty;             /* bitmap: which group descs changed */

    /* Allocation hints */
    ULONG       LastAllocGroup;

    /* Journal (see extjbd.c); NULL when no journal or not writing */
    PVOID       Journal;
    USHORT      MountState;             /* s_state when writing started, restored at the end */
    BOOLEAN     CrashAfterCommit;       /* test hook: stop right after the commit point */
    BOOLEAN     BackupsStale;           /* superblock backups need rewriting (feature/label change) */

    /* Monotonic block counts (i_blocks accounting is their difference) */
    ULONGLONG   AllocTotal, FreeTotal;
    /* Blocks freed in the open transaction: they stay marked in use until it
       commits, so nothing reuses them before the change is durable */
    PULONGLONG  PendingFree;
    ULONG       PendingCount, PendingCap;
} EXT_VOL, *PEXT_VOL;

/* ------------------------------------------------------------------ */
/* Host interface (driver, tools, test)                                */
/* ------------------------------------------------------------------ */

EXT_STATUS  ExtHostMap(PEXT_VOL Vol, ULONGLONG Sector, ULONG Count, ULONG Mode, PEXT_BUF Buf);
VOID        ExtHostSetDirty(PEXT_VOL Vol, PEXT_BUF Buf);
VOID        ExtHostUnmap(PEXT_VOL Vol, PEXT_BUF Buf);
PVOID       ExtHostAlloc(ULONG Bytes);
VOID        ExtHostFree(PVOID P);
ULONG       ExtHostNow(VOID);
VOID        ExtHostFreed(PEXT_VOL Vol, ULONGLONG Sector, ULONG Count);

/* For the journal (all synchronous, bypassing any cache):
   EnumDirty reports every dirty cached run; Flush writes them all home and
   waits; ReadDirect/WriteDirect move sectors the cache never holds. */
typedef VOID (*EXT_DIRTY_CB)(PVOID Ctx, ULONGLONG Sector, ULONG Count, const UCHAR *Data);
VOID        ExtHostEnumDirty(PEXT_VOL Vol, EXT_DIRTY_CB Cb, PVOID Ctx);
EXT_STATUS  ExtHostFlush(PEXT_VOL Vol);
EXT_STATUS  ExtHostReadDirect(PEXT_VOL Vol, ULONGLONG Sector, ULONG Count, PVOID Buf);
EXT_STATUS  ExtHostWriteDirect(PEXT_VOL Vol, ULONGLONG Sector, ULONG Count, const VOID *Buf);

#define ExtSetDirty(Vol, Buf)  ExtHostSetDirty((Vol), (Buf))

/* ------------------------------------------------------------------ */
/* Basics                                                              */
/* ------------------------------------------------------------------ */

VOID    ExtZero(PVOID P, ULONG N);
BOOLEAN ExtMemEq(const VOID *A, const VOID *B, ULONG N);
VOID    ExtCopy(PVOID D, const VOID *S, ULONG N);
ULONG   ExtCrc32c(ULONG Crc, const VOID *Data, ULONG Length);
USHORT  ExtCrc16(USHORT Crc, const VOID *Data, ULONG Length);

#define EXT_UNKNOWN     0xFFFFFFFFUL
#define MIN_UL(a, b)    ((a) < (b) ? (a) : (b))

/* Block <-> sector */
#define ExtBlockSector(Vol, Block) ((ULONGLONG)(Block) * (Vol)->SectorsPerBlock)

/* Map a run of whole blocks */
EXT_STATUS  ExtMapBlocks(PEXT_VOL Vol, ULONGLONG Block, ULONG Count, ULONG Mode, PEXT_BUF Buf);

/* ------------------------------------------------------------------ */
/* Volume                                                              */
/* ------------------------------------------------------------------ */

BOOLEAN     ExtIsSuperblock(const VOID *Block1024);
EXT_STATUS  ExtMount(PEXT_VOL Vol, BOOLEAN ReadOnly);
VOID        ExtUnmountFree(PEXT_VOL Vol);
EXT_STATUS  ExtFlushSuper(PEXT_VOL Vol);        /* write superblock + dirty group descs */
EXT_STATUS  ExtSetState(PEXT_VOL Vol, BOOLEAN Clean);
EXT_STATUS  ExtSetLabel(PEXT_VOL Vol, const char *Label);

/* Group descriptor field access (handles 32/64-bit and checksums) */
PEXT_GROUP_DESC ExtGroupDesc(PEXT_VOL Vol, ULONG Group);
ULONGLONG   ExtGdBlockBitmap(PEXT_VOL Vol, ULONG Group);
ULONGLONG   ExtGdInodeBitmap(PEXT_VOL Vol, ULONG Group);
ULONGLONG   ExtGdInodeTable(PEXT_VOL Vol, ULONG Group);
ULONG       ExtGdFreeBlocks(PEXT_VOL Vol, ULONG Group);
ULONG       ExtGdFreeInodes(PEXT_VOL Vol, ULONG Group);
VOID        ExtGdSetFreeBlocks(PEXT_VOL Vol, ULONG Group, ULONG Value);
VOID        ExtGdSetFreeInodes(PEXT_VOL Vol, ULONG Group, ULONG Value);
VOID        ExtGdAddUsedDirs(PEXT_VOL Vol, ULONG Group, LONG Delta);
VOID        ExtGroupTouched(PEXT_VOL Vol, ULONG Group);  /* recompute csum, mark dirty */

/* ------------------------------------------------------------------ */
/* Allocation                                                          */
/* ------------------------------------------------------------------ */

EXT_STATUS  ExtAllocBlock(PEXT_VOL Vol, ULONGLONG Near, PULONGLONG Block);
EXT_STATUS  ExtFreeBlock(PEXT_VOL Vol, ULONGLONG Block);
EXT_STATUS  ExtApplyPendingFrees(PEXT_VOL Vol);
EXT_STATUS  ExtAllocInode(PEXT_VOL Vol, BOOLEAN Directory, ULONG NearInode, PULONG Inode);
EXT_STATUS  ExtFreeInode(PEXT_VOL Vol, ULONG Inode, BOOLEAN Directory);
EXT_STATUS  ExtCountFree(PEXT_VOL Vol, PULONGLONG FreeBlocks, PULONG FreeInodes);

/* ------------------------------------------------------------------ */
/* Inodes                                                              */
/* ------------------------------------------------------------------ */

EXT_STATUS  ExtReadInode(PEXT_VOL Vol, ULONG Ino, PEXT_INODE Inode);
EXT_STATUS  ExtWriteInode(PEXT_VOL Vol, ULONG Ino, PEXT_INODE Inode);
/* Location of inode Ino: its sector and byte offset within that sector run */
EXT_STATUS  ExtInodeLocation(PEXT_VOL Vol, ULONG Ino, PULONGLONG Sector, PULONG Offset);
ULONGLONG   ExtInodeSize(PEXT_VOL Vol, PEXT_INODE Inode);
VOID        ExtInodeSetSize(PEXT_VOL Vol, PEXT_INODE Inode, ULONGLONG Size);
ULONGLONG   ExtInodeBlocks(PEXT_VOL Vol, PEXT_INODE Inode);
VOID        ExtInodeSetBlocks(PEXT_VOL Vol, PEXT_INODE Inode, ULONGLONG Sectors512);
VOID        ExtInodeCsum(PEXT_VOL Vol, ULONG Ino, PEXT_INODE Inode);
VOID        ExtInodeCsumImage(PEXT_VOL Vol, ULONG Ino, PUCHAR Image);
ULONG       ExtInodeCoreSize(PEXT_VOL Vol, PEXT_INODE Inode);
EXT_STATUS  ExtZeroInodeImage(PEXT_VOL Vol, ULONG Ino);
/* releases a deleted inode's xattr block and ea_inode references */
EXT_STATUS  ExtXattrRelease(PEXT_VOL Vol, ULONG Ino, PEXT_INODE Inode);

/* ------------------------------------------------------------------ */
/* Block mapping of a file inode                                       */
/* ------------------------------------------------------------------ */

/* Map file block FileBlk of inode (Ino, Inode) to a disk block.
   *Disk = 0 means a hole. *RunLen = contiguous blocks mapped from FileBlk. */
EXT_STATUS  ExtMapFileBlock(PEXT_VOL Vol, ULONG Ino, PEXT_INODE Inode, ULONGLONG FileBlk,
                            PULONGLONG Disk, PULONG RunLen);

/* Make file block FileBlk present (allocate if a hole); returns the disk block */
EXT_STATUS  ExtGetFileBlock(PEXT_VOL Vol, ULONG Ino, PEXT_INODE Inode, ULONGLONG FileBlk,
                            PULONGLONG Disk, PBOOLEAN Fresh);   /* Fresh: newly allocated, contents undefined */

/* extent trees (extext.c) */
EXT_STATUS  ExtExtentMap(PEXT_VOL Vol, PEXT_INODE Inode, ULONGLONG FileBlk, PULONGLONG Disk, PULONG RunLen);
EXT_STATUS  ExtExtentGet(PEXT_VOL Vol, ULONG Ino, PEXT_INODE Inode, ULONGLONG FileBlk, PULONGLONG Disk, PBOOLEAN Fresh);
EXT_STATUS  ExtExtentTruncate(PEXT_VOL Vol, ULONG Ino, PEXT_INODE Inode, ULONGLONG NewBlocks);
EXT_STATUS  ExtExtentFreeAll(PEXT_VOL Vol, ULONG Ino, PEXT_INODE Inode);
VOID        ExtExtentBlockCsum(PEXT_VOL Vol, ULONG Ino, PEXT_INODE Inode, PUCHAR Block);

/* Truncate the block map to NewBlocks file blocks, freeing the rest */
EXT_STATUS  ExtTruncateBlocks(PEXT_VOL Vol, ULONG Ino, PEXT_INODE Inode, ULONGLONG NewBlocks);

/* Free every data block, extent/indirect metadata and xattr block of an inode */
EXT_STATUS  ExtFreeInodeData(PEXT_VOL Vol, ULONG Ino, PEXT_INODE Inode);

/* Internal (extmap.c) */
ULONGLONG   ExtGroupBase(PEXT_VOL Vol, ULONG Ino);
EXT_STATUS  ExtFreeIndirectTree(PEXT_VOL Vol, ULONG Block, int Level);

/* ------------------------------------------------------------------ */
/* Directories                                                         */
/* ------------------------------------------------------------------ */

typedef struct _EXT_ENTRY {
    ULONG       Inode;
    UCHAR       FileType;
    USHORT      NameLength;
    char        Name[EXT_MAX_NAME + 1];
    ULONGLONG   Cookie;                 /* resume position for enumeration */
} EXT_ENTRY, *PEXT_ENTRY;

EXT_STATUS  ExtLookup(PEXT_VOL Vol, ULONG DirIno, PEXT_INODE Dir, const char *Name, ULONG NameLen,
                      PEXT_ENTRY Entry);
/* the original 1992 ext (extold.c), read-only */
EXT_STATUS  ExtOldMount(PEXT_VOL Vol, const UCHAR *Sb1024);
EXT_STATUS  ExtOldReadInode(PEXT_VOL Vol, ULONG Ino, PEXT_INODE Inode);
EXT_STATUS  ExtOldMapFileBlock(PEXT_VOL Vol, PEXT_INODE Inode, ULONGLONG FileBlk, PULONGLONG Disk, PULONG RunLen);
EXT_STATUS  ExtOldEnum(PEXT_VOL Vol, ULONG DirIno, PEXT_INODE Dir, ULONGLONG Cookie, PEXT_ENTRY Entry);
EXT_STATUS  ExtOldLookup(PEXT_VOL Vol, ULONG DirIno, PEXT_INODE Dir, const char *Name, ULONG NameLen, PEXT_ENTRY Entry);
EXT_STATUS  ExtOldWriteInode(PEXT_VOL Vol, ULONG Ino, PEXT_INODE Inode);
EXT_STATUS  ExtOldCreate(PEXT_VOL Vol, ULONG ParentIno, const char *Name, ULONG NameLen, USHORT Mode, PULONG NewIno);
EXT_STATUS  ExtOldWriteData(PEXT_VOL Vol, ULONG Ino, ULONGLONG Off, const UCHAR *Data, ULONG Len);
EXT_STATUS  ExtOldUnlink(PEXT_VOL Vol, ULONG ParentIno, const char *Name, ULONG NameLen);
EXT_STATUS  ExtEnum(PEXT_VOL Vol, ULONG DirIno, PEXT_INODE Dir, ULONGLONG Cookie, PEXT_ENTRY Entry);
EXT_STATUS  ExtDirIsEmpty(PEXT_VOL Vol, ULONG DirIno, PEXT_INODE Dir, PBOOLEAN Empty);
EXT_STATUS  ExtAddEntry(PEXT_VOL Vol, ULONG DirIno, PEXT_INODE Dir, const char *Name, ULONG NameLen,
                        ULONG Inode, UCHAR FileType);
EXT_STATUS  ExtRemoveEntry(PEXT_VOL Vol, ULONG DirIno, PEXT_INODE Dir, const char *Name, ULONG NameLen);

/* Inline data: i_block + the system.data attribute, assembled (free with ExtHostFree) */
EXT_STATUS  ExtInlineLoad(PEXT_VOL Vol, ULONG Ino, PEXT_INODE Inode, PUCHAR *Data, PULONG Len);
EXT_STATUS  ExtInlineSetParent(PEXT_VOL Vol, ULONG DirIno, PEXT_INODE Dir, ULONG Parent);
/* Points ".." of a directory at NewParent (after a move) */
EXT_STATUS  ExtSetDotDot(PEXT_VOL Vol, ULONG DirIno, PEXT_INODE Dir, ULONG NewParent);
EXT_STATUS  ExtGetParent(PEXT_VOL Vol, ULONG DirIno, PEXT_INODE Dir, PULONG Parent);
/* Turns an inline file or directory into an ordinary block-mapped one */
EXT_STATUS  ExtUninline(PEXT_VOL Vol, ULONG Ino, PEXT_INODE Inode);
EXT_STATUS  ExtUninlineDir(PEXT_VOL Vol, ULONG DirIno, PEXT_INODE Dir);
/* A symlink whose target is kept in i_block (Linux's rule) */
BOOLEAN     ExtIsFastSymlink(PEXT_VOL Vol, PEXT_INODE Inode);

/* Stamps the metadata_csum tail of a directory block (no-op without the feature) */
VOID        ExtDirBlockCsum(PEXT_VOL Vol, ULONG DirIno, PEXT_INODE Dir, PUCHAR Block);

/* ------------------------------------------------------------------ */
/* Files, directories, links                                           */
/* ------------------------------------------------------------------ */

/* Creates an inode (file/dir/symlink) named Name in the parent; returns its
   number and the new inode. Mode holds the type and permission bits. For a
   symlink, Target is the link text. */
EXT_STATUS  ExtCreate(PEXT_VOL Vol, ULONG DirIno, PEXT_INODE Dir, const char *Name, ULONG NameLen,
                      USHORT Mode, const char *Target, PULONG NewIno, PEXT_INODE NewInode);

/* Removes a directory entry and, when the link count reaches zero, the inode */
EXT_STATUS  ExtUnlink(PEXT_VOL Vol, ULONG DirIno, PEXT_INODE Dir, const char *Name, ULONG NameLen);

/* Adds a hard link to an existing inode */
EXT_STATUS  ExtLink(PEXT_VOL Vol, ULONG DirIno, PEXT_INODE Dir, const char *Name, ULONG NameLen,
                    ULONG TargetIno, PEXT_INODE Target);

EXT_STATUS  ExtRename(PEXT_VOL Vol, ULONG OldDir, PEXT_INODE OldDirInode, const char *OldName, ULONG OldLen,
                      ULONG NewDir, PEXT_INODE NewDirInode, const char *NewName, ULONG NewLen);

/* Reads the target of a symlink into Buf (NUL-terminated); *Len excludes the NUL */
EXT_STATUS  ExtReadLink(PEXT_VOL Vol, ULONG Ino, PEXT_INODE Inode, char *Buf, ULONG BufSize, PULONG Len);

/* ------------------------------------------------------------------ */
/* File data (blocks of a regular file)                                */
/* ------------------------------------------------------------------ */

/* Reads Length bytes at Offset of the file; a hole reads as zeros */
EXT_STATUS  ExtReadFile(PEXT_VOL Vol, ULONG Ino, PEXT_INODE Inode, ULONGLONG Offset, ULONG Length,
                        PVOID Buffer, PULONG Read);

/* Writes Length bytes at Offset, growing the file; updates size in Inode */
EXT_STATUS  ExtWriteFile(PEXT_VOL Vol, ULONG Ino, PEXT_INODE Inode, ULONGLONG Offset, ULONG Length,
                         const VOID *Buffer, PULONG Written);

EXT_STATUS  ExtSetFileSize(PEXT_VOL Vol, ULONG Ino, PEXT_INODE Inode, ULONGLONG NewSize);

/* The checker's helpers */
BOOLEAN     ExtGroupHasSuper(PEXT_VOL Vol, ULONG Group);
ULONGLONG   ExtGroupFirstBlock(PEXT_VOL Vol, ULONG Group);
ULONG       ExtGroupClusters(PEXT_VOL Vol, ULONG Group);
ULONG       ExtGroupBaseMeta(PEXT_VOL Vol, ULONG Group);    /* superblock + descriptor blocks at its start */
ULONG       ExtGdtBlockCount(PEXT_VOL Vol);
EXT_STATUS  ExtExtentRebuild(PEXT_VOL Vol, ULONG Ino, PEXT_INODE Inode, ULONG Count,
                             const ULONG *FileBlock, const ULONG *RawLen, const ULONGLONG *Start);
VOID        ExtXattrBlockCsum(PEXT_VOL Vol, ULONGLONG BlockNr, PUCHAR Block);
EXT_STATUS  ExtExtentGetList(PEXT_VOL Vol, PEXT_INODE Inode, PULONG Count, PULONG *Block,
                             PULONG *RawLen, PULONGLONG *Start);
EXT_STATUS  ExtExtentRemap(PEXT_VOL Vol, ULONG Ino, PEXT_INODE Inode, ULONG Count,
                           const ULONG *Block, const ULONG *RawLen, const ULONGLONG *Start);
EXT_STATUS  ExtMapBlockBitmap(PEXT_VOL Vol, ULONG Group, PEXT_BUF Buf, PBOOLEAN WasUninit);
EXT_STATUS  ExtBitmapCsum(PEXT_VOL Vol, ULONG Group, PUCHAR Bitmap, ULONG Bytes, BOOLEAN Inode);

/* The formatter's helpers: a reserved inode, the root directory, one more
   directory block */
EXT_STATUS  ExtNewInodeAt(PEXT_VOL Vol, ULONG Ino, USHORT Mode, PEXT_INODE Inode);
EXT_STATUS  ExtMakeRoot(PEXT_VOL Vol);
EXT_STATUS  ExtGrowDirectory(PEXT_VOL Vol, ULONG Ino, PEXT_INODE Inode);

/* Deletes/truncates the inodes a crashed Linux left on the orphan list */
EXT_STATUS  ExtProcessOrphans(PEXT_VOL Vol);

/* ------------------------------------------------------------------ */
/* Extended attributes (name -> value, in the ext namespaces)          */
/* ------------------------------------------------------------------ */

typedef struct _EXT_XATTR {
    UCHAR       NameIndex;
    char        Name[256];
    ULONG       NameLen;
    PUCHAR      Value;
    ULONG       ValueLen;
} EXT_XATTR, *PEXT_XATTR;

/* Lists xattrs: for each, Report is called; returns EXT_OK at the end */
EXT_STATUS  ExtListXattr(PEXT_VOL Vol, ULONG Ino, PEXT_INODE Inode,
                         VOID (*Report)(PVOID Ctx, UCHAR NameIndex, const char *Name, ULONG NameLen,
                                        const UCHAR *Value, ULONG ValueLen), PVOID Ctx);
EXT_STATUS  ExtGetXattr(PEXT_VOL Vol, ULONG Ino, PEXT_INODE Inode, UCHAR NameIndex, const char *Name,
                        ULONG NameLen, PUCHAR Value, ULONG ValueSize, PULONG ValueLen);
EXT_STATUS  ExtSetXattr(PEXT_VOL Vol, ULONG Ino, PEXT_INODE Inode, UCHAR NameIndex, const char *Name,
                        ULONG NameLen, const UCHAR *Value, ULONG ValueLen);

/* ------------------------------------------------------------------ */
/* Journal (extjbd.c)                                                  */
/* ------------------------------------------------------------------ */

/* Replays the journal into the home locations if the volume needs recovery.
   After success the RECOVER flag is cleared. */
EXT_STATUS  ExtJournalRecover(PEXT_VOL Vol);

/* Opens the journal for writing (reads its superblock); no-op without one */
EXT_STATUS  ExtJournalOpen(PEXT_VOL Vol);
VOID        ExtJournalClose(PEXT_VOL Vol);

/* A transaction wraps a set of metadata writes. Begin, then the engine
   dirties blocks through the host; Commit writes them to the journal and
   then to their homes. The host calls ExtJournalLog for each dirty block it
   is about to write at commit (see extmeta/host). */
EXT_STATUS  ExtTxnBegin(PEXT_VOL Vol);
EXT_STATUS  ExtTxnCommit(PEXT_VOL Vol);
VOID        ExtTxnAbort(PEXT_VOL Vol);

/* Read-write session: StartWriting after mount/recovery (journal open,
   RECOVER set, state not clean); StopWriting at clean unmount */
EXT_STATUS  ExtStartWriting(PEXT_VOL Vol);
EXT_STATUS  ExtStopWriting(PEXT_VOL Vol);

/* Records one metadata block (its home block number and current contents) in
   the open transaction. Called by the host for each dirty metadata block. */
EXT_STATUS  ExtJournalLog(PEXT_VOL Vol, ULONGLONG HomeBlock, const VOID *Data);

/* TRUE while a transaction is open (the host journals metadata writes) */
BOOLEAN     ExtJournalInTxn(PEXT_VOL Vol);

/* ------------------------------------------------------------------ */
/* Time (ext stores seconds since 1970 UTC, plus extra nanoseconds)    */
/* ------------------------------------------------------------------ */

VOID        ExtSetTime(ULONG *Field, ULONG *Extra, ULONGLONG Nt100ns);
ULONGLONG   ExtGetTime(ULONG Field, ULONG Extra);

/* ------------------------------------------------------------------ */
/* Hashing (htree)                                                     */
/* ------------------------------------------------------------------ */

/* Version: the directory's dx hash version (legacy/half_md4/tea), already
   moved to its _unsigned variant when the superblock says so */
ULONG       ExtNameHash(PEXT_VOL Vol, UCHAR Version, const char *Name, ULONG Len, PULONG MinorHash);

#endif /* _EXTSUP_H_ */
