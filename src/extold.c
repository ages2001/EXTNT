/*
 * EXTNT - the original "ext" file system (Rémy Card, 1992; pre-ext2),
 * read-only.
 *
 * The first Extended File System, before ext2, is Minix-derived: a superblock
 * at byte offset 1024, a flat inode table right after it, free blocks and
 * inodes tracked by linked lists (not bitmaps, so there are no groups), and
 * 64-byte inodes with twelve 32-bit zone pointers - nine direct, then single,
 * double and triple indirect. Directory entries are variable length, like
 * ext2's but with a 16-bit name length and no file-type byte. 1 KB blocks.
 *
 * The format left the Linux kernel in 1997 and no mkfs for it survives, so
 * EXTNT mounts it read-only. Everything here is reached only when ExtMount
 * sets Vol->OldExt (magic 0x137D); the ext2/3/4 paths are untouched.
 */

#ifdef EXT_KERNEL
#include "ext.h"
#else
#include "extusr.h"
#include "extsup.h"
#endif

#define EXT_OLD_MAGIC           0x137D
#define EXT_OLD_INODE_SIZE      64
#define EXT_OLD_ROOT_INO        1
#define EXT_OLD_FIRST_INO       2        /* inode 1 is the root; 2+ are files */
#define EXT_OLD_NDIR            9        /* direct zone pointers */
#define EXT_OLD_IND             9        /* i_zone[9]  single indirect */
#define EXT_OLD_DIND            10       /* i_zone[10] double indirect */
#define EXT_OLD_TIND            11       /* i_zone[11] triple indirect */

/* Raw little-endian accessors (the engine is built on little-endian targets) */
#define OLD_U16(p, o)   ((USHORT)(((PUCHAR)(p))[o] | (((PUCHAR)(p))[(o)+1] << 8)))
#define OLD_U32(p, o)   ((ULONG)(((PUCHAR)(p))[o] | (((PUCHAR)(p))[(o)+1] << 8) | \
                                 (((ULONG)((PUCHAR)(p))[(o)+2]) << 16) | (((ULONG)((PUCHAR)(p))[(o)+3]) << 24)))

/* Fills the engine's volume geometry from an original-ext superblock (1024 bytes).
   Returns EXT_OK and sets Vol->OldExt, or EXT_UNRECOGNIZED. */
EXT_STATUS
ExtOldMount(PEXT_VOL Vol, const UCHAR *Sb)
{
    ULONG Ninodes, Nzones, LogZone, FirstData, ItabBlocks;

    if (OLD_U16(Sb, 56) != EXT_OLD_MAGIC) {
        return EXT_UNRECOGNIZED;
    }
    Ninodes   = OLD_U32(Sb, 0);
    Nzones    = OLD_U32(Sb, 4);
    FirstData = OLD_U32(Sb, 24);
    LogZone   = OLD_U32(Sb, 28);
    if (LogZone > 2 || Ninodes == 0 || Nzones == 0) {
        return EXT_CORRUPT;
    }

    ExtZero(Vol->Uuid, sizeof(Vol->Uuid));
    Vol->Super = (PEXT_SUPERBLOCK)ExtHostAlloc(1024);
    if (Vol->Super == NULL) return EXT_NO_MEMORY;
    ExtZero(Vol->Super, 1024);
    Vol->Super->Magic = EXT_OLD_MAGIC;
    Vol->Super->InodesCount = OLD_U32(Sb, 0);
    Vol->Super->BlocksCountLo = OLD_U32(Sb, 4);
    Vol->OldExt = TRUE;
    /* writable only for 1 KB-block volumes (the write engine assumes 1 KB) */
    Vol->ReadOnly = (LogZone != 0);
    Vol->ReadOnlyForced = (LogZone != 0);
    Vol->BlockSize = 1024u << LogZone;
    Vol->BlockShift = 10 + LogZone;
    Vol->SectorsPerBlock = Vol->BlockSize / 512;
    Vol->BlocksCount = Nzones;
    Vol->FreeBlocksCount = OLD_U32(Sb, 12);
    Vol->InodesCount = Ninodes;
    Vol->FreeInodesCount = OLD_U32(Sb, 20);
    Vol->FirstDataBlock = FirstData;
    Vol->InodeSize = EXT_OLD_INODE_SIZE;
    Vol->FirstIno = EXT_OLD_FIRST_INO;

    /* boot block 0, superblock block 1, then the inode table */
    ItabBlocks = (Ninodes * EXT_OLD_INODE_SIZE + Vol->BlockSize - 1) / Vol->BlockSize;
    Vol->ItbPerGroup = ItabBlocks;
    Vol->InodesPerGroup = Ninodes;          /* one "group": the whole volume */
    Vol->BlocksPerGroup = Nzones;
    Vol->GroupCount = 1;
    Vol->ClustersPerGroup = Nzones;
    Vol->SectorsPerCluster = Vol->SectorsPerBlock;

    /* a stable serial from the geometry (old ext has no UUID) */
    Vol->SerialNumber = (Ninodes * 0x9E3779B1u) ^ (Nzones * 0x85EBCA77u) ^ FirstData;
    Vol->VolumeName[0] = 0;
    return EXT_OK;
}

/* The disk sector and offset of inode Ino (1-based) in the flat table */
static EXT_STATUS
ExtOldInodeLoc(PEXT_VOL Vol, ULONG Ino, PULONGLONG Sector, PULONG Offset)
{
    ULONGLONG ByteOff, Blk;
    /* caller inode N is disk inode N-1, so the disk root (inode 1) is the
       engine's EXT_ROOT_INO (2) and every ext2-based caller just works */
    if (Ino < 2 || Ino > Vol->InodesCount + 1) return EXT_CORRUPT;
    /* the table begins at block 2 */
    ByteOff = (ULONGLONG)(Ino - 2) * EXT_OLD_INODE_SIZE;
    Blk = 2 + ByteOff / Vol->BlockSize;
    *Sector = Blk * Vol->SectorsPerBlock + (ByteOff % Vol->BlockSize) / 512;
    *Offset = (ULONG)(ByteOff % 512);
    return EXT_OK;
}

/* Reads an original-ext inode into the engine's (ext2-shaped) EXT_INODE */
EXT_STATUS
ExtOldReadInode(PEXT_VOL Vol, ULONG Ino, PEXT_INODE Inode)
{
    ULONGLONG Sector;
    ULONG Offset, i;
    EXT_BUF Buf;
    EXT_STATUS S;
    PUCHAR p;

    S = ExtOldInodeLoc(Vol, Ino, &Sector, &Offset);
    if (!NT_SUCCESS(S)) return S;
    S = ExtHostMap(Vol, Sector, (Offset + EXT_OLD_INODE_SIZE + 511) / 512, EXT_MAP_READ, &Buf);
    if (!NT_SUCCESS(S)) return S;
    p = Buf.Data + Offset;

    ExtZero(Inode, sizeof(EXT_INODE));
    Inode->Mode       = OLD_U16(p, 0);
    Inode->UidLo      = OLD_U16(p, 2);
    Inode->SizeLo     = OLD_U32(p, 4);
    Inode->Atime      = OLD_U32(p, 8);
    Inode->Ctime      = OLD_U32(p, 8);
    Inode->Mtime      = OLD_U32(p, 8);
    Inode->GidLo      = OLD_U16(p, 12);
    Inode->LinksCount = OLD_U16(p, 14);
    for (i = 0; i < 12; i++) {
        Inode->Block[i] = OLD_U32(p, 16 + 4 * i);
    }
    /* i_blocks the engine expects in 512-byte units: approximate from size */
    Inode->BlocksLo = (ULONG)(((ULONGLONG)Inode->SizeLo + 511) / 512);
    ExtHostUnmap(Vol, &Buf);
    return EXT_OK;
}

/* One level of indirection: the Index-th pointer under block Blk */
static EXT_STATUS
ExtOldIndirect(PEXT_VOL Vol, ULONG Blk, ULONG Index, PULONG Out)
{
    EXT_BUF Buf;
    EXT_STATUS S;
    if (Blk == 0) { *Out = 0; return EXT_OK; }
    S = ExtHostMap(Vol, (ULONGLONG)Blk * Vol->SectorsPerBlock, Vol->SectorsPerBlock, EXT_MAP_READ, &Buf);
    if (!NT_SUCCESS(S)) return S;
    *Out = OLD_U32(Buf.Data, Index * 4);
    ExtHostUnmap(Vol, &Buf);
    return EXT_OK;
}

/* Maps file block FileBlk of an old-ext inode to a disk block (0 = hole) */
EXT_STATUS
ExtOldMapFileBlock(PEXT_VOL Vol, PEXT_INODE Inode, ULONGLONG FileBlk, PULONGLONG Disk, PULONG RunLen)
{
    ULONG PerBlock = Vol->BlockSize / 4;
    ULONG Blk;
    EXT_STATUS S;

    *RunLen = 1;
    if (FileBlk < EXT_OLD_NDIR) {
        *Disk = Inode->Block[FileBlk];
        return EXT_OK;
    }
    FileBlk -= EXT_OLD_NDIR;
    if (FileBlk < PerBlock) {
        S = ExtOldIndirect(Vol, Inode->Block[EXT_OLD_IND], (ULONG)FileBlk, &Blk);
        if (!NT_SUCCESS(S)) return S;
        *Disk = Blk;
        return EXT_OK;
    }
    FileBlk -= PerBlock;
    if (FileBlk < (ULONGLONG)PerBlock * PerBlock) {
        ULONG Mid;
        S = ExtOldIndirect(Vol, Inode->Block[EXT_OLD_DIND], (ULONG)(FileBlk / PerBlock), &Mid);
        if (!NT_SUCCESS(S)) return S;
        S = ExtOldIndirect(Vol, Mid, (ULONG)(FileBlk % PerBlock), &Blk);
        if (!NT_SUCCESS(S)) return S;
        *Disk = Blk;
        return EXT_OK;
    }
    FileBlk -= (ULONGLONG)PerBlock * PerBlock;
    if (FileBlk < (ULONGLONG)PerBlock * PerBlock * PerBlock) {
        ULONG Hi, Mid;
        S = ExtOldIndirect(Vol, Inode->Block[EXT_OLD_TIND], (ULONG)(FileBlk / ((ULONGLONG)PerBlock * PerBlock)), &Hi);
        if (!NT_SUCCESS(S)) return S;
        S = ExtOldIndirect(Vol, Hi, (ULONG)((FileBlk / PerBlock) % PerBlock), &Mid);
        if (!NT_SUCCESS(S)) return S;
        S = ExtOldIndirect(Vol, Mid, (ULONG)(FileBlk % PerBlock), &Blk);
        if (!NT_SUCCESS(S)) return S;
        *Disk = Blk;
        return EXT_OK;
    }
    *Disk = 0;
    return EXT_OK;
}

/*
 * Enumerates an old-ext directory. Entry layout: inode(u32), rec_len(u16),
 * name_len(u16), name[]. Cookie is the byte offset of the next entry.
 */
EXT_STATUS
ExtOldEnum(PEXT_VOL Vol, ULONG DirIno, PEXT_INODE Dir, ULONGLONG Cookie, PEXT_ENTRY Entry)
{
    ULONGLONG Size = ExtInodeSize(Vol, Dir);
    ULONGLONG Pos = Cookie;
    EXT_BUF Buf;
    EXT_STATUS S;

    (VOID)DirIno;
    while (Pos < Size) {
        ULONGLONG FileBlk = Pos >> Vol->BlockShift;
        ULONG InBlk = (ULONG)(Pos & (Vol->BlockSize - 1));
        ULONGLONG Disk;
        ULONG Run, Ino, NameLen;
        USHORT RecLen;
        PUCHAR e;

        S = ExtOldMapFileBlock(Vol, Dir, FileBlk, &Disk, &Run);
        if (!NT_SUCCESS(S)) return S;
        if (Disk == 0) { Pos = (FileBlk + 1) << Vol->BlockShift; continue; }
        S = ExtHostMap(Vol, Disk * Vol->SectorsPerBlock, Vol->SectorsPerBlock, EXT_MAP_READ, &Buf);
        if (!NT_SUCCESS(S)) return S;

        e = Buf.Data + InBlk;
        Ino = OLD_U32(e, 0);
        RecLen = OLD_U16(e, 4);
        NameLen = OLD_U16(e, 6);
        if (RecLen < 8 || InBlk + RecLen > Vol->BlockSize) {
            ExtHostUnmap(Vol, &Buf);
            /* a damaged record: step to the next block */
            Pos = (FileBlk + 1) << Vol->BlockShift;
            continue;
        }
        if (Ino != 0 && NameLen != 0 && NameLen <= EXT_MAX_NAME && InBlk + 8 + NameLen <= Vol->BlockSize) {
            Entry->Inode = Ino + 1;     /* disk inode -> caller inode (root 1 -> 2) */
            Entry->FileType = 0;        /* old ext has no type byte; caller reads the inode */
            Entry->NameLength = (USHORT)NameLen;
            ExtCopy(Entry->Name, e + 8, NameLen);
            Entry->Name[NameLen] = 0;
            Entry->Cookie = Pos + RecLen;
            ExtHostUnmap(Vol, &Buf);
            return EXT_OK;
        }
        ExtHostUnmap(Vol, &Buf);
        Pos += RecLen;
    }
    return EXT_NO_MORE_ENTRIES;
}

EXT_STATUS
ExtOldLookup(PEXT_VOL Vol, ULONG DirIno, PEXT_INODE Dir, const char *Name, ULONG NameLen, PEXT_ENTRY Entry)
{
    EXT_ENTRY E;
    ULONGLONG Cookie = 0;
    while (NT_SUCCESS(ExtOldEnum(Vol, DirIno, Dir, Cookie, &E))) {
        Cookie = E.Cookie;
        if (E.NameLength == NameLen) {
            ULONG i;
            for (i = 0; i < NameLen; i++) if (E.Name[i] != Name[i]) break;
            if (i == NameLen) { *Entry = E; return EXT_OK; }
        }
    }
    return EXT_NOT_FOUND;
}

/* ================================================================== */
/* original-ext WRITE engine (1 KB blocks)                             */
/*                                                                      */
/* No historical mkfs/fsck survives, so correctness is defined by this  */
/* engine together with mkext1 and the read path above, and validated   */
/* by round-trip (write then read byte-exact) and a structural checker. */
/* ================================================================== */

#define OLD_PUT16(p, o, v) do { ((PUCHAR)(p))[o] = (UCHAR)(v); ((PUCHAR)(p))[(o)+1] = (UCHAR)((v) >> 8); } while (0)
#define OLD_PUT32(p, o, v) do { ((PUCHAR)(p))[o] = (UCHAR)(v); ((PUCHAR)(p))[(o)+1] = (UCHAR)((v) >> 8); \
                                ((PUCHAR)(p))[(o)+2] = (UCHAR)((ULONG)(v) >> 16); ((PUCHAR)(p))[(o)+3] = (UCHAR)((ULONG)(v) >> 24); } while (0)

#define OLD_SB_FIRSTFREEBLK  8
#define OLD_SB_FREEBLKS      12
#define OLD_SB_FREEINODES    20

static int OldNameEq(const UCHAR *a, const char *b, ULONG n)
{ ULONG i; for (i = 0; i < n; i++) if (a[i] != (UCHAR)b[i]) return 0; return 1; }

/* The on-disk superblock lives at byte 1024 = the start of block 1, so field
   offset O is byte O of block 1.  The write path reads and updates its free-list
   head and free counts there directly (Vol->Super stays the minimal read copy). */
static EXT_STATUS
ExtOldSbField(PEXT_VOL Vol, ULONG Off, PULONG Val)
{
    EXT_BUF Buf; EXT_STATUS S;
    S = ExtHostMap(Vol, 1 * Vol->SectorsPerBlock, Vol->SectorsPerBlock, EXT_MAP_READ, &Buf);
    if (!NT_SUCCESS(S)) return S;
    *Val = OLD_U32(Buf.Data, Off);
    ExtHostUnmap(Vol, &Buf);
    return EXT_OK;
}
/* update up to two superblock fields atomically (one mapped write) */
static EXT_STATUS
ExtOldSbSet(PEXT_VOL Vol, ULONG O1, ULONG V1, int Have2, ULONG O2, ULONG V2)
{
    EXT_BUF Buf; EXT_STATUS S;
    S = ExtHostMap(Vol, 1 * Vol->SectorsPerBlock, Vol->SectorsPerBlock, EXT_MAP_WRITE, &Buf);
    if (!NT_SUCCESS(S)) return S;
    OLD_PUT32(Buf.Data, O1, V1);
    if (Have2) OLD_PUT32(Buf.Data, O2, V2);
    ExtHostSetDirty(Vol, &Buf);
    ExtHostUnmap(Vol, &Buf);
    return EXT_OK;
}

/* zero a whole block on disk */
static EXT_STATUS
ExtOldZeroBlock(PEXT_VOL Vol, ULONG Blk)
{
    EXT_BUF Buf; EXT_STATUS S;
    S = ExtHostMap(Vol, (ULONGLONG)Blk * Vol->SectorsPerBlock, Vol->SectorsPerBlock, EXT_MAP_WRITE, &Buf);
    if (!NT_SUCCESS(S)) return S;
    ExtZero(Buf.Data, Vol->BlockSize);
    ExtHostSetDirty(Vol, &Buf);
    ExtHostUnmap(Vol, &Buf);
    return EXT_OK;
}

/* pop a block off the threaded free list; returns 0 in *Blk if full */
static EXT_STATUS
ExtOldAllocBlock(PEXT_VOL Vol, PULONG Blk)
{
    ULONG head, next, freeblks;
    EXT_BUF Buf; EXT_STATUS S;
    *Blk = 0;
    S = ExtOldSbField(Vol, OLD_SB_FIRSTFREEBLK, &head); if (!NT_SUCCESS(S)) return S;
    if (head == 0) return EXT_NO_SPACE;
    S = ExtHostMap(Vol, (ULONGLONG)head * Vol->SectorsPerBlock, Vol->SectorsPerBlock, EXT_MAP_READ, &Buf);
    if (!NT_SUCCESS(S)) return S;
    next = OLD_U32(Buf.Data, 0);
    ExtHostUnmap(Vol, &Buf);
    S = ExtOldSbField(Vol, OLD_SB_FREEBLKS, &freeblks); if (!NT_SUCCESS(S)) return S;
    S = ExtOldSbSet(Vol, OLD_SB_FIRSTFREEBLK, next, 1, OLD_SB_FREEBLKS, freeblks ? freeblks - 1 : 0);
    if (!NT_SUCCESS(S)) return S;
    if (Vol->FreeBlocksCount) Vol->FreeBlocksCount--;
    S = ExtOldZeroBlock(Vol, head);
    if (!NT_SUCCESS(S)) return S;
    *Blk = head;
    return EXT_OK;
}

/* push a block onto the free list */
static EXT_STATUS
ExtOldFreeBlock(PEXT_VOL Vol, ULONG Blk)
{
    ULONG head, freeblks;
    EXT_BUF Buf; EXT_STATUS S;
    if (Blk < Vol->FirstDataBlock || Blk >= (ULONG)Vol->BlocksCount) return EXT_OK;
    S = ExtOldSbField(Vol, OLD_SB_FIRSTFREEBLK, &head); if (!NT_SUCCESS(S)) return S;
    S = ExtHostMap(Vol, (ULONGLONG)Blk * Vol->SectorsPerBlock, Vol->SectorsPerBlock, EXT_MAP_WRITE, &Buf);
    if (!NT_SUCCESS(S)) return S;
    ExtZero(Buf.Data, Vol->BlockSize);
    OLD_PUT32(Buf.Data, 0, head);
    ExtHostSetDirty(Vol, &Buf);
    ExtHostUnmap(Vol, &Buf);
    S = ExtOldSbField(Vol, OLD_SB_FREEBLKS, &freeblks); if (!NT_SUCCESS(S)) return S;
    S = ExtOldSbSet(Vol, OLD_SB_FIRSTFREEBLK, Blk, 1, OLD_SB_FREEBLKS, freeblks + 1);
    if (!NT_SUCCESS(S)) return S;
    Vol->FreeBlocksCount++;
    return EXT_OK;
}

/* serialize an EXT_INODE back to the 64-byte on-disk inode (caller Ino) */
EXT_STATUS
ExtOldWriteInode(PEXT_VOL Vol, ULONG Ino, PEXT_INODE Inode)
{
    ULONGLONG Sector; ULONG Offset, i;
    EXT_BUF Buf; EXT_STATUS S; PUCHAR p;
    S = ExtOldInodeLoc(Vol, Ino, &Sector, &Offset);
    if (!NT_SUCCESS(S)) return S;
    S = ExtHostMap(Vol, Sector, (Offset + EXT_OLD_INODE_SIZE + 511) / 512, EXT_MAP_WRITE, &Buf);
    if (!NT_SUCCESS(S)) return S;
    p = Buf.Data + Offset;
    ExtZero(p, EXT_OLD_INODE_SIZE);
    OLD_PUT16(p, 0, Inode->Mode);
    OLD_PUT16(p, 2, Inode->UidLo);
    OLD_PUT32(p, 4, Inode->SizeLo);
    OLD_PUT32(p, 8, Inode->Mtime);
    OLD_PUT16(p, 12, Inode->GidLo);
    OLD_PUT16(p, 14, Inode->LinksCount);
    for (i = 0; i < 12; i++) OLD_PUT32(p, 16 + 4 * i, Inode->Block[i]);
    ExtHostSetDirty(Vol, &Buf);
    ExtHostUnmap(Vol, &Buf);
    return EXT_OK;
}

/* find a free inode by scanning the table; returns the caller inode number */
static EXT_STATUS
ExtOldAllocInode(PEXT_VOL Vol, PULONG OutIno)
{
    ULONG D;    /* disk inode number 1..Ninodes */
    for (D = EXT_OLD_FIRST_INO; D <= Vol->InodesCount; D++) {
        EXT_INODE In;
        ULONG caller = D + 1;
        if (!NT_SUCCESS(ExtOldReadInode(Vol, caller, &In))) continue;
        if (In.Mode == 0 && In.LinksCount == 0) {
            ULONG fi; ExtOldSbField(Vol, OLD_SB_FREEINODES, &fi);
            ExtOldSbSet(Vol, OLD_SB_FREEINODES, fi ? fi - 1 : 0, 0, 0, 0);
            if (Vol->FreeInodesCount) Vol->FreeInodesCount--;
            *OutIno = caller;
            return EXT_OK;
        }
    }
    return EXT_NO_SPACE;
}

/* map file block FileBlk, allocating it (and any missing indirect block) */
static EXT_STATUS
ExtOldMapOrAlloc(PEXT_VOL Vol, PEXT_INODE Inode, ULONGLONG FileBlk, PULONG Disk, int *InodeDirty)
{
    ULONG PerBlock = Vol->BlockSize / 4;
    EXT_STATUS S;

    if (FileBlk < EXT_OLD_NDIR) {
        if (Inode->Block[FileBlk] == 0) {
            ULONG b; S = ExtOldAllocBlock(Vol, &b); if (!NT_SUCCESS(S)) return S;
            Inode->Block[FileBlk] = b; *InodeDirty = 1;
        }
        *Disk = Inode->Block[FileBlk];
        return EXT_OK;
    }
    FileBlk -= EXT_OLD_NDIR;
    if (FileBlk < PerBlock) {
        /* single indirect */
        ULONG ind = Inode->Block[EXT_OLD_IND], b;
        EXT_BUF Buf;
        if (ind == 0) { S = ExtOldAllocBlock(Vol, &ind); if (!NT_SUCCESS(S)) return S;
                        Inode->Block[EXT_OLD_IND] = ind; *InodeDirty = 1; }
        S = ExtHostMap(Vol, (ULONGLONG)ind * Vol->SectorsPerBlock, Vol->SectorsPerBlock, EXT_MAP_WRITE, &Buf);
        if (!NT_SUCCESS(S)) return S;
        b = OLD_U32(Buf.Data, (ULONG)FileBlk * 4);
        if (b == 0) {
            S = ExtOldAllocBlock(Vol, &b);
            if (!NT_SUCCESS(S)) { ExtHostUnmap(Vol, &Buf); return S; }
            OLD_PUT32(Buf.Data, (ULONG)FileBlk * 4, b);
            ExtHostSetDirty(Vol, &Buf);
        }
        ExtHostUnmap(Vol, &Buf);
        *Disk = b;
        return EXT_OK;
    }
    /* double/triple indirect: not needed for the modest files the driver
       creates - fail cleanly rather than risk a wrong map */
    return EXT_NO_SPACE;
}

/* append (or splice) a directory entry naming disk inode ChildDisk */
static EXT_STATUS
ExtOldAddDirEntry(PEXT_VOL Vol, ULONG DirIno, PEXT_INODE Dir,
                  const char *Name, ULONG NameLen, ULONG ChildDisk)
{
    ULONGLONG Size = Dir->SizeLo;
    ULONG need = 8 + ((NameLen + 3) & ~3u);
    ULONGLONG FileBlk;
    EXT_STATUS S;
    int idirty = 0;

    for (FileBlk = 0; FileBlk < (Size >> Vol->BlockShift); FileBlk++) {
        ULONG disk; EXT_BUF Buf; ULONG pos = 0;
        ULONGLONG d64; ULONG run;
        S = ExtOldMapFileBlock(Vol, Dir, FileBlk, &d64, &run);
        if (!NT_SUCCESS(S)) return S;
        disk = (ULONG)d64;
        if (disk == 0) continue;
        S = ExtHostMap(Vol, (ULONGLONG)disk * Vol->SectorsPerBlock, Vol->SectorsPerBlock, EXT_MAP_WRITE, &Buf);
        if (!NT_SUCCESS(S)) return S;
        while (pos < Vol->BlockSize) {
            ULONG eino = OLD_U32(Buf.Data, pos);
            USHORT erec = OLD_U16(Buf.Data, pos + 4);
            USHORT enl  = OLD_U16(Buf.Data, pos + 6);
            ULONG eused = (eino != 0) ? (8 + ((enl + 3) & ~3u)) : 0;
            if (erec < 8 || pos + erec > Vol->BlockSize) break;
            if (erec - eused >= need) {
                ULONG off = pos + (eused ? eused : 0);
                if (eused) { OLD_PUT16(Buf.Data, pos + 4, (USHORT)eused); }
                else off = pos;
                {
                    USHORT newrec = (USHORT)(erec - (eused ? eused : 0));
                    OLD_PUT32(Buf.Data, off, ChildDisk);
                    OLD_PUT16(Buf.Data, off + 4, newrec);
                    OLD_PUT16(Buf.Data, off + 6, (USHORT)NameLen);
                    ExtCopy(Buf.Data + off + 8, Name, NameLen);
                }
                ExtHostSetDirty(Vol, &Buf);
                ExtHostUnmap(Vol, &Buf);
                return EXT_OK;
            }
            pos += erec;
        }
        ExtHostUnmap(Vol, &Buf);
    }

    /* no room: append a fresh directory block */
    {
        ULONG disk; EXT_BUF Buf;
        S = ExtOldMapOrAlloc(Vol, Dir, Size >> Vol->BlockShift, &disk, &idirty);
        if (!NT_SUCCESS(S)) return S;
        S = ExtHostMap(Vol, (ULONGLONG)disk * Vol->SectorsPerBlock, Vol->SectorsPerBlock, EXT_MAP_WRITE, &Buf);
        if (!NT_SUCCESS(S)) return S;
        ExtZero(Buf.Data, Vol->BlockSize);
        OLD_PUT32(Buf.Data, 0, ChildDisk);
        OLD_PUT16(Buf.Data, 4, (USHORT)Vol->BlockSize);
        OLD_PUT16(Buf.Data, 6, (USHORT)NameLen);
        ExtCopy(Buf.Data + 8, Name, NameLen);
        ExtHostSetDirty(Vol, &Buf);
        ExtHostUnmap(Vol, &Buf);
        Dir->SizeLo += Vol->BlockSize;
        return ExtOldWriteInode(Vol, DirIno, Dir);
    }
}

/* remove the entry named Name from a directory (tombstone by folding its space
   into the previous entry, or zeroing the inode if it is first in the block) */
static EXT_STATUS
ExtOldRemoveDirEntry(PEXT_VOL Vol, PEXT_INODE Dir, const char *Name, ULONG NameLen, PULONG ChildDisk)
{
    ULONGLONG Size = Dir->SizeLo, FileBlk;
    EXT_STATUS S;
    *ChildDisk = 0;
    for (FileBlk = 0; FileBlk < (Size >> Vol->BlockShift); FileBlk++) {
        ULONGLONG d64; ULONG run, disk, pos = 0, prev = (ULONG)-1;
        EXT_BUF Buf;
        S = ExtOldMapFileBlock(Vol, Dir, FileBlk, &d64, &run);
        if (!NT_SUCCESS(S)) return S;
        disk = (ULONG)d64;
        if (disk == 0) continue;
        S = ExtHostMap(Vol, (ULONGLONG)disk * Vol->SectorsPerBlock, Vol->SectorsPerBlock, EXT_MAP_WRITE, &Buf);
        if (!NT_SUCCESS(S)) return S;
        while (pos < Vol->BlockSize) {
            ULONG eino = OLD_U32(Buf.Data, pos);
            USHORT erec = OLD_U16(Buf.Data, pos + 4);
            USHORT enl  = OLD_U16(Buf.Data, pos + 6);
            if (erec < 8 || pos + erec > Vol->BlockSize) break;
            if (eino != 0 && enl == NameLen &&
                OldNameEq(Buf.Data + pos + 8, Name, NameLen)) {
                *ChildDisk = eino;
                if (prev != (ULONG)-1) {
                    USHORT prec = OLD_U16(Buf.Data, prev + 4);
                    OLD_PUT16(Buf.Data, prev + 4, (USHORT)(prec + erec));
                } else {
                    OLD_PUT32(Buf.Data, pos, 0);   /* first entry: just clear inode */
                }
                ExtHostSetDirty(Vol, &Buf);
                ExtHostUnmap(Vol, &Buf);
                return EXT_OK;
            }
            prev = pos;
            pos += erec;
        }
        ExtHostUnmap(Vol, &Buf);
    }
    return EXT_NOT_FOUND;
}

/* ---- high-level operations (caller inode numbers throughout) ---- */

/* create a file (Mode has S_IFREG) or directory (S_IFDIR) named Name in Parent */
EXT_STATUS
ExtOldCreate(PEXT_VOL Vol, ULONG ParentIno, const char *Name, ULONG NameLen,
             USHORT Mode, PULONG NewIno)
{
    EXT_INODE Parent, New;
    ULONG ino; EXT_STATUS S;
    int isdir = ((Mode & 0xF000) == 0x4000);

    if (Vol->ReadOnly) return EXT_ACCESS_DENIED;
    S = ExtOldReadInode(Vol, ParentIno, &Parent); if (!NT_SUCCESS(S)) return S;
    S = ExtOldAllocInode(Vol, &ino); if (!NT_SUCCESS(S)) return S;

    ExtZero(&New, sizeof(New));
    New.Mode = Mode;
    New.LinksCount = isdir ? 2 : 1;
    New.SizeLo = 0;
    New.Mtime = 0;

    if (isdir) {
        /* give the directory its first block with "." and ".." */
        ULONG disk; EXT_BUF Buf; int idirty = 0;
        S = ExtOldMapOrAlloc(Vol, &New, 0, &disk, &idirty); if (!NT_SUCCESS(S)) return S;
        New.SizeLo = Vol->BlockSize;
        S = ExtHostMap(Vol, (ULONGLONG)disk * Vol->SectorsPerBlock, Vol->SectorsPerBlock, EXT_MAP_WRITE, &Buf);
        if (!NT_SUCCESS(S)) return S;
        ExtZero(Buf.Data, Vol->BlockSize);
        OLD_PUT32(Buf.Data, 0, ino - 1); OLD_PUT16(Buf.Data, 4, 12); OLD_PUT16(Buf.Data, 6, 1); Buf.Data[8] = '.';
        OLD_PUT32(Buf.Data, 12, ParentIno - 1); OLD_PUT16(Buf.Data, 16, (USHORT)(Vol->BlockSize - 12));
        OLD_PUT16(Buf.Data, 18, 2); Buf.Data[20] = '.'; Buf.Data[21] = '.';
        ExtHostSetDirty(Vol, &Buf); ExtHostUnmap(Vol, &Buf);
    }
    S = ExtOldWriteInode(Vol, ino, &New); if (!NT_SUCCESS(S)) return S;
    S = ExtOldAddDirEntry(Vol, ParentIno, &Parent, Name, NameLen, ino - 1);
    if (!NT_SUCCESS(S)) return S;
    if (isdir) { Parent.LinksCount++; S = ExtOldWriteInode(Vol, ParentIno, &Parent); if (!NT_SUCCESS(S)) return S; }
    if (NewIno) *NewIno = ino;
    return EXT_OK;
}

/* write Len bytes of Data at byte offset Off into file Ino */
EXT_STATUS
ExtOldWriteData(PEXT_VOL Vol, ULONG Ino, ULONGLONG Off, const UCHAR *Data, ULONG Len)
{
    EXT_INODE In; EXT_STATUS S; ULONG done = 0; int idirty = 0;
    if (Vol->ReadOnly) return EXT_ACCESS_DENIED;
    S = ExtOldReadInode(Vol, Ino, &In); if (!NT_SUCCESS(S)) return S;
    while (done < Len) {
        ULONGLONG pos = Off + done;
        ULONGLONG fileBlk = pos >> Vol->BlockShift;
        ULONG inBlk = (ULONG)(pos & (Vol->BlockSize - 1));
        ULONG chunk = Vol->BlockSize - inBlk; ULONG disk;
        EXT_BUF Buf;
        if (chunk > Len - done) chunk = Len - done;
        S = ExtOldMapOrAlloc(Vol, &In, fileBlk, &disk, &idirty); if (!NT_SUCCESS(S)) return S;
        S = ExtHostMap(Vol, (ULONGLONG)disk * Vol->SectorsPerBlock, Vol->SectorsPerBlock, EXT_MAP_WRITE, &Buf);
        if (!NT_SUCCESS(S)) return S;
        ExtCopy(Buf.Data + inBlk, Data + done, chunk);
        ExtHostSetDirty(Vol, &Buf); ExtHostUnmap(Vol, &Buf);
        done += chunk;
    }
    if (Off + Len > In.SizeLo) { In.SizeLo = (ULONG)(Off + Len); idirty = 1; }
    if (idirty) { S = ExtOldWriteInode(Vol, Ino, &In); if (!NT_SUCCESS(S)) return S; }
    return ExtHostFlush(Vol);
}

/* unlink a file (frees its data blocks + inode) */
EXT_STATUS
ExtOldUnlink(PEXT_VOL Vol, ULONG ParentIno, const char *Name, ULONG NameLen)
{
    EXT_INODE Parent, Child; ULONG childDisk = 0, childIno; EXT_STATUS S;
    ULONGLONG fb; int isdir;
    if (Vol->ReadOnly) return EXT_ACCESS_DENIED;
    S = ExtOldReadInode(Vol, ParentIno, &Parent); if (!NT_SUCCESS(S)) return S;
    S = ExtOldRemoveDirEntry(Vol, &Parent, Name, NameLen, &childDisk); if (!NT_SUCCESS(S)) return S;
    childIno = childDisk + 1;
    S = ExtOldReadInode(Vol, childIno, &Child); if (!NT_SUCCESS(S)) return S;
    isdir = ((Child.Mode & 0xF000) == 0x4000);

    /* free direct + single-indirect data blocks */
    for (fb = 0; (ULONGLONG)fb * Vol->BlockSize < Child.SizeLo || fb < EXT_OLD_NDIR; fb++) {
        ULONGLONG d64; ULONG run;
        if (fb >= EXT_OLD_NDIR + (Vol->BlockSize / 4)) break;
        if (!NT_SUCCESS(ExtOldMapFileBlock(Vol, &Child, fb, &d64, &run))) break;
        if (d64) ExtOldFreeBlock(Vol, (ULONG)d64);
        if (fb >= 64 && (ULONGLONG)fb * Vol->BlockSize >= Child.SizeLo) break;
    }
    if (Child.Block[EXT_OLD_IND]) ExtOldFreeBlock(Vol, Child.Block[EXT_OLD_IND]);

    /* release the inode: mode 0, links 0 */
    ExtZero(&Child, sizeof(Child));
    S = ExtOldWriteInode(Vol, childIno, &Child); if (!NT_SUCCESS(S)) return S;
    { ULONG fi; ExtOldSbField(Vol, OLD_SB_FREEINODES, &fi); ExtOldSbSet(Vol, OLD_SB_FREEINODES, fi + 1, 0, 0, 0); }
    Vol->FreeInodesCount++;
    if (isdir && Parent.LinksCount > 2) { Parent.LinksCount--; ExtOldWriteInode(Vol, ParentIno, &Parent); }
    return ExtHostFlush(Vol);
}
