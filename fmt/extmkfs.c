/*
 * EXTNT - making an ext2/ext3/ext4 file system, as mke2fs does
 *
 * The static layout - superblock, group descriptors, bitmaps, inode tables
 * - is computed and written here. Then the shared engine mounts the new
 * volume and makes the root directory, lost+found and the journal with its
 * own code, so they are exactly what the driver itself would write.
 * Last, the superblock and group descriptor backups are copied from the
 * primary ones.
 *
 * Defaults follow mke2fs.conf:
 *   floppy (< 3 MB):   1 KB blocks, 128-byte inodes, one inode per 8 KB
 *   small (< 512 MB):  1 KB blocks, 256-byte inodes, one inode per 4 KB
 *   otherwise:         4 KB blocks, 256-byte inodes, one inode per 16 KB
 *   ext2:  ext_attr dir_index filetype sparse_super large_file
 *   ext3:  ext2 + has_journal
 *   ext4:  has_journal ext_attr dir_index filetype extent 64bit flex_bg
 *          sparse_super large_file huge_file dir_nlink extra_isize
 *          metadata_csum
 * The resize inode (online growing in Linux) is not made.
 */

#include "extusr.h"
#include "extsup.h"
#include "extimg.h"
#include "extmkfs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

#define MAX_FLEX_SPILL  3           /* groups a flex group's metadata may run into */

typedef struct _LAYOUT {
    ULONG       Bs, Log;
    ULONGLONG   Blocks;
    ULONG       Fdb;                /* first data block */
    ULONG       Bpg, Ipg, Itb;
    ULONG       Cb;                 /* bigalloc: log2(blocks per cluster), else 0 */
    ULONG       SbBlk;              /* block of the superblock: 1 for 1 KB blocks, else 0 */
    ULONG       Groups;
    ULONG       DescSize, GdtBlocks;
    ULONG       Isize;
    ULONG       Flex;               /* groups per flex group, 1 = none */
    ULONG       Compat, Incompat, RoCompat;
    BOOLEAN     Csum, GdtCsum, Sparse;
    ULONGLONG  *Bb, *Ib, *It;       /* per group: block bitmap, inode bitmap, inode table */
    ULONGLONG   BootStart;          /* boot area (inode 5): data blocks, then its map blocks */
    ULONG       BootLen, BootMeta;
} LAYOUT;

static void
Fill(PUCHAR Buf, ULONG Len)
{
#ifdef _WIN32
    static ULONG Mix;
    LARGE_INTEGER C;
    ULONG i;
    for (i = 0; i < Len; i++) {
        QueryPerformanceCounter(&C);
        Mix = ExtCrc32c(Mix ^ C.LowPart ^ GetTickCount() ^ (ULONG)(ULONG_PTR)&C, &C, sizeof(C)) + i * 2654435761u;
        Buf[i] = (UCHAR)(Mix >> 13);
    }
#else
    int Fd = open("/dev/urandom", O_RDONLY);
    if (Fd < 0 || read(Fd, Buf, Len) != (ssize_t)Len) {
        ULONG i, X = (ULONG)time(NULL) ^ (ULONG)getpid();
        for (i = 0; i < Len; i++) { X = X * 1103515245 + 12345; Buf[i] = (UCHAR)(X >> 16); }
    }
    if (Fd >= 0) close(Fd);
#endif
}

static int
IsPowerOf(ULONG G, ULONG N)
{
    while (G > 1 && G % N == 0) G /= N;
    return G == 1;
}

static int
HasSuper(LAYOUT *L, ULONG G)
{
    if (G <= 1 || !L->Sparse) return 1;
    if (!(G & 1)) return 0;
    return IsPowerOf(G, 3) || IsPowerOf(G, 5) || IsPowerOf(G, 7);
}

static ULONGLONG GroupStart(LAYOUT *L, ULONG G) { return L->Fdb + (ULONGLONG)G * L->Bpg; }

static ULONG
GroupLen(LAYOUT *L, ULONG G)
{
    ULONGLONG S = GroupStart(L, G);
    return (ULONG)((S + L->Bpg <= L->Blocks) ? L->Bpg : L->Blocks - S);
}

/* Blocks the superblock and descriptor table (or its backup) take at a group's start
   (group 0 of a bigalloc volume with 1 KB blocks also starts with the boot block) */
static ULONG
SuperBlocks(LAYOUT *L, ULONG G)
{
    if (G == 0) return L->SbBlk + 1 + L->GdtBlocks - L->Fdb;
    return HasSuper(L, G) ? 1 + L->GdtBlocks : 0;
}

static void SetBit(PUCHAR B, ULONG N);

/* bigalloc: a bitmap bit is a cluster */
static void SetBlockBit(LAYOUT *L, PUCHAR B, ULONG Rel) { SetBit(B, Rel >> L->Cb); }
static ULONG GroupClusters(LAYOUT *L, ULONG G);

/* Next run of N blocks at or after *Cursor that does not cross a superblock area */
static ULONGLONG
Take(LAYOUT *L, ULONGLONG *Cursor, ULONG N)
{
    ULONGLONG A = *Cursor;
    for (;;) {
        ULONG G0 = (ULONG)((A - L->Fdb) / L->Bpg), G1 = (ULONG)((A + N - 1 - L->Fdb) / L->Bpg), G;
        int Moved = 0;
        if (A + N > L->Blocks) return 0;
        for (G = G0; G <= G1 && G < L->Groups; G++) {
            ULONGLONG S = GroupStart(L, G), E = S + SuperBlocks(L, G);
            if (A < E && A + N > S) { A = E; Moved = 1; break; }
        }
        if (!Moved) break;
    }
    *Cursor = A + N;
    return A;
}

static void
SetBit(PUCHAR B, ULONG N)
{
    B[N >> 3] |= (UCHAR)(1 << (N & 7));
}

static ULONG
GroupClusters(LAYOUT *L, ULONG G)
{
    return (GroupLen(L, G) + (1u << L->Cb) - 1) >> L->Cb;
}

static void
PutLe16(PUCHAR P, ULONG V) { P[0] = (UCHAR)V; P[1] = (UCHAR)(V >> 8); }
static void
PutLe32(PUCHAR P, ULONG V) { P[0] = (UCHAR)V; P[1] = (UCHAR)(V >> 8); P[2] = (UCHAR)(V >> 16); P[3] = (UCHAR)(V >> 24); }
static void
PutBe32(PUCHAR P, ULONG V) { P[0] = (UCHAR)(V >> 24); P[1] = (UCHAR)(V >> 16); P[2] = (UCHAR)(V >> 8); P[3] = (UCHAR)V; }

static int
WriteBlocks(PEXT_IMAGE Img, LAYOUT *L, ULONGLONG Block, ULONG Count, const void *Buf)
{
    ULONG Spb = L->Bs / 512;
    return ImgWrite(Img, Block * Spb, Count * Spb, Buf);
}

/* Zeroes Count blocks from Block, a megabyte at a time */
static int
ZeroBlocks(PEXT_IMAGE Img, LAYOUT *L, ULONGLONG Block, ULONGLONG Count)
{
    static PUCHAR Z;
    ULONG Chunk = (1024 * 1024) / L->Bs;
    if (Z == NULL) Z = (PUCHAR)calloc(1, 1024 * 1024);
    if (Z == NULL) return -1;
    while (Count) {
        ULONG N = Count > Chunk ? Chunk : (ULONG)Count;
        if (WriteBlocks(Img, L, Block, N, Z) != 0) return -1;
        Block += N;
        Count -= N;
    }
    return 0;
}

void
ExtMkfsDefaults(PEXT_MKFS P, ULONGLONG Bytes)
{
    if (P->Type == 0) P->Type = 4;
    if (P->Revision0) P->Type = 2;
    if (Bytes < 3 * 1024 * 1024) {
        if (!P->BlockSize) P->BlockSize = 1024;
        if (!P->InodeSize) P->InodeSize = 128;
        if (!P->InodeRatio) P->InodeRatio = 8192;
    } else if (Bytes < 512ULL * 1024 * 1024) {
        if (!P->BlockSize) P->BlockSize = 1024;
        if (!P->InodeRatio) P->InodeRatio = 4096;
    } else {
        if (!P->BlockSize) P->BlockSize = 4096;
        if (!P->InodeRatio) P->InodeRatio = 16384;
    }
    if (!P->InodeSize) P->InodeSize = P->Revision0 ? 128 : 256;
    if (P->Revision0) P->InodeSize = 128;
    if (P->ReservedPercent == EXT_MKFS_DEFAULT) P->ReservedPercent = 5;
    if (P->ReservedPercent > 50) P->ReservedPercent = 50;
    if (!P->FlexGroups) P->FlexGroups = 16;
}

/* mke2fs's default journal size, in blocks */
static ULONG
JournalSize(ULONGLONG Blocks)
{
    if (Blocks < 2048) return 0;
    if (Blocks < 32768) return 1024;
    if (Blocks < 256 * 1024) return 4096;
    if (Blocks < 512 * 1024) return 8192;
    if (Blocks < 4096 * 1024) return 16384;
    if (Blocks < 8192 * 1024) return 32768;
    if (Blocks < 16384 * 1024) return 65536;
    if (Blocks < 32768 * 1024) return 131072;
    return 262144;
}

static int
Err(char *Error, unsigned Size, const char *Msg)
{
    snprintf(Error, Size, "%s", Msg);
    return -1;
}

int
ExtMakeFs(PEXT_IMAGE Img, PEXT_MKFS P, char *Error, unsigned ErrorSize)
{
    LAYOUT L;
    ULONGLONG Bytes = Img->Sectors * 512, Cursor, Reserved;
    ULONG G, i, Ipb, Pass, JBlocks;
    PUCHAR Sb = NULL, Gdt = NULL, Bitmap = NULL;
    PEXT_SUPERBLOCK S;
    ULONG Now = ExtHostNow();
    ULONGLONG FreeBlocks = 0;
    ULONG FreeInodes = 0;
    EXT_VOL Vol;
    EXT_STATUS St;
    int Rc = -1;

    memset(&L, 0, sizeof(L));
    ExtMkfsDefaults(P, Bytes);

    /* features */
    if (P->Revision0) {
        L.Compat = L.Incompat = L.RoCompat = 0;
    } else {
        L.Compat = EXT_FC_EXT_ATTR | EXT_FC_DIR_INDEX;
        L.Incompat = EXT_FI_FILETYPE;
        L.RoCompat = EXT_RO_SPARSE_SUPER | EXT_RO_LARGE_FILE;
        if (P->Type >= 3) L.Compat |= EXT_FC_HAS_JOURNAL;
        if (P->Type == 4) {
            L.Incompat |= EXT_FI_EXTENTS | EXT_FI_64BIT | EXT_FI_FLEX_BG;
            L.RoCompat |= EXT_RO_HUGE_FILE | EXT_RO_DIR_NLINK | EXT_RO_EXTRA_ISIZE | EXT_RO_METADATA_CSUM;
        }
        L.Compat = (L.Compat | P->FeaturesOn[0]) & ~P->FeaturesOff[0];
        L.Incompat = (L.Incompat | P->FeaturesOn[1]) & ~P->FeaturesOff[1];
        L.RoCompat = (L.RoCompat | P->FeaturesOn[2]) & ~P->FeaturesOff[2];
        /* what this formatter can make */
        if ((L.Compat & ~(ULONG)(EXT_FC_EXT_ATTR | EXT_FC_DIR_INDEX | EXT_FC_HAS_JOURNAL)) ||
            (L.Incompat & ~(ULONG)(EXT_FI_FILETYPE | EXT_FI_EXTENTS | EXT_FI_64BIT | EXT_FI_FLEX_BG)) ||
            (L.RoCompat & ~(ULONG)(EXT_RO_SPARSE_SUPER | EXT_RO_LARGE_FILE | EXT_RO_HUGE_FILE |
                                   EXT_RO_DIR_NLINK | EXT_RO_EXTRA_ISIZE | EXT_RO_METADATA_CSUM |
                                   EXT_RO_GDT_CSUM | EXT_RO_BIGALLOC))) {
            return Err(Error, ErrorSize, "a feature asked for cannot be made by this formatter");
        }
        if ((L.RoCompat & EXT_RO_METADATA_CSUM) && (L.RoCompat & EXT_RO_GDT_CSUM)) {
            L.RoCompat &= ~(ULONG)EXT_RO_GDT_CSUM;      /* metadata_csum supersedes it */
        }
        if ((L.RoCompat & EXT_RO_EXTRA_ISIZE) && P->InodeSize <= 128) {
            L.RoCompat &= ~(ULONG)EXT_RO_EXTRA_ISIZE;
        }
    }
    L.Csum = (BOOLEAN)((L.RoCompat & EXT_RO_METADATA_CSUM) != 0);
    L.GdtCsum = (BOOLEAN)((L.RoCompat & EXT_RO_GDT_CSUM) != 0);
    L.Sparse = (BOOLEAN)((L.RoCompat & EXT_RO_SPARSE_SUPER) != 0);
    L.Flex = (L.Incompat & EXT_FI_FLEX_BG) ? P->FlexGroups : 1;
    if (L.Flex & (L.Flex - 1)) return Err(Error, ErrorSize, "flex group size must be a power of two");

    /* geometry */
    L.Bs = P->BlockSize;
    if (L.Bs != 1024 && L.Bs != 2048 && L.Bs != 4096 && L.Bs != 8192 && L.Bs != 16384 &&
        L.Bs != 32768 && L.Bs != 65536) {
        return Err(Error, ErrorSize, "block size must be a power of two from 1024 to 65536");
    }
    for (L.Log = 0; (1024u << L.Log) < L.Bs; L.Log++) ;
    L.Cb = 0;
    if (P->ClusterSize && P->ClusterSize != L.Bs) {
        /* bigalloc: allocation in clusters of 2^Cb blocks (up to 1 GB, as Linux allows) */
        if (P->ClusterSize < L.Bs || (P->ClusterSize & (P->ClusterSize - 1)) || P->ClusterSize > (1u << 30)) {
            return Err(Error, ErrorSize, "cluster size must be a power of two from twice the block size to 1 GB");
        }
        if (!(L.Incompat & EXT_FI_EXTENTS)) return Err(Error, ErrorSize, "clusters (bigalloc) need ext4 (extents)");
        if (P->Boot) return Err(Error, ErrorSize, "a volume with clusters cannot start Windows NT");
        while ((L.Bs << L.Cb) < P->ClusterSize) L.Cb++;
        L.RoCompat |= EXT_RO_BIGALLOC;
    }
    L.SbBlk = (L.Bs == 1024) ? 1 : 0;
    L.Isize = P->InodeSize;
    if (L.Isize < 128 || L.Isize > L.Bs || (L.Isize & (L.Isize - 1))) {
        return Err(Error, ErrorSize, "inode size must be a power of two from 128 to the block size");
    }
    L.Blocks = Bytes / L.Bs;
    if (!(L.Incompat & EXT_FI_64BIT) && L.Blocks > 0xFFFFFFFFULL) L.Blocks = 0xFFFFFFFFULL;
    L.Fdb = (L.Bs == 1024 && !L.Cb) ? 1 : 0;       /* bigalloc: always 0 */
    if (L.Cb) {
        /* clusters per group: a bitmap block's bits, at most 65528; a group
           of blocks must stay below 2^32 */
        ULONG Cpg = 8 * L.Bs;
        if (Cpg > 65528) Cpg = 65528;
        while (((ULONGLONG)Cpg << L.Cb) > 0x80000000ULL) Cpg /= 2;
        Cpg &= ~7u;
        if (Cpg < 8) return Err(Error, ErrorSize, "the cluster size is too large");
        L.Bpg = Cpg << L.Cb;
        L.Blocks &= ~(((ULONGLONG)1 << L.Cb) - 1);   /* whole clusters */
    } else {
        L.Bpg = P->BlocksPerGroup ? P->BlocksPerGroup : 8 * L.Bs;
        if (L.Bpg > 65528) L.Bpg = 65528;           /* EXT2_MAX_BLOCKS_PER_GROUP */
        if (L.Bpg > 8 * L.Bs || (L.Bpg & 7) || L.Bpg < 256) return Err(Error, ErrorSize, "bad blocks per group");
    }
    if (L.Blocks < 64 || (L.Blocks >> L.Cb) < 16) return Err(Error, ErrorSize, "the volume is too small");
    L.DescSize = (L.Incompat & EXT_FI_64BIT) ? 64 : 32;
    Ipb = L.Bs / L.Isize;

    for (Pass = 0; Pass < 4; Pass++) {
        ULONGLONG Want, Last;
        ULONG Overhead;
        L.Groups = (ULONG)((L.Blocks - L.Fdb + L.Bpg - 1) / L.Bpg);
        L.GdtBlocks = (L.Groups * L.DescSize + L.Bs - 1) / L.Bs;
        Want = P->Inodes ? P->Inodes : (L.Blocks * L.Bs) / P->InodeRatio;
        if (Want < 16) Want = 16;
        L.Ipg = (ULONG)((Want + L.Groups - 1) / L.Groups);
        L.Ipg = ((L.Ipg + Ipb - 1) / Ipb) * Ipb;
        L.Ipg = (L.Ipg + 7) & ~7u;
        if (L.Ipg > 8 * L.Bs) L.Ipg = 8 * L.Bs - (8 * L.Bs) % Ipb;
        if (L.Ipg > 65536 - Ipb) L.Ipg = (65536 - Ipb) - (65536 - Ipb) % (Ipb > 8 ? Ipb : 8);
        if ((ULONGLONG)L.Ipg * L.Groups > 0xFFFFFFFFULL) L.Ipg = (ULONG)(0xFFFFFFFFULL / L.Groups) & ~7u;
        L.Itb = L.Ipg * L.Isize / L.Bs;
        /* a last group too small to hold its own metadata goes */
        Last = L.Blocks - GroupStart(&L, L.Groups - 1);
        Overhead = SuperBlocks(&L, L.Groups - 1) + (L.Flex == 1 ? 2 + L.Itb : 0);
        if (L.Groups > 1 && Last < Overhead + 50 + (2u << L.Cb)) {
            L.Blocks -= Last;
            continue;
        }
        break;
    }
    if (L.Groups == 1 && L.Blocks - L.Fdb < SuperBlocks(&L, 0) + 2 + L.Itb + 16) {
        return Err(Error, ErrorSize, "the volume is too small");
    }
    if (L.Flex > 1 && L.Itb * L.Flex + 2 * L.Flex > (MAX_FLEX_SPILL - 1) * L.Bpg) {
        L.Flex = 1;
        L.Incompat &= ~(ULONG)EXT_FI_FLEX_BG;
    }

    /* placement */
    L.Bb = (ULONGLONG *)calloc(L.Groups, sizeof(ULONGLONG));
    L.Ib = (ULONGLONG *)calloc(L.Groups, sizeof(ULONGLONG));
    L.It = (ULONGLONG *)calloc(L.Groups, sizeof(ULONGLONG));
    Sb = (PUCHAR)calloc(1, 1024);
    Gdt = (PUCHAR)calloc(L.GdtBlocks, L.Bs);
    Bitmap = (PUCHAR)malloc(L.Bs);
    if (!L.Bb || !L.Ib || !L.It || !Sb || !Gdt || !Bitmap) { Err(Error, ErrorSize, "out of memory"); goto Done; }

    if (P->Boot) {
        ULONG Spb = L.Bs / 512, Per = L.Bs / 4;
        L.BootLen = (EXT_BOOT_FAT_SECTORS + Spb - 1) / Spb + (EXT_BOOT_DIR_BYTES + L.Bs - 1) / L.Bs;
        L.BootMeta = 0;
        if (!(L.Incompat & EXT_FI_EXTENTS) && L.BootLen > 12) {
            L.BootMeta = 1;
            if (L.BootLen > 12 + Per) L.BootMeta += 1 + (L.BootLen - 12 - Per + Per - 1) / Per;
        }
    }
    for (G = 0; G < L.Groups; G += L.Flex) {
        ULONG N = (L.Groups - G < L.Flex) ? L.Groups - G : L.Flex, k;
        Cursor = GroupStart(&L, G) + SuperBlocks(&L, G);
        if (G == 0 && P->Boot) {
            /* low on the disk: the FAT must start within the first 32 MB */
            L.BootStart = Take(&L, &Cursor, L.BootLen + L.BootMeta);
            if (!L.BootStart || L.BootStart * (L.Bs / 512) > 65535 - 1) {
                Err(Error, ErrorSize, "no room for the boot area"); goto Done;
            }
        }
        for (k = 0; k < N; k++) L.Bb[G + k] = Take(&L, &Cursor, 1);
        for (k = 0; k < N; k++) L.Ib[G + k] = Take(&L, &Cursor, 1);
        for (k = 0; k < N; k++) L.It[G + k] = Take(&L, &Cursor, L.Itb);
        for (k = 0; k < N; k++) {
            if (!L.Bb[G + k] || !L.Ib[G + k] || !L.It[G + k]) { Err(Error, ErrorSize, "the volume is too small for its metadata"); goto Done; }
            if (L.It[G + k] + L.Itb > GroupStart(&L, G) + (ULONGLONG)MAX_FLEX_SPILL * L.Bpg) {
                Err(Error, ErrorSize, "metadata layout does not fit"); goto Done;
            }
        }
    }

    /* superblock */
    S = (PEXT_SUPERBLOCK)Sb;
    S->InodesCount = L.Ipg * L.Groups;
    S->BlocksCountLo = (ULONG)L.Blocks;
    S->BlocksCountHi = (L.Incompat & EXT_FI_64BIT) ? (ULONG)(L.Blocks >> 32) : 0;
    Reserved = L.Blocks * P->ReservedPercent / 100;
    S->RBlocksCountLo = (ULONG)Reserved;
    S->RBlocksCountHi = (L.Incompat & EXT_FI_64BIT) ? (ULONG)(Reserved >> 32) : 0;
    S->FirstDataBlock = L.Fdb;
    S->LogBlockSize = L.Log;
    S->LogClusterSize = L.Log + L.Cb;
    S->BlocksPerGroup = L.Bpg;
    S->ClustersPerGroup = L.Bpg >> L.Cb;
    S->InodesPerGroup = L.Ipg;
    S->WriteTime = Now;
    S->MaxMountCount = 0xFFFF;
    S->Magic = EXT_SUPER_MAGIC;
    S->State = EXT_VALID_FS;
    S->Errors = 1;                      /* continue */
    S->LastCheck = Now;
    S->CreatorOs = 0;                   /* Linux */
    S->RevLevel = P->Revision0 ? 0 : 1;
    if (!P->Revision0) {
        S->FirstIno = 11;
        S->InodeSize = (USHORT)L.Isize;
        S->FeatureCompat = L.Compat & ~(ULONG)EXT_FC_HAS_JOURNAL;   /* the journal comes later */
        S->FeatureIncompat = L.Incompat;
        S->FeatureRoCompat = L.RoCompat;
        S->DefHashVersion = 1;          /* half_md4 */
        S->Flags = 0x0001;              /* signed directory hash, as x86 Linux */
        Fill((PUCHAR)S->HashSeed, 16);
        S->DescSize = (USHORT)((L.Incompat & EXT_FI_64BIT) ? 64 : 0);
        S->DefaultMountOpts = (L.Compat & EXT_FC_EXT_ATTR) ? 0x000C : 0;   /* user_xattr, acl */
        S->MkfsTime = Now;
        if (L.Isize > 128 && (L.RoCompat & EXT_RO_EXTRA_ISIZE)) {
            S->MinExtraIsize = 32;
            S->WantExtraIsize = 32;
        }
        if (L.Flex > 1) {
            for (i = 0; (1u << i) < L.Flex; i++) ;
            S->LogGroupsPerFlex = (UCHAR)i;
        }
        if (L.Csum) S->ChecksumType = 1;
    }
    memcpy(S->Uuid, P->Uuid, 16);
    for (i = 0; i < 16 && !S->Uuid[i]; i++) ;
    if (i == 16) {
        Fill(S->Uuid, 16);
        S->Uuid[6] = (UCHAR)((S->Uuid[6] & 0x0F) | 0x40);
        S->Uuid[8] = (UCHAR)((S->Uuid[8] & 0x3F) | 0x80);
    }
    memcpy(S->VolumeName, P->Label, strlen(P->Label) > 16 ? 16 : strlen(P->Label));

    /* group descriptors and bitmaps; inode tables zeroed */
    {
        ULONG Seed = ExtCrc32c(0xFFFFFFFF, S->Uuid, 16);
        for (G = 0; G < L.Groups; G++) {
            PUCHAR D = Gdt + G * L.DescSize;
            ULONGLONG Start = GroupStart(&L, G), End = Start + GroupLen(&L, G);
            ULONG Used = 0, FreeI = L.Ipg, H, H0, H1, Flags = 0, Unused = L.Ipg;
            UCHAR Gle[4];

            /* block bitmap */
            memset(Bitmap, 0, L.Bs);
            for (i = 0; i < SuperBlocks(&L, G); i++) SetBlockBit(&L, Bitmap, i);
            H0 = (G / L.Flex >= MAX_FLEX_SPILL) ? (G / L.Flex - (MAX_FLEX_SPILL - 1)) * L.Flex : 0;
            H1 = (G / L.Flex + 1) * L.Flex;
            if (H1 > L.Groups) H1 = L.Groups;
            if (L.BootLen) {
                ULONGLONG B;
                for (B = L.BootStart; B < L.BootStart + L.BootLen + L.BootMeta; B++)
                    if (B >= Start && B < End) SetBlockBit(&L, Bitmap, (ULONG)(B - Start));
            }
            for (H = H0; H < H1; H++) {
                ULONGLONG B;
                if (L.Bb[H] >= Start && L.Bb[H] < End) SetBlockBit(&L, Bitmap, (ULONG)(L.Bb[H] - Start));
                if (L.Ib[H] >= Start && L.Ib[H] < End) SetBlockBit(&L, Bitmap, (ULONG)(L.Ib[H] - Start));
                for (B = L.It[H]; B < L.It[H] + L.Itb; B++)
                    if (B >= Start && B < End) SetBlockBit(&L, Bitmap, (ULONG)(B - Start));
            }
            for (i = 0; i < GroupClusters(&L, G); i++) if (Bitmap[i >> 3] & (1 << (i & 7))) Used++;
            for (i = GroupClusters(&L, G); i < 8 * L.Bs; i++) SetBit(Bitmap, i);
            if (WriteBlocks(Img, &L, L.Bb[G], 1, Bitmap) != 0) { Err(Error, ErrorSize, "write error (block bitmap)"); goto Done; }
            if (L.Csum) {
                ULONG C = ExtCrc32c(Seed, Bitmap, (L.Bpg >> L.Cb) / 8);
                PutLe16(D + 0x18, C);
                if (L.DescSize >= 64) PutLe16(D + 0x38, C >> 16);
            }

            /* inode bitmap */
            memset(Bitmap, 0, L.Bs);
            if (G == 0) {
                for (i = 0; i < 10; i++) SetBit(Bitmap, i);     /* inodes 1 - 10 */
                FreeI -= 10;
                Unused -= 10;
            }
            for (i = L.Ipg; i < 8 * L.Bs; i++) SetBit(Bitmap, i);
            if (WriteBlocks(Img, &L, L.Ib[G], 1, Bitmap) != 0) { Err(Error, ErrorSize, "write error (inode bitmap)"); goto Done; }
            if (L.Csum) {
                ULONG C = ExtCrc32c(Seed, Bitmap, L.Ipg / 8);
                PutLe16(D + 0x1A, C);
                if (L.DescSize >= 64) PutLe16(D + 0x3A, C >> 16);
            }

            /* inode table */
            if (!(P->Lazy && (L.Csum || L.GdtCsum)) || G == 0) {
                if (ZeroBlocks(Img, &L, L.It[G], L.Itb) != 0) { Err(Error, ErrorSize, "write error (inode table)"); goto Done; }
                Flags |= EXT_BG_INODE_ZEROED;
            }

            PutLe32(D + 0x00, (ULONG)L.Bb[G]);
            PutLe32(D + 0x04, (ULONG)L.Ib[G]);
            PutLe32(D + 0x08, (ULONG)L.It[G]);
            PutLe16(D + 0x0C, GroupClusters(&L, G) - Used);         /* clusters */
            PutLe16(D + 0x0E, FreeI);
            PutLe16(D + 0x10, 0);
            if (L.Csum || L.GdtCsum) {
                PutLe16(D + 0x12, Flags);
                PutLe16(D + 0x1C, Unused);
            }
            if (L.DescSize >= 64) {
                PutLe32(D + 0x20, (ULONG)(L.Bb[G] >> 32));
                PutLe32(D + 0x24, (ULONG)(L.Ib[G] >> 32));
                PutLe32(D + 0x28, (ULONG)(L.It[G] >> 32));
                PutLe16(D + 0x2C, (GroupClusters(&L, G) - Used) >> 16);
                PutLe16(D + 0x2E, FreeI >> 16);
                if (L.Csum || L.GdtCsum) PutLe16(D + 0x32, Unused >> 16);
            }
            PutLe32(Gle, G);
            if (L.Csum) {
                ULONG C = ExtCrc32c(Seed, Gle, 4);
                C = ExtCrc32c(C, D, L.DescSize);
                PutLe16(D + 0x1E, C);
            } else if (L.GdtCsum) {
                USHORT C = ExtCrc16(0xFFFF, S->Uuid, 16);
                C = ExtCrc16(C, Gle, 4);
                C = ExtCrc16(C, D, 0x1E);
                if (L.DescSize > 0x20) C = ExtCrc16(C, D + 0x20, L.DescSize - 0x20);
                PutLe16(D + 0x1E, C);
            }
            FreeBlocks += (ULONGLONG)(GroupClusters(&L, G) - Used) << L.Cb;   /* the superblock counts blocks */
            FreeInodes += FreeI;
            if (!P->Quiet && (G % 64 == 63 || G + 1 == L.Groups)) {
                printf("\r  %u of %u groups", (unsigned)(G + 1), (unsigned)L.Groups);
                fflush(stdout);
            }
        }
        if (!P->Quiet) printf("\n");
    }
    S->FreeBlocksCountLo = (ULONG)FreeBlocks;
    S->FreeBlocksCountHi = (L.Incompat & EXT_FI_64BIT) ? (ULONG)(FreeBlocks >> 32) : 0;
    S->FreeInodesCount = FreeInodes;
    if (L.Csum) S->Checksum = ExtCrc32c(0xFFFFFFFF, Sb, 0x3FC);

    /* block 0 (before the superblock) is the boot block: zero it, as mke2fs
       does, but keep a partition table that may be in sector 0 of a whole disk */
    {
        UCHAR Z[1024];
        memset(Z, 0, sizeof(Z));
        if (ImgWrite(Img, 0, 2, Z) != 0) { Err(Error, ErrorSize, "write error"); goto Done; }
    }
    if (ImgWrite(Img, 2, 2, Sb) != 0 ||
        WriteBlocks(Img, &L, L.SbBlk + 1, L.GdtBlocks, Gdt) != 0) {
        Err(Error, ErrorSize, "write error (superblock)"); goto Done;
    }
    if (L.Bs > 1024) {
        /* the rest of block 0 */
        PUCHAR Z = (PUCHAR)calloc(1, L.Bs);
        int R = Z ? ImgWrite(Img, 4, (L.Bs - 2048) / 512, Z) : -1;
        free(Z);
        if (R != 0) { Err(Error, ErrorSize, "write error"); goto Done; }
    }

    /* the engine makes the rest */
    memset(&Vol, 0, sizeof(Vol));
    Vol.HostContext = Img;
    St = ExtMount(&Vol, FALSE);
    if (!NT_SUCCESS(St)) { snprintf(Error, ErrorSize, "the new file system does not mount (%lX)", (unsigned long)St); goto Done; }

    St = ExtMakeRoot(&Vol);
    if (NT_SUCCESS(St)) {
        /* group 0 counts the root directory */
        PEXT_GROUP_DESC D0 = ExtGroupDesc(&Vol, 0);
        D0->UsedDirsCountLo++;
        ExtGroupTouched(&Vol, 0);
    }
    if (NT_SUCCESS(St)) {
        EXT_INODE Root, Lf;
        ULONG LfIno, Want = 16384 / L.Bs, k;
        St = ExtReadInode(&Vol, EXT_ROOT_INO, &Root);
        if (NT_SUCCESS(St))
            St = ExtCreate(&Vol, EXT_ROOT_INO, &Root, "lost+found", 10, (USHORT)(EXT_S_IFDIR | 0700), NULL, &LfIno, &Lf);
        for (k = 1; NT_SUCCESS(St) && k < Want; k++) St = ExtGrowDirectory(&Vol, LfIno, &Lf);
    }

    if (NT_SUCCESS(St) && L.BootLen) {
        /* inode 5, the boot loader inode, owns the boot area */
        EXT_INODE Bi;
        St = ExtNewInodeAt(&Vol, 5, (USHORT)(EXT_S_IFREG | 0600), &Bi);
        if (NT_SUCCESS(St)) {
            if (L.Incompat & EXT_FI_EXTENTS) {
                PEXT_EXTENT_HEADER H = (PEXT_EXTENT_HEADER)Bi.Block;
                PEXT_EXTENT E = (PEXT_EXTENT)(H + 1);
                H->Entries = 1;
                E->Block = 0;
                E->Len = (USHORT)L.BootLen;
                E->StartLo = (ULONG)L.BootStart;
                E->StartHi = (USHORT)(L.BootStart >> 32);
            } else {
                ULONG Per = L.Bs / 4, k, Next = 0;
                PULONG Map = (PULONG)calloc(1, L.Bs), Top = (PULONG)calloc(1, L.Bs);
                ULONGLONG MetaBlk = L.BootStart + L.BootLen;
                if (!Map || !Top) St = EXT_NO_MEMORY;
                for (k = 0; NT_SUCCESS(St) && k < 12 && k < L.BootLen; k++) Bi.Block[k] = (ULONG)(L.BootStart + k);
                if (NT_SUCCESS(St) && L.BootLen > 12) {
                    /* single indirect */
                    memset(Map, 0, L.Bs);
                    for (k = 0; k < Per && 12 + k < L.BootLen; k++) Map[k] = (ULONG)(L.BootStart + 12 + k);
                    Bi.Block[12] = (ULONG)MetaBlk;
                    if (WriteBlocks(Img, &L, MetaBlk++, 1, Map) != 0) St = EXT_IO_ERROR;
                    Next = 12 + Per;
                }
                if (NT_SUCCESS(St) && L.BootLen > 12 + Per) {
                    /* double indirect: its table, then the indirect blocks */
                    ULONG Tab = 0;
                    ULONGLONG TopBlk = MetaBlk++;
                    memset(Top, 0, L.Bs);
                    while (NT_SUCCESS(St) && Next < L.BootLen) {
                        memset(Map, 0, L.Bs);
                        for (k = 0; k < Per && Next < L.BootLen; k++, Next++) Map[k] = (ULONG)(L.BootStart + Next);
                        Top[Tab++] = (ULONG)MetaBlk;
                        if (WriteBlocks(Img, &L, MetaBlk++, 1, Map) != 0) St = EXT_IO_ERROR;
                    }
                    Bi.Block[13] = (ULONG)TopBlk;
                    if (NT_SUCCESS(St) && WriteBlocks(Img, &L, TopBlk, 1, Top) != 0) St = EXT_IO_ERROR;
                }
                free(Map); free(Top);
            }
            ExtInodeSetSize(&Vol, &Bi, (ULONGLONG)L.BootLen * L.Bs);
            ExtInodeSetBlocks(&Vol, &Bi, (ULONGLONG)(L.BootLen + L.BootMeta) * (L.Bs / 512));
            if (NT_SUCCESS(St)) St = ExtWriteInode(&Vol, 5, &Bi);
        }
        if (NT_SUCCESS(St) && ZeroBlocks(Img, &L, L.BootStart, L.BootLen) != 0) St = EXT_IO_ERROR;
    }

    JBlocks = 0;
    if (NT_SUCCESS(St) && (L.Compat & EXT_FC_HAS_JOURNAL)) {
        EXT_INODE J;
        ULONGLONG Blk, Disk, RunStart = 0, RunLen = 0;
        JBlocks = P->JournalBlocks ? P->JournalBlocks : JournalSize(L.Blocks);
        if (JBlocks < 1024) JBlocks = 1024;
        if (JBlocks > L.Blocks / 4) JBlocks = (ULONG)(L.Blocks / 4);
        if (JBlocks < 1024) {
            Vol.Super->FeatureCompat &= ~(ULONG)EXT_FC_HAS_JOURNAL;     /* too small for one */
            JBlocks = 0;
        }
        if (JBlocks) {
            if (!P->Quiet) printf("  journal: %u blocks\n", (unsigned)JBlocks);
            St = ExtNewInodeAt(&Vol, EXT_JOURNAL_INO, (USHORT)(EXT_S_IFREG | 0600), &J);
            for (Blk = 0; NT_SUCCESS(St) && Blk < JBlocks; Blk++) {
                BOOLEAN Fresh;
                St = ExtGetFileBlock(&Vol, EXT_JOURNAL_INO, &J, Blk, &Disk, &Fresh);
                if (!NT_SUCCESS(St)) break;
                if (RunLen && RunStart + RunLen == Disk) { RunLen++; continue; }
                if (RunLen && ZeroBlocks(Img, &L, RunStart, RunLen) != 0) { St = EXT_IO_ERROR; break; }
                RunStart = Disk; RunLen = 1;
            }
            if (NT_SUCCESS(St) && RunLen && ZeroBlocks(Img, &L, RunStart, RunLen) != 0) St = EXT_IO_ERROR;
            if (NT_SUCCESS(St)) {
                /* the journal superblock (big-endian) in its first block */
                PUCHAR Jb = (PUCHAR)calloc(1, L.Bs);
                ULONGLONG First;
                St = ExtGetFileBlock(&Vol, EXT_JOURNAL_INO, &J, 0, &First, NULL);
                if (Jb && NT_SUCCESS(St)) {
                    PutBe32(Jb + 0x00, JBD_MAGIC);
                    PutBe32(Jb + 0x04, JBD_BT_SUPERBLOCK_V2);
                    PutBe32(Jb + 0x0C, L.Bs);
                    PutBe32(Jb + 0x10, JBlocks);
                    PutBe32(Jb + 0x14, 1);              /* first log block */
                    PutBe32(Jb + 0x18, 1);              /* first sequence */
                    memcpy(Jb + 0x30, Vol.Uuid, 16);
                    PutBe32(Jb + 0x40, 1);              /* users */
                    memcpy(Jb + 0x100, Vol.Uuid, 16);
                    if (WriteBlocks(Img, &L, First, 1, Jb) != 0) St = EXT_IO_ERROR;
                } else if (NT_SUCCESS(St)) {
                    St = EXT_NO_MEMORY;
                }
                free(Jb);
            }
            if (NT_SUCCESS(St)) {
                ExtInodeSetSize(&Vol, &J, (ULONGLONG)JBlocks * L.Bs);
                St = ExtWriteInode(&Vol, EXT_JOURNAL_INO, &J);
            }
            if (NT_SUCCESS(St)) {
                Vol.Super->FeatureCompat |= EXT_FC_HAS_JOURNAL;
                Vol.FeatureCompat |= EXT_FC_HAS_JOURNAL;
                Vol.Super->JournalInum = EXT_JOURNAL_INO;
                memcpy(Vol.Super->JnlBlocks, J.Block, sizeof(J.Block));
                Vol.Super->JnlBlocks[15] = J.SizeHi;
                Vol.Super->JnlBlocks[16] = J.SizeLo;
                Vol.Super->JnlBackupType = 1;   /* EXT3_JNL_BACKUP_BLOCKS */
            }
        }
    }
    if (NT_SUCCESS(St)) St = ExtFlushSuper(&Vol);
    if (NT_SUCCESS(St)) St = ExtHostFlush(&Vol);
    ExtUnmountFree(&Vol);
    if (!NT_SUCCESS(St)) { snprintf(Error, ErrorSize, "making the root, lost+found or journal failed (%lX)", (unsigned long)St); goto Done; }

    /* backups: superblock and descriptor table copied from the primary */
    if (ImgRead(Img, 2, 2, Sb) != 0 || ImgRead(Img, (ULONGLONG)(L.SbBlk + 1) * (L.Bs / 512), L.GdtBlocks * (L.Bs / 512), Gdt) != 0) {
        Err(Error, ErrorSize, "read error"); goto Done;
    }
    S = (PEXT_SUPERBLOCK)Sb;
    for (G = 1; G < L.Groups; G++) {
        PUCHAR Blk0;
        if (!HasSuper(&L, G)) continue;
        Blk0 = (PUCHAR)calloc(1, L.Bs);
        if (Blk0 == NULL) { Err(Error, ErrorSize, "out of memory"); goto Done; }
        S->BlockGroupNr = (USHORT)G;
        if (L.Csum) S->Checksum = ExtCrc32c(0xFFFFFFFF, Sb, 0x3FC);
        memcpy(Blk0, Sb, 1024);
        if (WriteBlocks(Img, &L, GroupStart(&L, G), 1, Blk0) != 0 ||
            WriteBlocks(Img, &L, GroupStart(&L, G) + 1, L.GdtBlocks, Gdt) != 0) {
            free(Blk0); Err(Error, ErrorSize, "write error (backups)"); goto Done;
        }
        free(Blk0);
    }

    if (!P->Quiet) {
        printf("  ext%d, %llu blocks of %u bytes, %u inodes of %u bytes, %u groups%s\n",
               (L.Incompat & (EXT_FI_EXTENTS | EXT_FI_FLEX_BG | EXT_FI_64BIT)) || L.Csum ? 4 :
               JBlocks ? 3 : 2, (unsigned long long)L.Blocks, (unsigned)L.Bs,
               (unsigned)(L.Ipg * L.Groups), (unsigned)L.Isize, (unsigned)L.Groups,
               L.Csum ? ", checksummed" : "");
        if (L.Cb) printf("  clusters of %u bytes (bigalloc)\n", (unsigned)(L.Bs << L.Cb));
    }
    Rc = 0;

Done:
    free(L.Bb); free(L.Ib); free(L.It);
    free(Sb); free(Gdt); free(Bitmap);
    return Rc;
}
