/*
 * EXTNT - mapping a file's logical blocks to disk blocks, for both layouts:
 * the classic indirect-block map (ext2/ext3) and the ext4 extent tree. Also
 * growth (allocating a hole), truncation and freeing all of an inode's data.
 */

#ifdef EXT_KERNEL
#include "ext.h"
#else
#include "extusr.h"
#include "extsup.h"
#endif

#define PTRS_PER_BLOCK(Vol)     ((Vol)->BlockSize / 4)

/* ------------------------------------------------------------------ */
/* Extent tree checksum                                                */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* Indirect block map lookup (ext2/ext3)                               */
/* ------------------------------------------------------------------ */

/* Translate FileBlk into the chain of indirection: returns the number of
   levels (0 = direct) and the index at each level. */
static int
ExtIndirectPath(PEXT_VOL Vol, ULONGLONG FileBlk, ULONG Offsets[4])
{
    ULONG Ptrs = PTRS_PER_BLOCK(Vol);
    ULONGLONG DirBlocks = EXT_NDIR_BLOCKS;
    ULONGLONG IndCount = Ptrs;
    ULONGLONG DindCount = (ULONGLONG)Ptrs * Ptrs;

    if (FileBlk < DirBlocks) {
        Offsets[0] = (ULONG)FileBlk;
        return 0;
    }
    FileBlk -= DirBlocks;
    if (FileBlk < IndCount) {
        Offsets[0] = EXT_IND_BLOCK;
        Offsets[1] = (ULONG)FileBlk;
        return 1;
    }
    FileBlk -= IndCount;
    if (FileBlk < DindCount) {
        Offsets[0] = EXT_DIND_BLOCK;
        Offsets[1] = (ULONG)(FileBlk / Ptrs);
        Offsets[2] = (ULONG)(FileBlk % Ptrs);
        return 2;
    }
    FileBlk -= DindCount;
    Offsets[0] = EXT_TIND_BLOCK;
    Offsets[1] = (ULONG)(FileBlk / ((ULONGLONG)Ptrs * Ptrs));
    Offsets[2] = (ULONG)((FileBlk / Ptrs) % Ptrs);
    Offsets[3] = (ULONG)(FileBlk % Ptrs);
    return 3;
}

static EXT_STATUS
ExtIndirectLookup(PEXT_VOL Vol, PEXT_INODE Inode, ULONGLONG FileBlk,
                  PULONGLONG Disk, PULONG RunLen)
{
    ULONG Offsets[4];
    int Levels = ExtIndirectPath(Vol, FileBlk, Offsets);
    ULONG Cur = Inode->Block[Offsets[0]];
    int l;
    EXT_BUF Buf;
    EXT_STATUS S;

    *RunLen = 1;
    if (Levels == 0) {
        /* run of contiguous direct blocks */
        if (Cur != 0) {
            ULONG i = Offsets[0];
            ULONG run = 1;
            while (i + 1 < EXT_NDIR_BLOCKS && Inode->Block[i + 1] == Inode->Block[i] + 1) {
                run++; i++;
            }
            *RunLen = run;
        }
        *Disk = Cur;
        return EXT_OK;
    }
    for (l = 1; l <= Levels; l++) {
        if (Cur == 0) { *Disk = 0; return EXT_OK; }
        if (Cur >= Vol->BlocksCount) return EXT_FILE_CORRUPT;
        S = ExtMapBlocks(Vol, Cur, 1, EXT_MAP_READ, &Buf);
        if (!NT_SUCCESS(S)) return S;
        Cur = ((PULONG)Buf.Data)[Offsets[l]];
        /* extend a run at the leaf level */
        if (l == Levels && Cur != 0) {
            ULONG idx = Offsets[l];
            ULONG run = 1;
            PULONG P = (PULONG)Buf.Data;
            while (idx + 1 < PTRS_PER_BLOCK(Vol) && P[idx + 1] == P[idx] + 1) { run++; idx++; }
            *RunLen = run;
        }
        ExtHostUnmap(Vol, &Buf);
    }
    *Disk = Cur;
    return EXT_OK;
}

EXT_STATUS
ExtMapFileBlock(PEXT_VOL Vol, ULONG Ino, PEXT_INODE Inode, ULONGLONG FileBlk,
                PULONGLONG Disk, PULONG RunLen)
{
    (VOID)Ino;
    if (Vol->OldExt) return ExtOldMapFileBlock(Vol, Inode, FileBlk, Disk, RunLen);
    if ((Inode->Flags & EXT_IF_INLINE_DATA) || ExtIsFastSymlink(Vol, Inode)) { *Disk = 0; *RunLen = 1; return EXT_OK; }
    if (Inode->Flags & EXT_IF_EXTENTS) {
        return ExtExtentMap(Vol, Inode, FileBlk, Disk, RunLen);
    }
    return ExtIndirectLookup(Vol, Inode, FileBlk, Disk, RunLen);
}

/* ------------------------------------------------------------------ */
/* Growth: make a file block present                                   */
/* ------------------------------------------------------------------ */

/* Allocate a fresh metadata/data block, near Near, and zero it if meta */
static EXT_STATUS
ExtNewBlock(PEXT_VOL Vol, PEXT_INODE Inode, ULONGLONG Near, BOOLEAN Zero, PULONGLONG Out)
{
    ULONGLONG Blk;
    EXT_STATUS S = ExtAllocBlock(Vol, Near, &Blk);
    if (!NT_SUCCESS(S)) return S;
    if (Zero) {
        EXT_BUF Buf;
        S = ExtMapBlocks(Vol, Blk, 1, EXT_MAP_NEW, &Buf);
        if (!NT_SUCCESS(S)) { ExtFreeBlock(Vol, Blk); return S; }
        ExtSetDirty(Vol, &Buf);
        ExtHostUnmap(Vol, &Buf);
    }
    ExtInodeSetBlocks(Vol, Inode, ExtInodeBlocks(Vol, Inode) + Vol->SectorsPerBlock);
    *Out = Blk;
    return EXT_OK;
}

/* The preferred allocation group for an inode's data (near its inode table) */
ULONGLONG
ExtGroupBase(PEXT_VOL Vol, ULONG Ino)
{
    ULONG g = (Ino - 1) / Vol->InodesPerGroup;
    return (ULONGLONG)g * Vol->BlocksPerGroup + Vol->FirstDataBlock;
}

/* Indirect-map growth: make FileBlk present, allocating indirect blocks.
   Data blocks are placed right after the previous file block when possible. */
static EXT_STATUS
ExtIndirectGet(PEXT_VOL Vol, ULONG Ino, PEXT_INODE Inode, ULONGLONG FileBlk, PULONGLONG Disk,
               PBOOLEAN Fresh)
{
    ULONG Offsets[4];
    int Levels = ExtIndirectPath(Vol, FileBlk, Offsets);
    int l;
    ULONGLONG Blk, Goal;
    EXT_STATUS S;

    if (Levels == 0) {
        if (Inode->Block[Offsets[0]] == 0) {
            Goal = (Offsets[0] > 0 && Inode->Block[Offsets[0] - 1]) ? Inode->Block[Offsets[0] - 1] + 1
                                                                      : ExtGroupBase(Vol, Ino);
            S = ExtNewBlock(Vol, Inode, Goal, FALSE, &Blk);
            if (!NT_SUCCESS(S)) return S;
            Inode->Block[Offsets[0]] = (ULONG)Blk;
            *Fresh = TRUE;
        }
        *Disk = Inode->Block[Offsets[0]];
        return EXT_OK;
    }

    if (Inode->Block[Offsets[0]] == 0) {
        Goal = Inode->Block[EXT_NDIR_BLOCKS - 1] ? Inode->Block[EXT_NDIR_BLOCKS - 1] + 1 : ExtGroupBase(Vol, Ino);
        S = ExtNewBlock(Vol, Inode, Goal, TRUE, &Blk);
        if (!NT_SUCCESS(S)) return S;
        Inode->Block[Offsets[0]] = (ULONG)Blk;
    }
    Blk = Inode->Block[Offsets[0]];

    for (l = 1; l <= Levels; l++) {
        EXT_BUF Buf;
        PULONG P;
        ULONG Idx = Offsets[l];
        if (Blk == 0 || Blk >= Vol->BlocksCount) return EXT_FILE_CORRUPT;
        S = ExtMapBlocks(Vol, Blk, 1, EXT_MAP_WRITE, &Buf);
        if (!NT_SUCCESS(S)) return S;
        P = (PULONG)Buf.Data;
        if (P[Idx] == 0) {
            ULONGLONG New;
            BOOLEAN Meta = (BOOLEAN)(l < Levels);
            Goal = (Idx > 0 && P[Idx - 1]) ? (ULONGLONG)P[Idx - 1] + 1 : Blk + 1;
            S = ExtNewBlock(Vol, Inode, Goal, Meta, &New);
            if (!NT_SUCCESS(S)) { ExtHostUnmap(Vol, &Buf); return S; }
            P[Idx] = (ULONG)New;
            ExtSetDirty(Vol, &Buf);
            if (!Meta) *Fresh = TRUE;
        }
        Blk = P[Idx];
        ExtHostUnmap(Vol, &Buf);
    }
    *Disk = Blk;
    return EXT_OK;
}

EXT_STATUS
ExtGetFileBlock(PEXT_VOL Vol, ULONG Ino, PEXT_INODE Inode, ULONGLONG FileBlk, PULONGLONG Disk,
                PBOOLEAN Fresh)
{
    BOOLEAN Dummy;
    if (!Fresh) Fresh = &Dummy;
    *Fresh = FALSE;
    if (Vol->ReadOnly) return EXT_READ_ONLY;
    if (Inode->Flags & EXT_IF_INLINE_DATA) return EXT_NOT_SUPPORTED;
    if (Inode->Flags & EXT_IF_EXTENTS) return ExtExtentGet(Vol, Ino, Inode, FileBlk, Disk, Fresh);
    if (Vol->ClusterBits) return EXT_NOT_SUPPORTED;     /* bigalloc needs extent-mapped files */
    {
        /* indirect map: block numbers are 32-bit; the map covers a bounded range */
        ULONGLONG Ptrs = PTRS_PER_BLOCK(Vol);
        if (FileBlk >= EXT_NDIR_BLOCKS + Ptrs + Ptrs * Ptrs + Ptrs * Ptrs * Ptrs) return EXT_TOO_LARGE;
    }
    {
        ULONGLONG D; ULONG Run;
        EXT_STATUS S = ExtMapFileBlock(Vol, Ino, Inode, FileBlk, &D, &Run);
        if (!NT_SUCCESS(S)) return S;
        if (D != 0) { *Disk = D; return EXT_OK; }
    }
    return ExtIndirectGet(Vol, Ino, Inode, FileBlk, Disk, Fresh);
}

/* ------------------------------------------------------------------ */
/* Freeing all data / truncation                                       */
/* ------------------------------------------------------------------ */

/* Recursive indirect truncation: free everything mapping file blocks >= From
   under a metadata block at the given level, freeing a metadata block when it
   has no pointers left. BaseBlk is the first file block this subtree maps. */
static EXT_STATUS
ExtTruncIndBlock(PEXT_VOL Vol, PEXT_INODE Inode, ULONG MetaBlock, int Level,
                 ULONGLONG BaseBlk, ULONGLONG From, PBOOLEAN Emptied)
{
    EXT_BUF Buf;
    EXT_STATUS S;
    PULONG P;
    ULONG i, Ptrs = PTRS_PER_BLOCK(Vol);
    ULONGLONG Span = 1;
    int k;
    BOOLEAN AnyLeft = FALSE;

    for (k = 1; k < Level; k++) Span *= Ptrs;

    S = ExtMapBlocks(Vol, MetaBlock, 1, EXT_MAP_WRITE, &Buf);
    if (!NT_SUCCESS(S)) return S;
    P = (PULONG)Buf.Data;
    for (i = 0; i < Ptrs; i++) {
        ULONGLONG Start = BaseBlk + (ULONGLONG)i * Span;
        if (P[i] == 0) continue;
        if (Level == 1) {
            if (Start >= From) { ExtFreeBlock(Vol, P[i]); P[i] = 0; ExtSetDirty(Vol, &Buf); }
            else AnyLeft = TRUE;
        } else if (Start + Span <= From) {
            AnyLeft = TRUE;
        } else if (Start >= From) {
            ULONG Child = P[i];
            ExtHostUnmap(Vol, &Buf);
            ExtFreeIndirectTree(Vol, Child, Level - 1);
            S = ExtMapBlocks(Vol, MetaBlock, 1, EXT_MAP_WRITE, &Buf);
            if (!NT_SUCCESS(S)) return S;
            P = (PULONG)Buf.Data; P[i] = 0; ExtSetDirty(Vol, &Buf);
        } else {
            BOOLEAN ChildEmpty = FALSE;
            ULONG Child = P[i];
            ExtHostUnmap(Vol, &Buf);
            ExtTruncIndBlock(Vol, Inode, Child, Level - 1, Start, From, &ChildEmpty);
            S = ExtMapBlocks(Vol, MetaBlock, 1, EXT_MAP_WRITE, &Buf);
            if (!NT_SUCCESS(S)) return S;
            P = (PULONG)Buf.Data;
            if (ChildEmpty) { ExtFreeBlock(Vol, P[i]); P[i] = 0; ExtSetDirty(Vol, &Buf); }
            else AnyLeft = TRUE;
        }
    }
    ExtHostUnmap(Vol, &Buf);
    if (Emptied) *Emptied = (BOOLEAN)!AnyLeft;
    return EXT_OK;
}

static EXT_STATUS
ExtTruncIndirect(PEXT_VOL Vol, PEXT_INODE Inode, ULONGLONG From)
{
    ULONG Ptrs = PTRS_PER_BLOCK(Vol);
    ULONG i;
    ULONGLONG IndStart = EXT_NDIR_BLOCKS;
    ULONGLONG DindStart = IndStart + Ptrs;
    ULONGLONG TindStart = DindStart + (ULONGLONG)Ptrs * Ptrs;
    BOOLEAN Empty;

    for (i = 0; i < EXT_NDIR_BLOCKS; i++) {
        if ((ULONGLONG)i >= From && Inode->Block[i]) { ExtFreeBlock(Vol, Inode->Block[i]); Inode->Block[i] = 0; }
    }
    if (Inode->Block[EXT_IND_BLOCK]) {
        if (From <= IndStart) { ExtFreeIndirectTree(Vol, Inode->Block[EXT_IND_BLOCK], 1); Inode->Block[EXT_IND_BLOCK] = 0; }
        else { Empty = FALSE; ExtTruncIndBlock(Vol, Inode, Inode->Block[EXT_IND_BLOCK], 1, IndStart, From, &Empty);
               if (Empty) { ExtFreeBlock(Vol, Inode->Block[EXT_IND_BLOCK]); Inode->Block[EXT_IND_BLOCK] = 0; } }
    }
    if (Inode->Block[EXT_DIND_BLOCK]) {
        if (From <= DindStart) { ExtFreeIndirectTree(Vol, Inode->Block[EXT_DIND_BLOCK], 2); Inode->Block[EXT_DIND_BLOCK] = 0; }
        else { Empty = FALSE; ExtTruncIndBlock(Vol, Inode, Inode->Block[EXT_DIND_BLOCK], 2, DindStart, From, &Empty);
               if (Empty) { ExtFreeBlock(Vol, Inode->Block[EXT_DIND_BLOCK]); Inode->Block[EXT_DIND_BLOCK] = 0; } }
    }
    if (Inode->Block[EXT_TIND_BLOCK]) {
        if (From <= TindStart) { ExtFreeIndirectTree(Vol, Inode->Block[EXT_TIND_BLOCK], 3); Inode->Block[EXT_TIND_BLOCK] = 0; }
        else { Empty = FALSE; ExtTruncIndBlock(Vol, Inode, Inode->Block[EXT_TIND_BLOCK], 3, TindStart, From, &Empty);
               if (Empty) { ExtFreeBlock(Vol, Inode->Block[EXT_TIND_BLOCK]); Inode->Block[EXT_TIND_BLOCK] = 0; } }
    }
    return EXT_OK;
}

EXT_STATUS
ExtFreeIndirectTree(PEXT_VOL Vol, ULONG Block, int Level)
{
    EXT_BUF Buf;
    EXT_STATUS S;
    PULONG P;
    ULONG i, Ptrs = PTRS_PER_BLOCK(Vol);
    ULONG Snapshot;

    if (Block == 0) return EXT_OK;
    if (Level > 1) {
        S = ExtMapBlocks(Vol, Block, 1, EXT_MAP_READ, &Buf);
        if (!NT_SUCCESS(S)) return S;
        P = (PULONG)Buf.Data;
        for (i = 0; i < Ptrs; i++) {
            if (P[i]) {
                Snapshot = P[i];
                ExtHostUnmap(Vol, &Buf);
                ExtFreeIndirectTree(Vol, Snapshot, Level - 1);
                S = ExtMapBlocks(Vol, Block, 1, EXT_MAP_READ, &Buf);
                if (!NT_SUCCESS(S)) return S;
                P = (PULONG)Buf.Data;
            }
        }
        ExtHostUnmap(Vol, &Buf);
    } else {
        S = ExtMapBlocks(Vol, Block, 1, EXT_MAP_READ, &Buf);
        if (!NT_SUCCESS(S)) return S;
        P = (PULONG)Buf.Data;
        for (i = 0; i < Ptrs; i++) if (P[i]) ExtFreeBlock(Vol, P[i]);
        ExtHostUnmap(Vol, &Buf);
    }
    return ExtFreeBlock(Vol, Block);
}

EXT_STATUS
ExtFreeInodeData(PEXT_VOL Vol, ULONG Ino, PEXT_INODE Inode)
{
    USHORT Mode = Inode->Mode & EXT_S_IFMT;

    /* Fast symlink and inline data have no external blocks */
    if (Inode->Flags & EXT_IF_INLINE_DATA) goto Xattr;
    if (Mode == EXT_S_IFLNK && ExtIsFastSymlink(Vol, Inode)) {
        goto Xattr;
    }
    if (Inode->Flags & EXT_IF_EXTENTS) {
        ExtExtentFreeAll(Vol, Ino, Inode);
    } else {
        ULONG i;
        for (i = 0; i < EXT_NDIR_BLOCKS; i++) if (Inode->Block[i]) ExtFreeBlock(Vol, Inode->Block[i]);
        if (Inode->Block[EXT_IND_BLOCK]) ExtFreeIndirectTree(Vol, Inode->Block[EXT_IND_BLOCK], 1);
        if (Inode->Block[EXT_DIND_BLOCK]) ExtFreeIndirectTree(Vol, Inode->Block[EXT_DIND_BLOCK], 2);
        if (Inode->Block[EXT_TIND_BLOCK]) ExtFreeIndirectTree(Vol, Inode->Block[EXT_TIND_BLOCK], 3);
    }
Xattr:
    ExtXattrRelease(Vol, Ino, Inode);       /* shared blocks and ea_inodes are reference counted */
    ExtZero(Inode->Block, sizeof(Inode->Block));
    if (Inode->Flags & EXT_IF_EXTENTS) {
        /* an empty but valid extent root, as Linux leaves it: e2fsck takes
           an inode table block with many bad roots for garbage */
        PEXT_EXTENT_HEADER H = (PEXT_EXTENT_HEADER)Inode->Block;
        H->Magic = EXT_EXTENT_MAGIC;
        H->Max = (USHORT)((sizeof(Inode->Block) - sizeof(EXT_EXTENT_HEADER)) / sizeof(EXT_EXTENT));
    }
    ExtInodeSetBlocks(Vol, Inode, 0);
    return EXT_OK;
}

static EXT_STATUS
ExtTruncateBlocksInner(PEXT_VOL Vol, ULONG Ino, PEXT_INODE Inode, ULONGLONG NewBlocks)
{
    (VOID)Ino;
    if (Vol->ReadOnly) return EXT_READ_ONLY;
    if (Inode->Flags & EXT_IF_INLINE_DATA) return EXT_OK;

    if (Inode->Flags & EXT_IF_EXTENTS) return ExtExtentTruncate(Vol, Ino, Inode, NewBlocks);

    return ExtTruncIndirect(Vol, Inode, NewBlocks);
}

EXT_STATUS
ExtTruncateBlocks(PEXT_VOL Vol, ULONG Ino, PEXT_INODE Inode, ULONGLONG NewBlocks)
{
    ULONGLONG Before = Vol->FreeTotal - Vol->AllocTotal, Freed, Have;
    EXT_STATUS S;
    if (Inode->Flags & EXT_IF_EXTENTS) return ExtTruncateBlocksInner(Vol, Ino, Inode, NewBlocks);
    S = ExtTruncateBlocksInner(Vol, Ino, Inode, NewBlocks);
    Freed = (Vol->FreeTotal - Vol->AllocTotal) - Before;    /* net blocks given up */
    Have = ExtInodeBlocks(Vol, Inode);
    Freed *= Vol->SectorsPerBlock;
    ExtInodeSetBlocks(Vol, Inode, Have > Freed ? Have - Freed : 0);
    return S;
}
