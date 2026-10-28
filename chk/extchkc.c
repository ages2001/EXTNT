/*
 * EXTNT - checking and repairing an ext2/ext3/ext4 file system
 *
 * The passes follow e2fsck:
 *   0  superblock and group descriptors; the journal is replayed and the
 *      orphan list processed (when repairing)
 *   1  every inode: type, checksum, flags, its block map or extent tree
 *      (each block claimed once: duplicates, blocks outside the volume or on
 *      its metadata), i_blocks, i_size, the external xattr block
 *   1b blocks claimed twice: every owner but the first gets copies
 *   2  every directory: entries, record lengths, names, file types, the
 *      checksum tail, "." and "..", the htree root
 *   3  connectivity: each directory reachable from the root, ".." right;
 *      lost ones go to lost+found
 *   4  link counts; files no directory names go to lost+found
 *   5  block and inode bitmaps, free counts and group summaries
 * Repairs that need new blocks (an extent tree rebuilt, a duplicate copied,
 * a directory hole filled, lost+found made) wait until the bitmaps have been
 * rebuilt from what pass 1 found, so nothing in use is handed out; the
 * passes then run again from the start, until a run finds nothing to fix.
 */

#include "extusr.h"
#include "extsup.h"
#include "extimg.h"
#include "extchkc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#define LINK_MAX            65000

#define XLEN(Raw)           ((ULONG)((Raw) > EXT_EXTENT_INIT_MAX ? (Raw) - EXT_EXTENT_INIT_MAX : (Raw)))
#define XUNINIT(Raw)        ((Raw) > EXT_EXTENT_INIT_MAX)
#define XSTART(E)           ((E)->StartLo | ((ULONGLONG)(E)->StartHi << 32))
#define XCHILD(I)           ((I)->LeafLo | ((ULONGLONG)(I)->LeafHi << 32))

/* per inode */
#define ST_DIR_REACHED      0x01
#define ST_DIR_VISITING     0x02
#define ST_ORPHAN_OK        0x04

typedef struct _EXTLIST {
    ULONG      *Lblk;
    ULONG      *Len;            /* raw (uninit flag included) */
    ULONGLONG  *Start;
    ULONG       Count, Cap;
} EXTLIST;

typedef struct _PENDING {
    ULONG       Ino;
    int         Kind;           /* 1 rebuild extents, 2 clone duplicates */
    EXTLIST     List;
} PENDING;

typedef struct _XREF {
    ULONGLONG   Block;
    ULONG       Refs;
} XREF;

typedef struct _CHK {
    PEXT_IMAGE  Img;
    EXT_VOL     Vol;
    ULONG       Flags;
    BOOLEAN     Fix;
    ULONG       Problems, Repairs;
    BOOLEAN     Restart;
    ULONG       Bs, Spb, Isize;
    ULONGLONG   Blocks;
    ULONG       Inodes, Ipg, Groups, FirstIno;
    PUCHAR      Claimed, Meta, Dup;
    ULONGLONG   DupCount;
    PUCHAR      IType, State;
    USHORT     *Links;
    ULONG      *Parent, *Found;
    ULONG       LostFound;
    PUCHAR      Buf, Buf2, ItBuf;
    PENDING    *Pend;
    ULONG       PendCount, PendCap;
    XREF       *XRefs;
    ULONG       XRefCount, XRefCap;
    ULONG       DirCount, FileCount;
} CHK;

/* ------------------------------------------------------------------ */
/* Reporting                                                           */
/* ------------------------------------------------------------------ */

static BOOLEAN
Problem(CHK *C, const char *Fmt, ...)
{
    char Line[300];
    va_list Ap;
    va_start(Ap, Fmt);
    vsnprintf(Line, sizeof(Line), Fmt, Ap);
    va_end(Ap);
    C->Problems++;
    if (!(C->Flags & EXT_CHECK_QUIET)) printf("  %s%s\n", Line, C->Fix ? " - fixed" : "");
    if (C->Fix) C->Repairs++;
    return C->Fix;
}

static void
Note(CHK *C, const char *Fmt, ...)
{
    va_list Ap;
    char Line[300];
    if (!(C->Flags & EXT_CHECK_VERBOSE)) return;
    va_start(Ap, Fmt);
    vsnprintf(Line, sizeof(Line), Fmt, Ap);
    va_end(Ap);
    printf("%s\n", Line);
}

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

static int  TestBit(PUCHAR B, ULONGLONG N) { return (B[N >> 3] >> (N & 7)) & 1; }
static void SetBit(PUCHAR B, ULONGLONG N) { B[N >> 3] |= (UCHAR)(1 << (N & 7)); }
static void ClrBit(PUCHAR B, ULONGLONG N) { B[N >> 3] &= (UCHAR)~(1 << (N & 7)); }

static int
ReadBlock(CHK *C, ULONGLONG Block, PUCHAR Buf)
{
    return ImgRead(C->Img, Block * C->Spb, C->Spb, Buf);
}

static int
WriteBlock(CHK *C, ULONGLONG Block, const UCHAR *Buf)
{
    return ImgWrite(C->Img, Block * C->Spb, C->Spb, Buf);
}

static ULONG
RecLen(CHK *C, PEXT_DIR_ENTRY De)
{
    ULONG L = De->RecLen;
    if (C->Bs < 65536) return L;
    if (L == 65535 || L == 0) return 65536;
    return (L & 65532) | ((L & 3) << 16);
}

static void
SetRecLen(CHK *C, PEXT_DIR_ENTRY De, ULONG L)
{
    if (C->Bs < 65536 || L < 65536) De->RecLen = (USHORT)L;
    else if (L == 65536) De->RecLen = 65535;
    else De->RecLen = (USHORT)((L & 65532) | ((L >> 16) & 3));
}

static UCHAR
ModeType(USHORT Mode)
{
    switch (Mode & EXT_S_IFMT) {
    case EXT_S_IFREG:  return EXT_FT_REG_FILE;
    case EXT_S_IFDIR:  return EXT_FT_DIR;
    case EXT_S_IFLNK:  return EXT_FT_SYMLINK;
    case EXT_S_IFCHR:  return EXT_FT_CHRDEV;
    case EXT_S_IFBLK:  return EXT_FT_BLKDEV;
    case EXT_S_IFIFO:  return EXT_FT_FIFO;
    case EXT_S_IFSOCK: return EXT_FT_SOCK;
    }
    return 0;
}

static int
ListAdd(EXTLIST *L, ULONG Lblk, ULONG RawLen, ULONGLONG Start)
{
    if (L->Count == L->Cap) {
        ULONG N = L->Cap ? L->Cap * 2 : 16;
        ULONG *A = (ULONG *)realloc(L->Lblk, N * sizeof(ULONG));
        ULONG *B;
        ULONGLONG *D;
        if (!A) return -1;
        L->Lblk = A;
        B = (ULONG *)realloc(L->Len, N * sizeof(ULONG));
        if (!B) return -1;
        L->Len = B;
        D = (ULONGLONG *)realloc(L->Start, N * sizeof(ULONGLONG));
        if (!D) return -1;
        L->Start = D;
        L->Cap = N;
    }
    L->Lblk[L->Count] = Lblk;
    L->Len[L->Count] = RawLen;
    L->Start[L->Count] = Start;
    L->Count++;
    return 0;
}

static void ListFree(EXTLIST *L) { free(L->Lblk); free(L->Len); free(L->Start); memset(L, 0, sizeof(*L)); }

static void
AddPending(CHK *C, ULONG Ino, int Kind, EXTLIST *List)
{
    if (C->PendCount == C->PendCap) {
        ULONG N = C->PendCap ? C->PendCap * 2 : 16;
        PENDING *P = (PENDING *)realloc(C->Pend, N * sizeof(PENDING));
        if (!P) return;
        C->Pend = P;
        C->PendCap = N;
    }
    C->Pend[C->PendCount].Ino = Ino;
    C->Pend[C->PendCount].Kind = Kind;
    if (List) { C->Pend[C->PendCount].List = *List; memset(List, 0, sizeof(*List)); }
    else memset(&C->Pend[C->PendCount].List, 0, sizeof(EXTLIST));
    C->PendCount++;
}

/* Reads inode Ino as stored (InodeSize bytes) */
static int
ReadInodeRaw(CHK *C, ULONG Ino, PUCHAR Raw)
{
    ULONGLONG Sector;
    ULONG Offset, Need;
    UCHAR Tmp[2048];
    if (!NT_SUCCESS(ExtInodeLocation(&C->Vol, Ino, &Sector, &Offset))) return -1;
    Need = (Offset + C->Isize + 511) / 512;
    if (Need * 512 > sizeof(Tmp)) return -1;
    if (ImgRead(C->Img, Sector, Need, Tmp) != 0) return -1;
    memcpy(Raw, Tmp + Offset, C->Isize);
    return 0;
}

static void
RawToInode(CHK *C, const UCHAR *Raw, PEXT_INODE I)
{
    memset(I, 0, sizeof(*I));
    memcpy(I, Raw, C->Isize < sizeof(*I) ? C->Isize : sizeof(*I));
    if (C->Isize <= EXT_GOOD_OLD_INODE_SIZE) I->ExtraIsize = 0;
}

static void
WriteInode(CHK *C, ULONG Ino, PEXT_INODE I)
{
    if (!NT_SUCCESS(ExtWriteInode(&C->Vol, Ino, I))) {
        printf("  could not write inode %lu\n", (unsigned long)Ino);
    }
}

/* Sets inode Ino free: no links, a deletion time, no blocks */
static void
ClearInode(CHK *C, ULONG Ino, PEXT_INODE I)
{
    USHORT Keep = (USHORT)(I->Flags & EXT_IF_EXTENTS);
    I->LinksCount = 0;
    I->Dtime = ExtHostNow();
    memset(I->Block, 0, sizeof(I->Block));
    if (Keep) {
        PEXT_EXTENT_HEADER H = (PEXT_EXTENT_HEADER)I->Block;
        H->Magic = EXT_EXTENT_MAGIC;
        H->Max = 4;
    }
    I->FileAclLo = 0;
    I->FileAclHi = 0;
    ExtInodeSetSize(&C->Vol, I, 0);
    ExtInodeSetBlocks(&C->Vol, I, 0);
    WriteInode(C, Ino, I);
    C->IType[Ino] = 0;
}

/* ------------------------------------------------------------------ */
/* Static metadata                                                     */
/* ------------------------------------------------------------------ */

static void
MarkRange(CHK *C, ULONGLONG Start, ULONGLONG Count)
{
    ULONGLONG B;
    for (B = Start; B < Start + Count && B < C->Blocks; B++) SetBit(C->Meta, B);
}

static void
BuildMeta(CHK *C)
{
    ULONG G;
    memset(C->Meta, 0, (size_t)((C->Blocks + 7) / 8));
    for (G = 0; G < C->Groups; G++) {
        MarkRange(C, ExtGroupFirstBlock(&C->Vol, G), ExtGroupBaseMeta(&C->Vol, G));
        MarkRange(C, ExtGdBlockBitmap(&C->Vol, G), 1);
        MarkRange(C, ExtGdInodeBitmap(&C->Vol, G), 1);
        MarkRange(C, ExtGdInodeTable(&C->Vol, G), C->Vol.ItbPerGroup);
    }
    /* block 0 of a 1 KB volume lies before the first group */
    if (C->Vol.FirstDataBlock) SetBit(C->Meta, 0);
}

static BOOLEAN
BlockOk(CHK *C, ULONGLONG B, BOOLEAN MetaAllowed)
{
    if (B <= C->Vol.FirstDataBlock && !(MetaAllowed && B == C->Vol.FirstDataBlock)) return FALSE;
    if (B >= C->Blocks) return FALSE;
    if (!MetaAllowed && TestBit(C->Meta, B)) return FALSE;
    return TRUE;
}

/* Claims a block for an inode; FALSE if it was claimed already */
static BOOLEAN
Claim(CHK *C, ULONGLONG B)
{
    if (TestBit(C->Claimed, B)) {
        if (C->Dup == NULL) C->Dup = (PUCHAR)calloc(1, (size_t)((C->Blocks + 7) / 8));
        if (C->Dup && !TestBit(C->Dup, B)) { SetBit(C->Dup, B); C->DupCount++; }
        return FALSE;
    }
    SetBit(C->Claimed, B);
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* Pass 1: an inode's blocks                                           */
/* ------------------------------------------------------------------ */

typedef struct _WALK {
    ULONG       Ino;
    PEXT_INODE  I;
    ULONGLONG   Count;          /* blocks claimed (data and tree) */
    ULONGLONG   Clusters;       /* bigalloc: clusters they take (what i_blocks counts) */
    ULONGLONG   LastC;          /* bigalloc: cluster of the last data block */
    LONGLONG    MaxLblk;        /* last mapped file block, -1 none */
    LONGLONG    LastInit;       /* last initialised file block, -1 none */
    BOOLEAN     Bad;            /* something had to be left out */
    BOOLEAN     Fatal;          /* nothing could be read */
    BOOLEAN     HasDup;
    BOOLEAN     MetaAllowed;    /* the resize inode maps descriptor blocks */
    BOOLEAN     Dirty;          /* inode changed in place */
    EXTLIST     Good;
} WALK;

static void
ClaimRun(CHK *C, WALK *W, ULONGLONG Start, ULONG Len)
{
    ULONG k;
    for (k = 0; k < Len; k++) {
        ULONGLONG Cl = (Start + k) >> C->Vol.ClusterBits;
        if (!Claim(C, Start + k)) W->HasDup = TRUE;
        /* data comes in file order: the blocks of a cluster are together */
        if (Cl != W->LastC) { W->Clusters++; W->LastC = Cl; }
    }
    W->Count += Len;
}

/* A tree block: a cluster of its own */
static void
ClaimNode(CHK *C, WALK *W, ULONGLONG B)
{
    if (!Claim(C, B)) W->HasDup = TRUE;
    W->Count++;
    W->Clusters++;
}

static void
ExtentNode(CHK *C, WALK *W, PEXT_EXTENT_HEADER H, ULONG Depth, ULONG Max, ULONGLONG NodeBlock,
           ULONGLONG Lo, ULONGLONG Hi)
{
    ULONG i;
    if (Depth > 0) {
        PEXT_EXTENT_IDX Ix = (PEXT_EXTENT_IDX)(H + 1);
        PUCHAR Node = (PUCHAR)malloc(C->Bs);
        if (!Node) return;
        for (i = 0; i < H->Entries; i++) {
            ULONGLONG Child = XCHILD(&Ix[i]);
            ULONGLONG ILo = Ix[i].Block, IHi = (i + 1 < H->Entries) ? Ix[i + 1].Block : Hi;
            PEXT_EXTENT_HEADER Ch = (PEXT_EXTENT_HEADER)Node;
            ULONG NodeMax = (C->Bs - sizeof(EXT_EXTENT_HEADER)) / sizeof(EXT_EXTENT);
            if (ILo < Lo || (i > 0 && Ix[i].Block <= Ix[i - 1].Block) || !BlockOk(C, Child, FALSE) ||
                ReadBlock(C, Child, Node) != 0 ||
                Ch->Magic != EXT_EXTENT_MAGIC || Ch->Depth != Depth - 1 || Ch->Max == 0 ||
                Ch->Max > NodeMax || Ch->Max + 2 < NodeMax || Ch->Entries > Ch->Max) {
                Problem(C, "inode %lu: extent tree node at %llu (index %lu) is bad", (unsigned long)W->Ino,
                        (unsigned long long)Child, (unsigned long)i);
                W->Bad = TRUE;
                continue;
            }
            if (C->Vol.HasMetaCsum) {
                PUCHAR Copy = (PUCHAR)malloc(C->Bs);
                ULONG Off = sizeof(EXT_EXTENT_HEADER) + Ch->Max * sizeof(EXT_EXTENT);
                if (Copy && Off + 4 <= C->Bs) {
                    memcpy(Copy, Node, C->Bs);
                    ExtExtentBlockCsum(&C->Vol, W->Ino, W->I, Copy);
                    if (memcmp(Copy + Off, Node + Off, 4) != 0 &&
                        Problem(C, "inode %lu: extent block %llu checksum is wrong", (unsigned long)W->Ino,
                                (unsigned long long)Child)) {
                        memcpy(Node, Copy, C->Bs);
                        WriteBlock(C, Child, Node);
                    }
                }
                free(Copy);
            }
            ClaimNode(C, W, Child);
            {
                PUCHAR Saved = (PUCHAR)malloc(C->Bs);
                if (Saved) {
                    memcpy(Saved, Node, C->Bs);
                    ExtentNode(C, W, (PEXT_EXTENT_HEADER)Saved, Depth - 1, NodeMax, Child, ILo, IHi);
                    free(Saved);
                }
            }
        }
        free(Node);
        (void)Max; (void)NodeBlock;
        return;
    }
    {
        PEXT_EXTENT E = (PEXT_EXTENT)(H + 1);
        ULONGLONG PrevEnd = Lo;
        for (i = 0; i < H->Entries; i++) {
            ULONG Len = XLEN(E[i].Len);
            ULONGLONG Start = XSTART(&E[i]);
            ULONGLONG k;
            BOOLEAN Ok = (Len != 0 && E[i].Block >= PrevEnd && (ULONGLONG)E[i].Block + Len <= Hi &&
                          (ULONGLONG)E[i].Block + Len <= 0x100000000ULL);
            for (k = 0; Ok && k < Len; k++) if (!BlockOk(C, Start + k, W->MetaAllowed)) Ok = FALSE;
            /* bigalloc: a block's offset in its cluster is the same on both sides */
            if (Ok && ((E[i].Block ^ Start) & ((1u << C->Vol.ClusterBits) - 1))) Ok = FALSE;
            if (!Ok) {
                Problem(C, "inode %lu: extent %lu (file block %lu, %lu blocks at %llu) is bad",
                        (unsigned long)W->Ino, (unsigned long)i, (unsigned long)E[i].Block,
                        (unsigned long)Len, (unsigned long long)Start);
                W->Bad = TRUE;
                continue;
            }
            ClaimRun(C, W, Start, Len);
            ListAdd(&W->Good, E[i].Block, E[i].Len, Start);
            PrevEnd = (ULONGLONG)E[i].Block + Len;
            if ((LONGLONG)PrevEnd - 1 > W->MaxLblk) W->MaxLblk = (LONGLONG)PrevEnd - 1;
            if (!XUNINIT(E[i].Len) && (LONGLONG)PrevEnd - 1 > W->LastInit) W->LastInit = (LONGLONG)PrevEnd - 1;
        }
    }
}

static void
WalkExtents(CHK *C, WALK *W)
{
    PEXT_EXTENT_HEADER H = (PEXT_EXTENT_HEADER)W->I->Block;
    if (H->Magic != EXT_EXTENT_MAGIC || H->Max == 0 || H->Max > 4 || H->Max < 2 ||
        H->Entries > H->Max || H->Depth > 5) {
        Problem(C, "inode %lu: extent tree root is bad", (unsigned long)W->Ino);
        W->Fatal = TRUE;
        return;
    }
    ExtentNode(C, W, H, H->Depth, 4, 0, 0, 0x100000000ULL);
}

/* One level of a block map; returns FALSE when the pointer block must be zeroed */
static void
IndirectBlock(CHK *C, WALK *W, ULONG Block, int Level, ULONGLONG FirstLblk)
{
    ULONG Per = C->Bs / 4, i;
    PULONG P = (PULONG)malloc(C->Bs);
    BOOLEAN Changed = FALSE;
    ULONGLONG Span = 1;
    int k;
    if (!P) return;
    for (k = 1; k < Level; k++) Span *= Per;
    if (ReadBlock(C, Block, (PUCHAR)P) != 0) { free(P); return; }
    for (i = 0; i < Per; i++) {
        if (P[i] == 0) continue;
        if (!BlockOk(C, P[i], W->MetaAllowed)) {
            if (Problem(C, "inode %lu: block pointer %lu in %lu is bad", (unsigned long)W->Ino,
                        (unsigned long)P[i], (unsigned long)Block)) {
                P[i] = 0;
                Changed = TRUE;
            }
            W->Bad = TRUE;
            continue;
        }
        ClaimRun(C, W, P[i], 1);
        if (Level > 1) {
            IndirectBlock(C, W, P[i], Level - 1, FirstLblk + i * Span);
        } else {
            LONGLONG L = (LONGLONG)(FirstLblk + i);
            if (L > W->MaxLblk) W->MaxLblk = L;
            if (L > W->LastInit) W->LastInit = L;
        }
    }
    if (Changed) WriteBlock(C, Block, (PUCHAR)P);
    free(P);
}

static void
WalkIndirect(CHK *C, WALK *W)
{
    ULONG Per = C->Bs / 4, i;
    ULONGLONG First[3];
    First[0] = 12;
    First[1] = 12 + (ULONGLONG)Per;
    First[2] = 12 + (ULONGLONG)Per + (ULONGLONG)Per * Per;
    for (i = 0; i < 15; i++) {
        ULONG B = W->I->Block[i];
        if (B == 0) continue;
        if (!BlockOk(C, B, W->MetaAllowed)) {
            if (Problem(C, "inode %lu: block pointer %lu (slot %lu) is bad", (unsigned long)W->Ino,
                        (unsigned long)B, (unsigned long)i)) {
                W->I->Block[i] = 0;
                W->Dirty = TRUE;
            }
            W->Bad = TRUE;
            continue;
        }
        ClaimRun(C, W, B, 1);
        if (i < 12) {
            if ((LONGLONG)i > W->MaxLblk) W->MaxLblk = i;
            if ((LONGLONG)i > W->LastInit) W->LastInit = i;
        } else {
            IndirectBlock(C, W, B, (int)(i - 11), First[i - 12]);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Pass 1: the external xattr block                                    */
/* ------------------------------------------------------------------ */

static XREF *
XRef(CHK *C, ULONGLONG Block)
{
    ULONG i;
    for (i = 0; i < C->XRefCount; i++) if (C->XRefs[i].Block == Block) return &C->XRefs[i];
    if (C->XRefCount == C->XRefCap) {
        ULONG N = C->XRefCap ? C->XRefCap * 2 : 64;
        XREF *X = (XREF *)realloc(C->XRefs, N * sizeof(XREF));
        if (!X) return NULL;
        C->XRefs = X;
        C->XRefCap = N;
    }
    C->XRefs[C->XRefCount].Block = Block;
    C->XRefs[C->XRefCount].Refs = 0;
    return &C->XRefs[C->XRefCount++];
}

/* TRUE if the block holds a sound xattr block */
static BOOLEAN
XattrBlockOk(CHK *C, ULONGLONG B, PUCHAR Blk)
{
    PEXT_XATTR_HEADER H = (PEXT_XATTR_HEADER)Blk;
    ULONG Off = sizeof(EXT_XATTR_HEADER), MinValue = C->Bs;
    if (ReadBlock(C, B, Blk) != 0 || H->Magic != EXT_XATTR_MAGIC || H->Blocks != 1) return FALSE;
    while (Off + 16 <= C->Bs && *(PULONG)(Blk + Off) != 0) {
        PEXT_XATTR_ENTRY E = (PEXT_XATTR_ENTRY)(Blk + Off);
        ULONG Size = EXT_XATTR_ENTRY_SIZE(E->NameLen);
        if (E->NameLen == 0 || Off + Size > MinValue) return FALSE;
        if (E->ValueInum == 0 && E->ValueSize) {
            if (E->ValueOffs + E->ValueSize > C->Bs || E->ValueOffs < Off + Size) return FALSE;
            if (E->ValueOffs < MinValue) MinValue = E->ValueOffs;
        }
        Off += Size;
    }
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* Pass 1                                                              */
/* ------------------------------------------------------------------ */

static void
CheckInode(CHK *C, ULONG Ino, PUCHAR Raw)
{
    EXT_INODE I;
    WALK W;
    UCHAR Type;
    ULONGLONG Size, Want, Have, Acl;
    BOOLEAN Dirty = FALSE;
    BOOLEAN Reserved = (BOOLEAN)(Ino < C->FirstIno && Ino != EXT_ROOT_INO);

    RawToInode(C, Raw, &I);
    if (I.LinksCount == 0) {
        return;                         /* free (deleted inodes keep their old contents) */
    }

    /* reserved inodes other than the root and the journal: only their blocks matter */
    if (Reserved && Ino != EXT_JOURNAL_INO && Ino != 7 && Ino != 1 && I.Mode == 0) {
        return;
    }

    /* checksum */
    if (C->Vol.HasMetaCsum) {
        PUCHAR Copy = (PUCHAR)malloc(C->Isize);
        if (Copy) {
            memcpy(Copy, Raw, C->Isize);
            ExtInodeCsumImage(&C->Vol, Ino, Copy);
            if (memcmp(Copy + 0x7C, Raw + 0x7C, 2) != 0 ||
                (C->Isize > 128 && I.ExtraIsize >= 4 && memcmp(Copy + 0x82, Raw + 0x82, 2) != 0)) {
                if (Problem(C, "inode %lu checksum is wrong", (unsigned long)Ino)) Dirty = TRUE;
            }
            free(Copy);
        }
    }

    Type = ModeType(I.Mode);
    if (Ino == EXT_ROOT_INO && Type != EXT_FT_DIR) {
        printf("  the root inode is not a directory\n");
        C->Problems++;
        return;
    }
    if (Type == 0 && !Reserved) {
        if (Problem(C, "inode %lu has a bad type (mode %o)", (unsigned long)Ino, I.Mode)) ClearInode(C, Ino, &I);
        return;
    }
    if (C->Isize > 128 && (I.ExtraIsize > C->Isize - 128 || (I.ExtraIsize & 3))) {
        if (Problem(C, "inode %lu: bad extra size %u", (unsigned long)Ino, I.ExtraIsize)) {
            I.ExtraIsize = (USHORT)(C->Isize - 128 >= 32 ? 32 : C->Isize - 128);
            Dirty = TRUE;
        }
    }
    if ((I.Flags & EXT_IF_EXTENTS) && !C->Vol.HasExtents) {
        if (Problem(C, "inode %lu uses extents but the volume does not have them", (unsigned long)Ino)) {
            C->Vol.Super->FeatureIncompat |= EXT_FI_EXTENTS;
            C->Vol.FeatureIncompat |= EXT_FI_EXTENTS;
            C->Vol.HasExtents = TRUE;
            C->Vol.BackupsStale = TRUE;
        }
    }
    if ((I.Flags & EXT_IF_INLINE_DATA) && !C->Vol.HasInlineData) {
        if (Problem(C, "inode %lu has inline data but the volume does not", (unsigned long)Ino)) {
            ClearInode(C, Ino, &I);
            return;
        }
    }
    if (I.Dtime != 0 && Ino >= C->FirstIno && C->Vol.Super->LastOrphan == 0) {
        if (Problem(C, "inode %lu is in use but has a deletion time", (unsigned long)Ino)) {
            I.Dtime = 0;
            Dirty = TRUE;
        }
    }

    memset(&W, 0, sizeof(W));
    W.Ino = Ino;
    W.I = &I;
    W.MaxLblk = W.LastInit = -1;
    W.MetaAllowed = (BOOLEAN)(Ino == 7);

    if (I.Flags & EXT_IF_INLINE_DATA) {
        /* nothing outside the inode */
    } else if (Type == EXT_FT_SYMLINK && ExtIsFastSymlink(&C->Vol, &I)) {
        if (ExtInodeSize(&C->Vol, &I) >= 60) {
            if (Problem(C, "fast symbolic link %lu is too long", (unsigned long)Ino)) {
                ExtInodeSetSize(&C->Vol, &I, 59);
                Dirty = TRUE;
            }
        }
    } else if (Type == EXT_FT_CHRDEV || Type == EXT_FT_BLKDEV || Type == EXT_FT_FIFO || Type == EXT_FT_SOCK) {
        /* device numbers, no blocks */
    } else if (I.Flags & EXT_IF_EXTENTS) {
        WalkExtents(C, &W);
    } else {
        WalkIndirect(C, &W);
        if (W.Dirty) Dirty = TRUE;
    }

    if (W.Fatal) {
        if (Problem(C, "inode %lu cannot be read: cleared", (unsigned long)Ino)) {
            ClearInode(C, Ino, &I);
        }
        ListFree(&W.Good);
        return;
    }
    if (W.Bad && (I.Flags & EXT_IF_EXTENTS)) {
        if (C->Fix) {
            AddPending(C, Ino, 1, &W.Good);
            C->Restart = TRUE;
        }
    }
    if (W.HasDup) {
        AddPending(C, Ino, 2, NULL);
    }
    ListFree(&W.Good);

    /* the external xattr block */
    Acl = I.FileAclLo | ((ULONGLONG)I.FileAclHi << 32);
    if (Acl) {
        if (!BlockOk(C, Acl, FALSE) || !XattrBlockOk(C, Acl, C->Buf)) {
            if (Problem(C, "inode %lu: extended attribute block %llu is bad", (unsigned long)Ino,
                        (unsigned long long)Acl)) {
                I.FileAclLo = 0;
                I.FileAclHi = 0;
                Dirty = TRUE;
            }
        } else {
            XREF *X = XRef(C, Acl);
            if (X) {
                if (X->Refs == 0) {
                    Claim(C, Acl);
                    if (C->Vol.HasMetaCsum) {
                        PEXT_XATTR_HEADER H = (PEXT_XATTR_HEADER)C->Buf;
                        ULONG Was = H->Checksum;
                        ExtXattrBlockCsum(&C->Vol, Acl, C->Buf);
                        if (H->Checksum != Was &&
                            Problem(C, "extended attribute block %llu checksum is wrong", (unsigned long long)Acl)) {
                            WriteBlock(C, Acl, C->Buf);
                        }
                    }
                }
                X->Refs++;
            }
            W.Count++;
            W.Clusters++;
        }
    }

    /* i_blocks */
    Want = C->Vol.ClusterBits ? W.Clusters * C->Vol.SectorsPerCluster : W.Count * C->Spb;
    Have = ExtInodeBlocks(&C->Vol, &I);
    /* (bigalloc: e2fsck counts the resize inode's reserved descriptor
       blocks one cluster each; it is left to its own rules) */
    if (Want != Have && !(I.Flags & EXT_IF_INLINE_DATA && Have == 0) &&
        !(C->Vol.ClusterBits && Ino == EXT_RESIZE_INO)) {
        if (Problem(C, "inode %lu: i_blocks is %llu, should be %llu", (unsigned long)Ino,
                    (unsigned long long)Have, (unsigned long long)Want)) {
            ExtInodeSetBlocks(&C->Vol, &I, Want);
            Dirty = TRUE;
        }
    }

    /* i_size (reserved inodes have their own rules) */
    Size = ExtInodeSize(&C->Vol, &I);
    if (Reserved || Ino == EXT_JOURNAL_INO) {
        /* nothing */
    } else if (Type == EXT_FT_DIR && !(I.Flags & EXT_IF_INLINE_DATA)) {
        ULONGLONG Need = (ULONGLONG)(W.MaxLblk + 1) * C->Bs;
        if ((Size & (C->Bs - 1)) || Size < Need || Size == 0) {
            if (Problem(C, "directory %lu: size %llu should be %llu", (unsigned long)Ino,
                        (unsigned long long)Size, (unsigned long long)(Need ? Need : C->Bs))) {
                ExtInodeSetSize(&C->Vol, &I, Need ? Need : C->Bs);
                Dirty = TRUE;
            }
        }
    } else if ((Type == EXT_FT_REG_FILE || Type == EXT_FT_SYMLINK) && W.LastInit >= 0 &&
               Size <= (ULONGLONG)W.LastInit * C->Bs) {
        ULONGLONG Need = (ULONGLONG)(W.LastInit + 1) * C->Bs;
        if (Problem(C, "inode %lu: size %llu is short of its last block (%llu)", (unsigned long)Ino,
                    (unsigned long long)Size, (unsigned long long)Need)) {
            ExtInodeSetSize(&C->Vol, &I, Need);
            Dirty = TRUE;
        }
    }

    if (Dirty) WriteInode(C, Ino, &I);

    C->IType[Ino] = Type ? Type : EXT_FT_REG_FILE;
    if (Type == EXT_FT_DIR) C->DirCount++; else C->FileCount++;
}

static void
Pass1(CHK *C)
{
    ULONG G, Per = C->Bs / C->Isize;
    Note(C, "Pass 1: inodes, blocks and sizes");
    for (G = 0; G < C->Groups; G++) {
        PEXT_GROUP_DESC D = ExtGroupDesc(&C->Vol, G);
        ULONG Count = C->Ipg, Done, i;
        ULONGLONG Table = ExtGdInodeTable(&C->Vol, G);
        if ((C->Vol.HasMetaCsum || C->Vol.HasGdtCsum)) {
            ULONG Unused = D->ItableUnusedLo | (C->Vol.DescSize >= 64 ? ((ULONG)D->ItableUnusedHi << 16) : 0);
            if (D->Flags & EXT_BG_INODE_UNINIT) Count = 0;
            else if (Unused <= C->Ipg) Count = C->Ipg - Unused;
        }
        for (Done = 0; Done < Count; ) {
            ULONG Blocks = (Count - Done + Per - 1) / Per;
            ULONG FirstBlk = Done / Per;
            if (Blocks > 64) Blocks = 64;
            if (ImgRead(C->Img, (Table + FirstBlk) * C->Spb, Blocks * C->Spb, C->ItBuf) != 0) {
                printf("  cannot read the inode table of group %lu\n", (unsigned long)G);
                C->Problems++;
                break;
            }
            for (i = 0; i < Blocks * Per && Done < Count; i++, Done++) {
                ULONG Ino = G * C->Ipg + Done + 1;
                if (Ino > C->Inodes) break;
                CheckInode(C, Ino, C->ItBuf + (size_t)i * C->Isize);
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Bitmaps (pass 5, also run before repairs that allocate)             */
/* ------------------------------------------------------------------ */

static void
Bitmaps(CHK *C, BOOLEAN Final)
{
    ULONG G, i;
    ULONGLONG FreeTotal = 0;
    ULONG FreeInodesTotal = 0;
    PUCHAR Bm = C->Buf2;
    BOOLEAN AnyGd = FALSE;

    if (Final) Note(C, "Pass 5: bitmaps and summaries");
    for (G = 0; G < C->Groups; G++) {
        PEXT_GROUP_DESC D = ExtGroupDesc(&C->Vol, G);
        ULONGLONG First = ExtGroupFirstBlock(&C->Vol, G);
        ULONG Blks = (ULONG)((First + C->Vol.BlocksPerGroup <= C->Blocks) ? C->Vol.BlocksPerGroup : C->Blocks - First);
        ULONG Len = ExtGroupClusters(&C->Vol, G);     /* bits: clusters (blocks without bigalloc) */
        ULONG Cb = C->Vol.ClusterBits;
        ULONG Used = 0, FreeB, FreeI, UsedI = 0, Dirs = 0, High = 0;
        BOOLEAN Write = FALSE, Changed = FALSE;
        ULONGLONG BmBlock = ExtGdBlockBitmap(&C->Vol, G);

        /* blocks */
        memset(C->Buf, 0, C->Bs);
        for (i = 0; i < Blks; i++) {
            if (TestBit(C->Meta, First + i) || TestBit(C->Claimed, First + i)) SetBit(C->Buf, i >> Cb);
        }
        for (i = 0; i < Len; i++) if (TestBit(C->Buf, i)) Used++;
        for (i = Len; i < C->Bs * 8; i++) SetBit(C->Buf, i);
        if (D->Flags & EXT_BG_BLOCK_UNINIT) {
            memset(Bm, 0, C->Bs);
            for (i = 0; i < Blks; i++) if (TestBit(C->Meta, First + i)) SetBit(Bm, i >> Cb);
            for (i = Len; i < C->Bs * 8; i++) SetBit(Bm, i);
        } else if (ReadBlock(C, BmBlock, Bm) != 0) {
            memset(Bm, 0, C->Bs);
        }
        if (memcmp(Bm, C->Buf, (C->Vol.ClustersPerGroup + 7) / 8) != 0) {
            ULONG Diff = 0;
            for (i = 0; i < Len; i++) if (TestBit(Bm, i) != TestBit(C->Buf, i)) Diff++;
            if (Problem(C, "group %lu: block bitmap differs (%lu %s)", (unsigned long)G, (unsigned long)Diff,
                        Cb ? "clusters" : "blocks")) Write = TRUE;
        } else if (!(D->Flags & EXT_BG_BLOCK_UNINIT) && memcmp(Bm, C->Buf, C->Bs) != 0 && Final) {
            if (Problem(C, "group %lu: block bitmap padding is not set", (unsigned long)G)) Write = TRUE;
        }
        if (Write && C->Fix) {
            WriteBlock(C, BmBlock, C->Buf);
            D->Flags &= ~EXT_BG_BLOCK_UNINIT;
            ExtBitmapCsum(&C->Vol, G, C->Buf, C->Vol.ClustersPerGroup / 8, FALSE);
            Changed = TRUE;
        } else if (C->Vol.HasMetaCsum && !(D->Flags & EXT_BG_BLOCK_UNINIT) && Final) {
            ULONG Was = D->BlockBitmapCsumLo | (C->Vol.DescSize >= 64 ? ((ULONG)D->BlockBitmapCsumHi << 16) : 0);
            ULONG Mask = C->Vol.DescSize >= 64 ? 0xFFFFFFFF : 0xFFFF;
            ULONG Crc = ExtCrc32c(C->Vol.ChecksumSeed, Bm, C->Vol.ClustersPerGroup / 8);
            if ((Crc & Mask) != (Was & Mask) &&
                Problem(C, "group %lu: block bitmap checksum is wrong", (unsigned long)G)) {
                ExtBitmapCsum(&C->Vol, G, Bm, C->Vol.ClustersPerGroup / 8, FALSE);
                Changed = TRUE;
            }
        }
        FreeB = Len - Used;

        /* inodes */
        Write = FALSE;
        memset(C->Buf, 0, C->Bs);
        for (i = 0; i < C->Ipg; i++) {
            ULONG Ino = G * C->Ipg + i + 1;
            if (Ino > C->Inodes) break;
            if (C->IType[Ino] || Ino < C->FirstIno) {
                SetBit(C->Buf, i);
                UsedI++;
                High = i + 1;
                if (C->IType[Ino] == EXT_FT_DIR) Dirs++;
            }
        }
        for (i = C->Ipg; i < C->Bs * 8; i++) SetBit(C->Buf, i);
        if (D->Flags & EXT_BG_INODE_UNINIT) {
            memset(Bm, 0, C->Bs);
            for (i = C->Ipg; i < C->Bs * 8; i++) SetBit(Bm, i);
        } else if (ReadBlock(C, ExtGdInodeBitmap(&C->Vol, G), Bm) != 0) {
            memset(Bm, 0, C->Bs);
        }
        if (memcmp(Bm, C->Buf, (C->Ipg + 7) / 8) != 0) {
            ULONG Diff = 0;
            for (i = 0; i < C->Ipg; i++) if (TestBit(Bm, i) != TestBit(C->Buf, i)) Diff++;
            if (Problem(C, "group %lu: inode bitmap differs (%lu inodes)", (unsigned long)G, (unsigned long)Diff)) Write = TRUE;
        }
        if (Write && C->Fix) {
            WriteBlock(C, ExtGdInodeBitmap(&C->Vol, G), C->Buf);
            D->Flags &= ~EXT_BG_INODE_UNINIT;
            ExtBitmapCsum(&C->Vol, G, C->Buf, (C->Ipg + 7) / 8, TRUE);
            Changed = TRUE;
        } else if (C->Vol.HasMetaCsum && !(D->Flags & EXT_BG_INODE_UNINIT) && Final) {
            ULONG Was = D->InodeBitmapCsumLo | (C->Vol.DescSize >= 64 ? ((ULONG)D->InodeBitmapCsumHi << 16) : 0);
            ULONG Mask = C->Vol.DescSize >= 64 ? 0xFFFFFFFF : 0xFFFF;
            ULONG Crc = ExtCrc32c(C->Vol.ChecksumSeed, Bm, (C->Ipg + 7) / 8);
            if ((Crc & Mask) != (Was & Mask) &&
                Problem(C, "group %lu: inode bitmap checksum is wrong", (unsigned long)G)) {
                ExtBitmapCsum(&C->Vol, G, Bm, (C->Ipg + 7) / 8, TRUE);
                Changed = TRUE;
            }
        }
        FreeI = C->Ipg - UsedI;

        /* summaries */
        if (ExtGdFreeBlocks(&C->Vol, G) != FreeB) {
            if (Problem(C, "group %lu: free blocks %lu, counted %lu", (unsigned long)G,
                        (unsigned long)ExtGdFreeBlocks(&C->Vol, G), (unsigned long)FreeB)) {
                ExtGdSetFreeBlocks(&C->Vol, G, FreeB);
                Changed = TRUE;
            }
        }
        if (ExtGdFreeInodes(&C->Vol, G) != FreeI) {
            if (Problem(C, "group %lu: free inodes %lu, counted %lu", (unsigned long)G,
                        (unsigned long)ExtGdFreeInodes(&C->Vol, G), (unsigned long)FreeI)) {
                ExtGdSetFreeInodes(&C->Vol, G, FreeI);
                Changed = TRUE;
            }
        }
        {
            ULONG Have = D->UsedDirsCountLo | (C->Vol.DescSize >= 64 ? ((ULONG)D->UsedDirsCountHi << 16) : 0);
            if (Have != Dirs) {
                if (Problem(C, "group %lu: directories %lu, counted %lu", (unsigned long)G,
                            (unsigned long)Have, (unsigned long)Dirs)) {
                    ExtGdAddUsedDirs(&C->Vol, G, (LONG)Dirs - (LONG)Have);
                    Changed = TRUE;
                }
            }
        }
        if (C->Vol.HasMetaCsum || C->Vol.HasGdtCsum) {
            ULONG Unused = D->ItableUnusedLo | (C->Vol.DescSize >= 64 ? ((ULONG)D->ItableUnusedHi << 16) : 0);
            if (Unused > C->Ipg - High) {
                if (Problem(C, "group %lu: unused inode count %lu too large", (unsigned long)G,
                            (unsigned long)Unused)) {
                    D->ItableUnusedLo = (USHORT)(C->Ipg - High);
                    if (C->Vol.DescSize >= 64) D->ItableUnusedHi = (USHORT)((C->Ipg - High) >> 16);
                    Changed = TRUE;
                }
            }
        }
        if (Changed && C->Fix) {
            ExtGroupTouched(&C->Vol, G);
            AnyGd = TRUE;
        }
        FreeTotal += (ULONGLONG)FreeB << Cb;            /* the superblock counts blocks */
        FreeInodesTotal += FreeI;
    }

    if (C->Vol.FreeBlocksCount != FreeTotal || C->Vol.FreeInodesCount != FreeInodesTotal) {
        if (Problem(C, "free blocks %llu / inodes %lu, counted %llu / %lu",
                    (unsigned long long)C->Vol.FreeBlocksCount, (unsigned long)C->Vol.FreeInodesCount,
                    (unsigned long long)FreeTotal, (unsigned long)FreeInodesTotal)) {
            C->Vol.FreeBlocksCount = FreeTotal;
            C->Vol.FreeInodesCount = FreeInodesTotal;
            AnyGd = TRUE;
        }
    }
    if (AnyGd && C->Fix) ExtFlushSuper(&C->Vol);
}

/* ------------------------------------------------------------------ */
/* Repairs that allocate                                               */
/* ------------------------------------------------------------------ */

/* Every owner of a duplicate block but the first gets its own copies */
static void
CloneDuplicates(CHK *C, ULONG Ino, PUCHAR FirstOwner)
{
    EXT_INODE I;
    UCHAR Raw[1024];
    ULONGLONG Blk, NBlocks, Disk;
    ULONG Run;
    if (ReadInodeRaw(C, Ino, Raw) != 0) return;
    RawToInode(C, Raw, &I);
    NBlocks = (ExtInodeSize(&C->Vol, &I) + C->Bs - 1) / C->Bs + 64;
    for (Blk = 0; Blk < NBlocks; Blk += Run ? Run : 1) {
        ULONG k;
        Run = 0;
        if (!NT_SUCCESS(ExtMapFileBlock(&C->Vol, Ino, &I, Blk, &Disk, &Run)) || Disk == 0) { Run = 1; continue; }
        for (k = 0; k < Run; k++) {
            ULONGLONG B = Disk + k, NewB;
            /* the inode that claimed it first keeps it: the ones queued here came later */
            if (!C->Dup || !TestBit(C->Dup, B)) continue;
            (void)FirstOwner;
            if (ReadBlock(C, B, C->Buf) != 0) continue;
            if (!NT_SUCCESS(ExtAllocBlock(&C->Vol, B, &NewB))) { printf("  no space to copy block %llu\n", (unsigned long long)B); return; }
            WriteBlock(C, NewB, C->Buf);
            SetBit(C->Claimed, NewB);
            if (I.Flags & EXT_IF_EXTENTS) {
                /* the extent list with this block replaced */
                EXTLIST L;
                ULONGLONG F;
                memset(&L, 0, sizeof(L));
                for (F = 0; F < NBlocks; ) {
                    ULONGLONG D2; ULONG R2 = 0;
                    if (!NT_SUCCESS(ExtMapFileBlock(&C->Vol, Ino, &I, F, &D2, &R2)) || R2 == 0) { F++; continue; }
                    if (D2 == 0) { F += R2; continue; }
                    if (Blk + k >= F && Blk + k < F + R2) {
                        ULONG Before = (ULONG)(Blk + k - F);
                        if (Before) ListAdd(&L, (ULONG)F, Before, D2);
                        ListAdd(&L, (ULONG)(Blk + k), 1, NewB);
                        if (R2 - Before - 1) ListAdd(&L, (ULONG)(Blk + k + 1), R2 - Before - 1, D2 + Before + 1);
                    } else {
                        ListAdd(&L, (ULONG)F, R2, D2);
                    }
                    F += R2;
                }
                ExtExtentRebuild(&C->Vol, Ino, &I, L.Count, L.Lblk, L.Len, L.Start);
                ListFree(&L);
                WriteInode(C, Ino, &I);
            } else {
                /* replace the pointer where it is */
                ULONG Per = C->Bs / 4, i;
                PULONG P = (PULONG)C->Buf2;
                int Done = 0;
                for (i = 0; i < 15 && !Done; i++) if (I.Block[i] == (ULONG)B) { I.Block[i] = (ULONG)NewB; Done = 1; WriteInode(C, Ino, &I); }
                /* search the indirect blocks (one and two levels) */
                for (i = 12; i < 15 && !Done; i++) {
                    ULONG j, j2;
                    if (!I.Block[i] || ReadBlock(C, I.Block[i], (PUCHAR)P) != 0) continue;
                    for (j = 0; j < Per && !Done; j++) if (P[j] == (ULONG)B) { P[j] = (ULONG)NewB; WriteBlock(C, I.Block[i], (PUCHAR)P); Done = 1; }
                    if (i >= 13) {
                        PULONG Q = (PULONG)malloc(C->Bs);
                        for (j = 0; Q && j < Per && !Done; j++) {
                            if (!P[j] || ReadBlock(C, P[j], (PUCHAR)Q) != 0) continue;
                            for (j2 = 0; j2 < Per && !Done; j2++) if (Q[j2] == (ULONG)B) { Q[j2] = (ULONG)NewB; WriteBlock(C, P[j], (PUCHAR)Q); Done = 1; }
                        }
                        free(Q);
                    }
                }
            }
            if (ReadInodeRaw(C, Ino, Raw) == 0) RawToInode(C, Raw, &I);
        }
    }
}

static void
DoPending(CHK *C)
{
    ULONG i;
    PUCHAR FirstOwner = NULL;
    if (C->PendCount == 0) return;
    for (i = 0; i < C->PendCount; i++) {
        PENDING *P = &C->Pend[i];
        UCHAR Raw[1024];
        EXT_INODE I;
        if (!C->Fix) continue;
        if (ReadInodeRaw(C, P->Ino, Raw) != 0) continue;
        RawToInode(C, Raw, &I);
        if (P->Kind == 1) {
            Note(C, "rebuilding the extent tree of inode %lu", (unsigned long)P->Ino);
            if (NT_SUCCESS(ExtExtentRebuild(&C->Vol, P->Ino, &I, P->List.Count, P->List.Lblk, P->List.Len, P->List.Start))) {
                WriteInode(C, P->Ino, &I);
            }
        } else if (P->Kind == 2 && C->Dup) {
            if (!FirstOwner) FirstOwner = (PUCHAR)calloc(1, (size_t)((C->Blocks + 7) / 8));
            if (FirstOwner) CloneDuplicates(C, P->Ino, FirstOwner);
        }
    }
    free(FirstOwner);
    for (i = 0; i < C->PendCount; i++) ListFree(&C->Pend[i].List);
    C->PendCount = 0;
    C->Restart = TRUE;
}

/* ------------------------------------------------------------------ */
/* Pass 2                                                              */
/* ------------------------------------------------------------------ */

static BOOLEAN
EntryTarget(CHK *C, ULONG Ino)
{
    if (Ino == 0 || Ino > C->Inodes) return FALSE;
    if (Ino < C->FirstIno && Ino != EXT_ROOT_INO) return FALSE;
    return (BOOLEAN)(C->IType[Ino] != 0);
}

/* The entries of one block; TRUE if it was changed */
static BOOLEAN
DirBlock(CHK *C, ULONG Dir, PEXT_INODE DI, ULONGLONG Lblk, PUCHAR B, ULONG End, ULONG Limit)
{
    ULONG Off = 0, Prev = (ULONG)-1, Idx = 0;
    BOOLEAN Changed = FALSE;
    while (Off < End && (Limit == 0 || Idx < Limit)) {
        PEXT_DIR_ENTRY De = (PEXT_DIR_ENTRY)(B + Off);
        ULONG Rl = (Off + 8 <= End) ? RecLen(C, De) : 0;
        ULONG NameLen = C->Vol.HasFiletype ? De->NameLen : (De->NameLen | ((ULONG)De->FileType << 8));
        if (Off + 8 > End || Rl < 8 || (Rl & 3) || Off + Rl > End || (De->Inode && (NameLen == 0 || 8 + NameLen > Rl))) {
            if (Problem(C, "directory %lu block %llu: bad entry at %lu", (unsigned long)Dir,
                        (unsigned long long)Lblk, (unsigned long)Off)) {
                if (Off + 8 <= End) {
                    memset(B + Off, 0, End - Off);
                    De->Inode = 0;
                    SetRecLen(C, De, End - Off);
                } else if (Prev != (ULONG)-1) {
                    PEXT_DIR_ENTRY P = (PEXT_DIR_ENTRY)(B + Prev);
                    SetRecLen(C, P, End - Prev);
                }
                Changed = TRUE;
            }
            break;
        }
        if (De->Inode) {
            BOOLEAN Remove = FALSE;
            BOOLEAN IsDot = (BOOLEAN)(NameLen == 1 && De->Name[0] == '.');
            BOOLEAN IsDotDot = (BOOLEAN)(NameLen == 2 && De->Name[0] == '.' && De->Name[1] == '.');
            if (Lblk == 0 && Idx == 0) {
                if (!IsDot) {
                    Problem(C, "directory %lu: first entry is not \".\"", (unsigned long)Dir);
                } else if (De->Inode != Dir) {
                    if (Problem(C, "directory %lu: \".\" names inode %lu", (unsigned long)Dir, (unsigned long)De->Inode)) {
                        De->Inode = Dir;
                        Changed = TRUE;
                    }
                }
            } else if (Lblk == 0 && Idx == 1 && IsDotDot) {
                if (!EntryTarget(C, De->Inode) || C->IType[De->Inode] != EXT_FT_DIR) {
                    if (Problem(C, "directory %lu: \"..\" names a bad inode %lu", (unsigned long)Dir,
                                (unsigned long)De->Inode)) {
                        De->Inode = EXT_ROOT_INO;   /* pass 3 sets the right parent */
                        Changed = TRUE;
                    }
                }
                C->Parent[Dir] = De->Inode;
            } else if (IsDot || IsDotDot) {
                Remove = Problem(C, "directory %lu: stray \"%s\" entry", (unsigned long)Dir, IsDot ? "." : "..");
            } else if (!EntryTarget(C, De->Inode)) {
                Remove = Problem(C, "directory %lu: entry \"%.*s\" names unused inode %lu", (unsigned long)Dir,
                                 (int)NameLen, De->Name, (unsigned long)De->Inode);
            } else {
                ULONG k;
                UCHAR Want = C->IType[De->Inode];
                for (k = 0; k < NameLen; k++) {
                    if (De->Name[k] == '/' || De->Name[k] == 0) {
                        if (Problem(C, "directory %lu: entry name has '/' or NUL", (unsigned long)Dir)) {
                            De->Name[k] = '.';
                            Changed = TRUE;
                        }
                    }
                }
                if (Want == EXT_FT_DIR) {
                    if (De->Inode == EXT_ROOT_INO) {
                        Remove = Problem(C, "directory %lu: entry \"%.*s\" names the root", (unsigned long)Dir,
                                         (int)NameLen, De->Name);
                    } else if (C->Found[De->Inode] != 0 && C->Found[De->Inode] != Dir) {
                        Remove = Problem(C, "directory %lu: entry \"%.*s\" is a second name of directory %lu",
                                         (unsigned long)Dir, (int)NameLen, De->Name, (unsigned long)De->Inode);
                    } else if (C->Found[De->Inode] == Dir) {
                        Remove = Problem(C, "directory %lu: directory %lu is named twice", (unsigned long)Dir,
                                         (unsigned long)De->Inode);
                    } else {
                        C->Found[De->Inode] = Dir;
                    }
                }
                if (!Remove && C->Vol.HasFiletype && De->FileType != Want) {
                    if (Problem(C, "directory %lu: entry \"%.*s\" has file type %u, should be %u", (unsigned long)Dir,
                                (int)NameLen, De->Name, De->FileType, Want)) {
                        De->FileType = Want;
                        Changed = TRUE;
                    }
                }
                if (!Remove && C->Links[De->Inode] < 65535) C->Links[De->Inode]++;
            }
            if (Remove) {
                if (Prev != (ULONG)-1) {
                    PEXT_DIR_ENTRY P = (PEXT_DIR_ENTRY)(B + Prev);
                    SetRecLen(C, P, RecLen(C, P) + Rl);
                    Changed = TRUE;
                    Off += Rl;
                    Idx++;
                    continue;           /* Prev stays */
                }
                De->Inode = 0;
                Changed = TRUE;
            }
        }
        Prev = Off;
        Off += Rl;
        Idx++;
    }
    (void)DI;
    return Changed;
}

/* Block 0 must start with "." and ".."; where the salvage left room, they are made */
static BOOLEAN
EnsureDots(CHK *C, ULONG Dir, PUCHAR B, ULONG End)
{
    PEXT_DIR_ENTRY D0 = (PEXT_DIR_ENTRY)B, D1;
    ULONG R0 = RecLen(C, D0);
    UCHAR Ft = C->Vol.HasFiletype ? EXT_FT_DIR : 0;
    if (D0->Inode == Dir && D0->NameLen == 1 && D0->Name[0] == '.') {
        if (R0 + 12 > End) return FALSE;
        D1 = (PEXT_DIR_ENTRY)(B + R0);
        if (D1->Inode && D1->NameLen == 2 && D1->Name[0] == '.' && D1->Name[1] == '.') return FALSE;
        if (D1->Inode != 0 || RecLen(C, D1) < 12) return FALSE;
        if (!Problem(C, "directory %lu: \"..\" is missing", (unsigned long)Dir)) return FALSE;
        memset(D1->Name, 0, 4);
        D1->Inode = EXT_ROOT_INO; D1->NameLen = 2; D1->FileType = Ft; D1->Name[0] = '.'; D1->Name[1] = '.';
        C->Parent[Dir] = EXT_ROOT_INO;
        return TRUE;
    }
    if (D0->Inode != 0 || R0 < 24) {
        Problem(C, "directory %lu: \".\" is missing (no room to make it)", (unsigned long)Dir);
        return FALSE;
    }
    if (!Problem(C, "directory %lu: \".\" and \"..\" are missing", (unsigned long)Dir)) return FALSE;
    memset(B, 0, 24);
    D1 = (PEXT_DIR_ENTRY)(B + 12);
    D1->Inode = EXT_ROOT_INO; SetRecLen(C, D1, R0 - 12); D1->NameLen = 2; D1->FileType = Ft;
    D1->Name[0] = '.'; D1->Name[1] = '.';
    D0->Inode = Dir; SetRecLen(C, D0, 12); D0->NameLen = 1; D0->FileType = Ft; D0->Name[0] = '.';
    C->Parent[Dir] = EXT_ROOT_INO;
    return TRUE;
}

static void
CheckDirectory(CHK *C, ULONG Dir)
{
    UCHAR Raw[1024];
    EXT_INODE I;
    ULONGLONG N, Lblk;
    ULONG End = C->Vol.HasMetaCsum ? C->Bs - 12 : C->Bs;
    PUCHAR DxBlocks = NULL;     /* logical blocks that are htree nodes */
    BOOLEAN Indexed;

    if (ReadInodeRaw(C, Dir, Raw) != 0) return;
    RawToInode(C, Raw, &I);

    if (I.Flags & EXT_IF_INLINE_DATA) {
        /* entries after the 4-byte parent in i_block (and system.data) */
        ULONG Parent = I.Block[0];
        if (!EntryTarget(C, Parent) || C->IType[Parent] != EXT_FT_DIR) {
            if (Problem(C, "directory %lu (inline): bad parent %lu", (unsigned long)Dir, (unsigned long)Parent)) {
                I.Block[0] = EXT_ROOT_INO;
                WriteInode(C, Dir, &I);
                Parent = EXT_ROOT_INO;
            }
        }
        C->Parent[Dir] = Parent;
        {
            EXT_ENTRY E;
            ULONGLONG Cookie = 0;
            while (NT_SUCCESS(ExtEnum(&C->Vol, Dir, &I, Cookie, &E))) {
                Cookie = E.Cookie;
                if ((E.NameLength == 1 && E.Name[0] == '.') || (E.NameLength == 2 && E.Name[0] == '.' && E.Name[1] == '.')) continue;
                if (!EntryTarget(C, E.Inode)) {
                    if (Problem(C, "directory %lu (inline): entry \"%s\" names unused inode %lu", (unsigned long)Dir,
                                E.Name, (unsigned long)E.Inode)) {
                        ExtRemoveEntry(&C->Vol, Dir, &I, E.Name, E.NameLength);
                        C->Restart = TRUE;
                        break;
                    }
                    continue;
                }
                if (C->IType[E.Inode] == EXT_FT_DIR) {
                    if (C->Found[E.Inode] == 0) C->Found[E.Inode] = Dir;
                    else if (Problem(C, "directory %lu (inline): second name of directory %lu", (unsigned long)Dir,
                                     (unsigned long)E.Inode)) {
                        ExtRemoveEntry(&C->Vol, Dir, &I, E.Name, E.NameLength);
                        C->Restart = TRUE;
                        break;
                    }
                }
                if (C->Links[E.Inode] < 65535) C->Links[E.Inode]++;
            }
        }
        return;
    }

    N = ExtInodeSize(&C->Vol, &I) / C->Bs;
    Indexed = (BOOLEAN)((I.Flags & EXT_IF_INDEX) != 0);
    if (Indexed && N > 1) {
        /* the htree root: "." (12), ".." (rest), then the dx info */
        PUCHAR B = C->Buf;
        ULONGLONG Disk; ULONG Run;
        BOOLEAN Ok = FALSE;
        if (NT_SUCCESS(ExtMapFileBlock(&C->Vol, Dir, &I, 0, &Disk, &Run)) && Disk && ReadBlock(C, Disk, B) == 0) {
            PEXT_DIR_ENTRY D1 = (PEXT_DIR_ENTRY)B, D2 = (PEXT_DIR_ENTRY)(B + 12);
            PUCHAR Info = B + 24;
            ULONG Levels = Info[6], InfoLen = Info[5], HashV = Info[4];
            USHORT Limit = *(PUSHORT)(Info + 8), Count = *(PUSHORT)(Info + 10);
            ULONG MaxLevels = (C->Vol.FeatureIncompat & EXT_FI_LARGEDIR) ? 3 : 2;
            ULONG Room = (C->Bs - 32 - (C->Vol.HasMetaCsum ? 8 : 0)) / 8;
            if (RecLen(C, D1) == 12 && RecLen(C, D2) == C->Bs - 12 && InfoLen == 8 && Levels < MaxLevels &&
                HashV <= 5 && *(PULONG)Info == 0 && Limit == Room && Count >= 1 && Count <= Limit) {
                PEXT_DX_ENTRY Ent = (PEXT_DX_ENTRY)(Info + 8);
                ULONG k;
                Ok = TRUE;
                DxBlocks = (PUCHAR)calloc(1, (size_t)(N / 8 + 1));
                for (k = 0; k < Count && Ok; k++) {
                    ULONG Child = (k == 0) ? Ent[0].Block : Ent[k].Block;
                    if (Child >= N || Child == 0) Ok = FALSE;
                    else if (Levels > 0 && DxBlocks) {
                        /* an interior node: its children are leaves (or further nodes) */
                        PUCHAR NB = C->Buf2;
                        ULONGLONG ND;
                        SetBit(DxBlocks, Child);
                        if (NT_SUCCESS(ExtMapFileBlock(&C->Vol, Dir, &I, Child, &ND, &Run)) && ND && ReadBlock(C, ND, NB) == 0) {
                            USHORT NC = *(PUSHORT)(NB + 8 + 2);
                            PEXT_DX_ENTRY NE = (PEXT_DX_ENTRY)(NB + 8);
                            ULONG m;
                            if (NC == 0 || NC > (C->Bs - 8) / 8) Ok = FALSE;
                            for (m = 1; m < NC && Ok; m++) if (NE[m].Block >= N) Ok = FALSE;
                            if (Levels > 1 && Ok) for (m = 0; m < NC; m++) SetBit(DxBlocks, m == 0 ? NE[0].Block : NE[m].Block);
                        } else {
                            Ok = FALSE;
                        }
                    }
                }
            }
        }
        if (!Ok) {
            if (Problem(C, "directory %lu: htree index is bad (the directory stays, unindexed)", (unsigned long)Dir)) {
                I.Flags &= ~EXT_IF_INDEX;
                WriteInode(C, Dir, &I);
                Indexed = FALSE;
            }
            free(DxBlocks);
            DxBlocks = NULL;
        }
    } else if (Indexed && !C->Vol.HasHtree) {
        if (Problem(C, "directory %lu is indexed but the volume has no dir_index", (unsigned long)Dir)) {
            I.Flags &= ~EXT_IF_INDEX;
            WriteInode(C, Dir, &I);
            Indexed = FALSE;
        }
    }

    for (Lblk = 0; Lblk < N; Lblk++) {
        ULONGLONG Disk;
        ULONG Run = 0;
        BOOLEAN Changed = FALSE;
        if (!NT_SUCCESS(ExtMapFileBlock(&C->Vol, Dir, &I, Lblk, &Disk, &Run)) || Disk == 0) {
            if (Problem(C, "directory %lu has a hole at block %llu", (unsigned long)Dir, (unsigned long long)Lblk)) {
                /* an empty block in its place */
                if (NT_SUCCESS(ExtGetFileBlock(&C->Vol, Dir, &I, Lblk, &Disk, NULL))) {
                    PEXT_DIR_ENTRY De = (PEXT_DIR_ENTRY)C->Buf;
                    memset(C->Buf, 0, C->Bs);
                    SetRecLen(C, De, End);
                    ExtDirBlockCsum(&C->Vol, Dir, &I, C->Buf);
                    WriteBlock(C, Disk, C->Buf);
                    WriteInode(C, Dir, &I);
                    C->Restart = TRUE;
                }
            }
            continue;
        }
        if (ReadBlock(C, Disk, C->Buf) != 0) {
            printf("  cannot read block %llu of directory %lu\n", (unsigned long long)Disk, (unsigned long)Dir);
            C->Problems++;
            continue;
        }
        if (Indexed && DxBlocks && Lblk < N && TestBit(DxBlocks, Lblk)) {
            continue;                       /* an index node: a fake empty entry over the block */
        }
        if (Indexed && Lblk == 0) {
            /* the root block holds "." and ".." only */
            Changed = DirBlock(C, Dir, &I, 0, C->Buf, C->Bs, 2);
            if (Changed) WriteBlock(C, Disk, C->Buf);
            continue;
        }
        if (C->Vol.HasMetaCsum) {
            PEXT_DIR_TAIL T = (PEXT_DIR_TAIL)(C->Buf + C->Bs - 12);
            if (T->ReservedZero1 != 0 || T->RecLen != 12 || T->ReservedZero2 != 0 || T->Reserved0xDE != 0xDE) {
                /* no room for the tail: e2fsck would rebuild the directory */
                Problem(C, "directory %lu block %llu has no checksum tail", (unsigned long)Dir, (unsigned long long)Lblk);
            } else {
                ULONG Was = T->Checksum;
                ExtDirBlockCsum(&C->Vol, Dir, &I, C->Buf);
                if (T->Checksum != Was) {
                    if (Problem(C, "directory %lu block %llu checksum is wrong", (unsigned long)Dir, (unsigned long long)Lblk)) Changed = TRUE;
                    else T->Checksum = Was;
                }
            }
        }
        if (DirBlock(C, Dir, &I, Lblk, C->Buf, End, 0)) Changed = TRUE;
        if (Lblk == 0 && EnsureDots(C, Dir, C->Buf, End)) Changed = TRUE;
        if (Changed && C->Fix) {
            ExtDirBlockCsum(&C->Vol, Dir, &I, C->Buf);
            WriteBlock(C, Disk, C->Buf);
        }
    }
    free(DxBlocks);
}

static void
Pass2(CHK *C)
{
    ULONG Ino;
    Note(C, "Pass 2: directory structure");
    for (Ino = 1; Ino <= C->Inodes; Ino++) {
        if (C->IType[Ino] == EXT_FT_DIR) CheckDirectory(C, Ino);
    }
}

/* ------------------------------------------------------------------ */
/* Passes 3 and 4                                                      */
/* ------------------------------------------------------------------ */

/* lost+found, made if it is missing (repairs only) */
static ULONG
LostAndFound(CHK *C)
{
    EXT_INODE Root, Lf;
    EXT_ENTRY E;
    ULONG Ino;
    if (C->LostFound) return C->LostFound;
    if (!NT_SUCCESS(ExtReadInode(&C->Vol, EXT_ROOT_INO, &Root))) return 0;
    if (NT_SUCCESS(ExtLookup(&C->Vol, EXT_ROOT_INO, &Root, "lost+found", 10, &E)) &&
        E.Inode <= C->Inodes && C->IType[E.Inode] == EXT_FT_DIR) {
        return C->LostFound = E.Inode;
    }
    if (!C->Fix) return 0;
    if (!NT_SUCCESS(ExtCreate(&C->Vol, EXT_ROOT_INO, &Root, "lost+found", 10, (USHORT)(EXT_S_IFDIR | 0700), NULL, &Ino, &Lf))) {
        printf("  cannot make lost+found\n");
        return 0;
    }
    printf("  made lost+found (inode %lu)\n", (unsigned long)Ino);
    C->IType[Ino] = EXT_FT_DIR;
    C->Found[Ino] = EXT_ROOT_INO;
    C->Parent[Ino] = EXT_ROOT_INO;
    C->Links[Ino] = 1;
    SetBit(C->State, Ino);
    C->Restart = TRUE;
    return C->LostFound = Ino;
}

static BOOLEAN
Reconnect(CHK *C, ULONG Ino)
{
    char Name[16];
    EXT_INODE Lf;
    ULONG L = LostAndFound(C);
    if (!L || !NT_SUCCESS(ExtReadInode(&C->Vol, L, &Lf))) return FALSE;
    sprintf(Name, "#%lu", (unsigned long)Ino);
    if (!NT_SUCCESS(ExtAddEntry(&C->Vol, L, &Lf, Name, (ULONG)strlen(Name), Ino, C->IType[Ino]))) {
        printf("  cannot add %s to lost+found\n", Name);
        return FALSE;
    }
    if (C->Links[Ino] < 65535) C->Links[Ino]++;
    if (C->IType[Ino] == EXT_FT_DIR) C->Found[Ino] = L;
    return TRUE;
}

static void
Pass3(CHK *C)
{
    ULONG Ino;
    Note(C, "Pass 3: directory connectivity");
    memset(C->State, 0, (C->Inodes + 8) / 8);
    SetBit(C->State, EXT_ROOT_INO);
    C->Found[EXT_ROOT_INO] = EXT_ROOT_INO;
    for (Ino = 1; Ino <= C->Inodes; Ino++) {
        ULONG Walk, Steps = 0;
        if (C->IType[Ino] != EXT_FT_DIR || TestBit(C->State, Ino)) continue;
        /* up the parents: to the root, or lost */
        for (Walk = Ino; Walk && !TestBit(C->State, Walk) && Steps <= C->DirCount + 1; Steps++) {
            Walk = C->Found[Walk];
        }
        if (Walk && TestBit(C->State, Walk)) {
            for (Walk = Ino; Walk && !TestBit(C->State, Walk); Walk = C->Found[Walk]) SetBit(C->State, Walk);
            continue;
        }
        /* lost (no parent, or a loop): the top of the lost tree goes to lost+found */
        {
            ULONG Top = Ino, k;
            for (k = 0; k <= Steps && C->Found[Top] && C->Found[Top] != Ino; k++) Top = C->Found[Top];
            if (Problem(C, "directory %lu is not connected to the root", (unsigned long)Top)) {
                if (Reconnect(C, Top)) {
                    for (Walk = Ino; Walk && !TestBit(C->State, Walk); Walk = C->Found[Walk]) SetBit(C->State, Walk);
                }
            }
        }
    }
    /* ".." must name the directory holding the entry */
    for (Ino = 1; Ino <= C->Inodes; Ino++) {
        if (C->IType[Ino] != EXT_FT_DIR || !C->Found[Ino] || C->Parent[Ino] == C->Found[Ino]) continue;
        if (Problem(C, "directory %lu: \"..\" is %lu, should be %lu", (unsigned long)Ino,
                    (unsigned long)C->Parent[Ino], (unsigned long)C->Found[Ino])) {
            EXT_INODE I;
            if (NT_SUCCESS(ExtReadInode(&C->Vol, Ino, &I)) && NT_SUCCESS(ExtSetDotDot(&C->Vol, Ino, &I, C->Found[Ino]))) {
                C->Parent[Ino] = C->Found[Ino];
            }
        }
    }
}

/* Inodes the superblock names (journal, quota, orphan file ...): no directory entry */
static BOOLEAN
SuperInode(CHK *C, ULONG Ino)
{
    PEXT_SUPERBLOCK S = C->Vol.Super;
    return (BOOLEAN)(Ino == S->JournalInum || Ino == S->UsrQuotaInum || Ino == S->GrpQuotaInum ||
                     Ino == S->PrjQuotaInum || Ino == S->OrphanFileInum || Ino == S->SnapshotInum);
}

static void
Pass4(CHK *C)
{
    ULONG Ino;
    ULONG *Subdirs = (ULONG *)calloc(C->Inodes + 1, sizeof(ULONG));
    Note(C, "Pass 4: reference counts");
    if (!Subdirs) return;
    for (Ino = 1; Ino <= C->Inodes; Ino++) {
        if (C->IType[Ino] == EXT_FT_DIR && Ino != EXT_ROOT_INO && C->Found[Ino]) Subdirs[C->Found[Ino]]++;
    }
    for (Ino = 1; Ino <= C->Inodes; Ino++) {
        EXT_INODE I;
        UCHAR Raw[1024];
        ULONG Want;
        if (!C->IType[Ino] || (Ino < C->FirstIno && Ino != EXT_ROOT_INO)) continue;
        if (SuperInode(C, Ino)) continue;
        if (C->IType[Ino] != EXT_FT_DIR && C->Links[Ino] == 0) {
            if (ReadInodeRaw(C, Ino, Raw) != 0) continue;
            RawToInode(C, Raw, &I);
            if (ExtInodeSize(&C->Vol, &I) == 0 && ExtInodeBlocks(&C->Vol, &I) == 0 && !(I.FileAclLo | I.FileAclHi)) {
                if (Problem(C, "inode %lu is empty and has no name: cleared", (unsigned long)Ino)) ClearInode(C, Ino, &I);
                continue;
            }
            if (Problem(C, "inode %lu has no name: put in lost+found", (unsigned long)Ino)) Reconnect(C, Ino);
        }
        if (ReadInodeRaw(C, Ino, Raw) != 0) continue;
        RawToInode(C, Raw, &I);
        if (C->IType[Ino] == EXT_FT_DIR) {
            Want = 2 + Subdirs[Ino];
            if (Want >= LINK_MAX && C->Vol.Super->FeatureRoCompat & EXT_RO_DIR_NLINK && I.LinksCount == 1) continue;
            if (Want >= LINK_MAX && (C->Vol.Super->FeatureRoCompat & EXT_RO_DIR_NLINK)) Want = 1;
        } else {
            Want = C->Links[Ino];
            if (Want == 0) continue;
        }
        if (I.LinksCount != Want) {
            if (Problem(C, "inode %lu: link count %u, counted %lu", (unsigned long)Ino, I.LinksCount, (unsigned long)Want)) {
                I.LinksCount = (USHORT)(Want > 65535 ? 65535 : Want);
                WriteInode(C, Ino, &I);
            }
        }
    }
    free(Subdirs);
}

/* ------------------------------------------------------------------ */
/* The superblock                                                      */
/* ------------------------------------------------------------------ */

static int
Pass0(CHK *C)
{
    PEXT_SUPERBLOCK S = C->Vol.Super;
    ULONG G;
    if (S->RevLevel > 1) { printf("  unknown revision %lu\n", (unsigned long)S->RevLevel); return -1; }
    if (S->InodesCount != C->Vol.InodesPerGroup * C->Vol.GroupCount) {
        printf("  the inode count does not match the groups\n");
        return -1;
    }
    for (G = 0; G < C->Groups; G++) {
        ULONGLONG Bb = ExtGdBlockBitmap(&C->Vol, G), Ib = ExtGdInodeBitmap(&C->Vol, G), It = ExtGdInodeTable(&C->Vol, G);
        if (Bb >= C->Blocks || Ib >= C->Blocks || It + C->Vol.ItbPerGroup > C->Blocks || Bb <= C->Vol.FirstDataBlock) {
            printf("  group %lu: its bitmaps or inode table lie outside the volume\n", (unsigned long)G);
            return -1;
        }
    }
    if (S->State & EXT_ERROR_FS) Problem(C, "the volume says it has errors");
    return 0;
}

/* ------------------------------------------------------------------ */
/* The run                                                             */
/* ------------------------------------------------------------------ */

static int
OneRun(CHK *C)
{
    size_t Bits = (size_t)((C->Blocks + 7) / 8);
    C->Problems = C->Repairs = 0;
    C->Restart = FALSE;
    C->DupCount = 0;
    C->DirCount = C->FileCount = 0;
    C->LostFound = 0;
    C->XRefCount = 0;
    memset(C->Claimed, 0, Bits);
    if (C->Dup) { free(C->Dup); C->Dup = NULL; }
    memset(C->IType, 0, C->Inodes + 1);
    memset(C->Links, 0, (C->Inodes + 1) * sizeof(USHORT));
    memset(C->Parent, 0, (C->Inodes + 1) * sizeof(ULONG));
    memset(C->Found, 0, (C->Inodes + 1) * sizeof(ULONG));

    BuildMeta(C);
    if (Pass0(C) != 0) return -1;
    Pass1(C);

    /* xattr block reference counts */
    {
        ULONG i;
        for (i = 0; i < C->XRefCount; i++) {
            PEXT_XATTR_HEADER H = (PEXT_XATTR_HEADER)C->Buf;
            if (ReadBlock(C, C->XRefs[i].Block, C->Buf) != 0) continue;
            if (H->Refcount != C->XRefs[i].Refs &&
                Problem(C, "extended attribute block %llu: %lu references, counted %lu",
                        (unsigned long long)C->XRefs[i].Block, (unsigned long)H->Refcount,
                        (unsigned long)C->XRefs[i].Refs)) {
                H->Refcount = C->XRefs[i].Refs;
                ExtXattrBlockCsum(&C->Vol, C->XRefs[i].Block, C->Buf);
                WriteBlock(C, C->XRefs[i].Block, C->Buf);
            }
        }
    }
    if (C->DupCount) {
        Problem(C, "%llu blocks are claimed by more than one inode", (unsigned long long)C->DupCount);
    } else {
        ULONG i, j;
        for (i = j = 0; i < C->PendCount; i++) {
            if (C->Pend[i].Kind == 2) ListFree(&C->Pend[i].List);
            else C->Pend[j++] = C->Pend[i];
        }
        C->PendCount = j;
    }

    if (C->Fix && C->PendCount) {
        /* allocation must see what pass 1 found in use */
        Bitmaps(C, FALSE);
        DoPending(C);
        return 1;
    }
    if (C->Fix && C->Restart) return 1;

    if (C->Fix) Bitmaps(C, FALSE);      /* lost+found and holes may need blocks */
    Pass2(C);
    if (C->Fix && C->Restart) return 1;
    Pass3(C);
    Pass4(C);
    if (C->Fix && C->Restart) return 1;
    Bitmaps(C, TRUE);
    return 0;
}

int
ExtCheckVolume(PEXT_IMAGE Img, ULONG Flags)
{
    CHK *C = (CHK *)calloc(1, sizeof(CHK));
    EXT_STATUS St;
    int Run, Rc, Result = EXT_CHECK_CLEAN;
    ULONG Total = 0, Fixed = 0;

    if (!C) return EXT_CHECK_FAILED;
    C->Img = Img;
    C->Flags = Flags;
    C->Fix = (BOOLEAN)((Flags & EXT_CHECK_FIX) != 0);
    C->Vol.HostContext = Img;

    St = ExtMount(&C->Vol, (BOOLEAN)!C->Fix);
    if (!NT_SUCCESS(St)) {
        printf("This is not an ext2/ext3/ext4 volume EXTNT can check (%lX).\n", (unsigned long)St);
        free(C);
        return EXT_CHECK_FAILED;
    }
    if (C->Vol.OldExt) {
        printf("This is the original (1992) ext file system, which EXTNT mounts read-only;\n"
               "there is no checker for it.\n");
        ExtUnmountFree(&C->Vol);
        free(C);
        return EXT_CHECK_FAILED;
    }
    if (C->Fix && C->Vol.ReadOnly) {
        printf("The volume has features EXTNT cannot write: checking only.\n");
        C->Fix = FALSE;
    }
    if (C->Vol.NeedsRecovery) {
        if (C->Fix) {
            printf("Replaying the journal...\n");
            St = ExtJournalRecover(&C->Vol);
            if (!NT_SUCCESS(St)) {
                printf("The journal could not be replayed (%lX).\n", (unsigned long)St);
                ExtUnmountFree(&C->Vol);
                free(C);
                return EXT_CHECK_FAILED;
            }
        } else {
            printf("The journal has not been replayed: the check may report what it would fix.\n");
        }
    }
    if (C->Vol.Super->LastOrphan) {
        if (C->Fix) {
            printf("Processing the orphan list...\n");
            ExtProcessOrphans(&C->Vol);
            ExtFlushSuper(&C->Vol);
        }
    }
    if (C->Fix) ExtHostFlush(&C->Vol);

    C->Bs = C->Vol.BlockSize;
    C->Spb = C->Vol.SectorsPerBlock;
    C->Isize = C->Vol.InodeSize;
    C->Blocks = C->Vol.BlocksCount;
    C->Inodes = C->Vol.InodesCount;
    C->Ipg = C->Vol.InodesPerGroup;
    C->Groups = C->Vol.GroupCount;
    C->FirstIno = C->Vol.FirstIno ? C->Vol.FirstIno : 11;
    C->Claimed = (PUCHAR)calloc(1, (size_t)((C->Blocks + 7) / 8));
    C->Meta = (PUCHAR)calloc(1, (size_t)((C->Blocks + 7) / 8));
    C->IType = (PUCHAR)calloc(C->Inodes + 1, 1);
    C->State = (PUCHAR)calloc((C->Inodes + 8) / 8, 1);
    C->Links = (USHORT *)calloc(C->Inodes + 1, sizeof(USHORT));
    C->Parent = (ULONG *)calloc(C->Inodes + 1, sizeof(ULONG));
    C->Found = (ULONG *)calloc(C->Inodes + 1, sizeof(ULONG));
    C->Buf = (PUCHAR)malloc(C->Bs);
    C->Buf2 = (PUCHAR)malloc(C->Bs);
    C->ItBuf = (PUCHAR)malloc((size_t)C->Bs * 64);
    if (!C->Claimed || !C->Meta || !C->IType || !C->State || !C->Links || !C->Parent || !C->Found ||
        !C->Buf || !C->Buf2 || !C->ItBuf) {
        printf("Not enough memory to check this volume.\n");
        Result = EXT_CHECK_FAILED;
        goto Done;
    }

    for (Run = 0; Run < 8; Run++) {
        Rc = OneRun(C);
        Total += C->Problems;
        Fixed += C->Repairs;
        if (Rc < 0) { Result = EXT_CHECK_FAILED; break; }
        if (Rc == 0) break;
        if (!(Flags & EXT_CHECK_QUIET)) printf("  (restarting the check after repairs)\n");
    }

    if (Result != EXT_CHECK_FAILED) {
        if (C->Fix) {
            /* the last run must have found nothing left */
            if (C->Problems == 0 || C->Problems == C->Repairs) {
                C->Vol.Super->State = (USHORT)((C->Vol.Super->State | EXT_VALID_FS) & ~EXT_ERROR_FS);
                C->Vol.Super->LastCheck = ExtHostNow();
                C->Vol.Super->MountCount = 0;
                C->Vol.Super->ErrorCount = 0;
                ExtFlushSuper(&C->Vol);
                ExtHostFlush(&C->Vol);
            }
            Result = Total ? EXT_CHECK_FIXED : EXT_CHECK_CLEAN;
            if (C->Problems > C->Repairs) Result = EXT_CHECK_ERRORS;
        } else {
            Result = Total ? EXT_CHECK_ERRORS : EXT_CHECK_CLEAN;
        }
        printf("%lu files, %lu directories, %llu/%llu blocks free; %lu problem(s)%s.\n",
               (unsigned long)C->FileCount, (unsigned long)C->DirCount,
               (unsigned long long)C->Vol.FreeBlocksCount, (unsigned long long)C->Blocks,
               (unsigned long)Total, C->Fix ? (Total ? " fixed" : "") : (Total ? " found" : ""));
    }

Done:
    ExtUnmountFree(&C->Vol);
    free(C->Claimed); free(C->Meta); free(C->Dup); free(C->IType); free(C->State); free(C->Links);
    free(C->Parent); free(C->Found); free(C->Buf); free(C->Buf2); free(C->ItBuf); free(C->Pend); free(C->XRefs);
    free(C);
    return Result;
}
