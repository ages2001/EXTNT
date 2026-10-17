/*
 * EXTNT - the shared ext2/3/4 engine: checksums, mount, superblock and group
 * descriptors, block and inode bitmaps, inode read/write, block I/O helpers.
 */

#ifdef EXT_KERNEL
#include "ext.h"
#else
#include "extusr.h"
#include "extsup.h"
#endif

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

VOID ExtZero(PVOID P, ULONG N) { PUCHAR p = (PUCHAR)P; while (N--) *p++ = 0; }
VOID ExtCopy(PVOID D, const VOID *S, ULONG N)
{ PUCHAR d = (PUCHAR)D; const UCHAR *s = (const UCHAR *)S; while (N--) *d++ = *s++; }
BOOLEAN ExtMemEq(const VOID *A, const VOID *B, ULONG N)
{ const UCHAR *a=(const UCHAR*)A,*b=(const UCHAR*)B; while(N--){ if(*a++!=*b++) return FALSE; } return TRUE; }

/* ------------------------------------------------------------------ */
/* CRC32C (Castagnoli, reflected) and CRC16 (ext gdt_csum)             */
/* ------------------------------------------------------------------ */

static ULONG ExtCrc32cTable[256];
static USHORT ExtCrc16Table[256];
static BOOLEAN ExtCrcReady = FALSE;

static VOID
ExtCrcInit(VOID)
{
    ULONG i, j, c;
    USHORT d;
    for (i = 0; i < 256; i++) {
        c = i;
        for (j = 0; j < 8; j++) {
            c = (c & 1) ? (c >> 1) ^ 0x82F63B78 : (c >> 1);
        }
        ExtCrc32cTable[i] = c;
        d = (USHORT)i;
        for (j = 0; j < 8; j++) {
            d = (USHORT)((d & 1) ? (d >> 1) ^ 0xA001 : (d >> 1));
        }
        ExtCrc16Table[i] = d;
    }
    ExtCrcReady = TRUE;
}

ULONG
ExtCrc32c(ULONG Crc, const VOID *Data, ULONG Length)
{
    const UCHAR *p = (const UCHAR *)Data;
    if (!ExtCrcReady) ExtCrcInit();
    while (Length--) {
        Crc = (Crc >> 8) ^ ExtCrc32cTable[(Crc ^ *p++) & 0xFF];
    }
    return Crc;
}

USHORT
ExtCrc16(USHORT Crc, const VOID *Data, ULONG Length)
{
    const UCHAR *p = (const UCHAR *)Data;
    if (!ExtCrcReady) ExtCrcInit();
    while (Length--) {
        Crc = (USHORT)((Crc >> 8) ^ ExtCrc16Table[(Crc ^ *p++) & 0xFF]);
    }
    return Crc;
}

/* ------------------------------------------------------------------ */
/* Time                                                                */
/* ------------------------------------------------------------------ */

/* NT 100ns ticks since 1601 -> ext (seconds since 1970 + nanoseconds<<2) */
#define EXT_EPOCH_100NS   116444736000000000ULL   /* 1601 -> 1970 */

VOID
ExtSetTime(ULONG *Field, ULONG *Extra, ULONGLONG Nt100ns)
{
    /* seconds since 1970 (signed), as ext4_encode_extra_time: the low 32 bits
       in the field, the epoch bits beyond in the low 2 bits of Extra */
    LONGLONG Unix100 = (LONGLONG)Nt100ns - (LONGLONG)EXT_EPOCH_100NS;
    LONGLONG Secs = Unix100 / 10000000LL;
    LONGLONG Rem = Unix100 % 10000000LL;
    ULONG Nsec;

    if (Rem < 0) { Rem += 10000000LL; Secs--; }
    if (Extra == NULL) {                    /* 128-byte inode: signed 32-bit only */
        if (Secs < -0x80000000LL) Secs = -0x80000000LL;
        if (Secs > 0x7FFFFFFFLL) Secs = 0x7FFFFFFFLL;
    } else {
        if (Secs < -0x80000000LL) Secs = -0x80000000LL;
        if (Secs > 0x37FFFFFFFLL) Secs = 0x37FFFFFFFLL;   /* year 2446 */
    }
    Nsec = (ULONG)(Rem * 100);
    *Field = (ULONG)Secs;
    if (Extra) {
        *Extra = ((ULONG)((Secs - (LONGLONG)(LONG)(ULONG)Secs) >> 32) & 3) | (Nsec << 2);
    }
}

ULONGLONG
ExtGetTime(ULONG Field, ULONG Extra)
{
    LONGLONG Secs = (LONGLONG)(LONG)Field;
    ULONG Nsec = Extra >> 2;
    Secs += (LONGLONG)(Extra & 3) << 32;
    if (Nsec > 999999999) Nsec = 0;
    if (Secs * 10000000LL + (LONGLONG)EXT_EPOCH_100NS < 0) return 0;
    return (ULONGLONG)((LONGLONG)EXT_EPOCH_100NS + Secs * 10000000LL + Nsec / 100);
}

/* ------------------------------------------------------------------ */
/* Block I/O through the host (in whole blocks)                        */
/* ------------------------------------------------------------------ */

EXT_STATUS
ExtMapBlocks(PEXT_VOL Vol, ULONGLONG Block, ULONG Count, ULONG Mode, PEXT_BUF Buf)
{
    ULONGLONG Sector = (ULONGLONG)Block * Vol->SectorsPerBlock;
    if (Block >= Vol->BlocksCount || Count == 0 ||
        (ULONGLONG)Sector + (ULONGLONG)Count * Vol->SectorsPerBlock > 0xFFFFFFFFULL) {
        return EXT_CORRUPT;
    }
    return ExtHostMap(Vol, (ULONG)Sector, Count * Vol->SectorsPerBlock, Mode, Buf);
}

/* Read one block into a caller buffer (no cache pin kept) */
static EXT_STATUS
ExtReadBlockInto(PEXT_VOL Vol, ULONGLONG Block, PVOID Into)
{
    EXT_BUF B;
    EXT_STATUS S = ExtMapBlocks(Vol, Block, 1, EXT_MAP_READ, &B);
    if (!NT_SUCCESS(S)) return S;
    ExtCopy(Into, B.Data, Vol->BlockSize);
    ExtHostUnmap(Vol, &B);
    return EXT_OK;
}

/* ------------------------------------------------------------------ */
/* Superblock checksum                                                 */
/* ------------------------------------------------------------------ */

static VOID
ExtSuperCsum(PEXT_VOL Vol)
{
    if (Vol->HasMetaCsum) {
        Vol->Super->Checksum = ExtCrc32c(0xFFFFFFFF, Vol->Super, 0x3FC);
    }
}

/* ------------------------------------------------------------------ */
/* Group descriptors                                                   */
/* ------------------------------------------------------------------ */

PEXT_GROUP_DESC
ExtGroupDesc(PEXT_VOL Vol, ULONG Group)
{
    return (PEXT_GROUP_DESC)(Vol->GroupDescs + (ULONGLONG)Group * Vol->DescSize);
}

ULONGLONG
ExtGdBlockBitmap(PEXT_VOL Vol, ULONG Group)
{
    PEXT_GROUP_DESC G = ExtGroupDesc(Vol, Group);
    ULONGLONG V = G->BlockBitmapLo;
    if (Vol->DescSize >= 64) V |= ((ULONGLONG)G->BlockBitmapHi) << 32;
    return V;
}

ULONGLONG
ExtGdInodeBitmap(PEXT_VOL Vol, ULONG Group)
{
    PEXT_GROUP_DESC G = ExtGroupDesc(Vol, Group);
    ULONGLONG V = G->InodeBitmapLo;
    if (Vol->DescSize >= 64) V |= ((ULONGLONG)G->InodeBitmapHi) << 32;
    return V;
}

ULONGLONG
ExtGdInodeTable(PEXT_VOL Vol, ULONG Group)
{
    PEXT_GROUP_DESC G = ExtGroupDesc(Vol, Group);
    ULONGLONG V = G->InodeTableLo;
    if (Vol->DescSize >= 64) V |= ((ULONGLONG)G->InodeTableHi) << 32;
    return V;
}

ULONG
ExtGdFreeBlocks(PEXT_VOL Vol, ULONG Group)
{
    PEXT_GROUP_DESC G = ExtGroupDesc(Vol, Group);
    ULONG V = G->FreeBlocksCountLo;
    if (Vol->DescSize >= 64) V |= ((ULONG)G->FreeBlocksCountHi) << 16;
    return V;
}

ULONG
ExtGdFreeInodes(PEXT_VOL Vol, ULONG Group)
{
    PEXT_GROUP_DESC G = ExtGroupDesc(Vol, Group);
    ULONG V = G->FreeInodesCountLo;
    if (Vol->DescSize >= 64) V |= ((ULONG)G->FreeInodesCountHi) << 16;
    return V;
}

static ULONG
ExtGdItableUnused(PEXT_VOL Vol, ULONG Group)
{
    PEXT_GROUP_DESC G = ExtGroupDesc(Vol, Group);
    ULONG V = G->ItableUnusedLo;
    if (Vol->DescSize >= 64) V |= ((ULONG)G->ItableUnusedHi) << 16;
    return V;
}

static VOID
ExtGdSetItableUnused(PEXT_VOL Vol, ULONG Group, ULONG Value)
{
    PEXT_GROUP_DESC G = ExtGroupDesc(Vol, Group);
    G->ItableUnusedLo = (USHORT)Value;
    if (Vol->DescSize >= 64) G->ItableUnusedHi = (USHORT)(Value >> 16);
}

VOID
ExtGdSetFreeBlocks(PEXT_VOL Vol, ULONG Group, ULONG Value)
{
    PEXT_GROUP_DESC G = ExtGroupDesc(Vol, Group);
    G->FreeBlocksCountLo = (USHORT)Value;
    if (Vol->DescSize >= 64) G->FreeBlocksCountHi = (USHORT)(Value >> 16);
}

VOID
ExtGdSetFreeInodes(PEXT_VOL Vol, ULONG Group, ULONG Value)
{
    PEXT_GROUP_DESC G = ExtGroupDesc(Vol, Group);
    G->FreeInodesCountLo = (USHORT)Value;
    if (Vol->DescSize >= 64) G->FreeInodesCountHi = (USHORT)(Value >> 16);
}

VOID
ExtGdAddUsedDirs(PEXT_VOL Vol, ULONG Group, LONG Delta)
{
    PEXT_GROUP_DESC G = ExtGroupDesc(Vol, Group);
    ULONG V = G->UsedDirsCountLo;
    if (Vol->DescSize >= 64) V |= ((ULONG)G->UsedDirsCountHi) << 16;
    V = (ULONG)((LONG)V + Delta);
    G->UsedDirsCountLo = (USHORT)V;
    if (Vol->DescSize >= 64) G->UsedDirsCountHi = (USHORT)(V >> 16);
}

/* The group descriptor checksum (metadata_csum crc32c, else gdt_csum crc16) */
static VOID
ExtGdCsum(PEXT_VOL Vol, ULONG Group)
{
    PEXT_GROUP_DESC G = ExtGroupDesc(Vol, Group);
    ULONG Offset = (ULONG)((PUCHAR)&G->Checksum - (PUCHAR)G);  /* 0x1E */
    ULONG Le = Group;      /* group number, little-endian in the formula */
    UCHAR GroupLe[4];

    if (!Vol->HasMetaCsum && !Vol->HasGdtCsum) {
        return;
    }
    GroupLe[0] = (UCHAR)Le; GroupLe[1] = (UCHAR)(Le >> 8);
    GroupLe[2] = (UCHAR)(Le >> 16); GroupLe[3] = (UCHAR)(Le >> 24);

    if (Vol->HasMetaCsum) {
        ULONG Crc;
        USHORT Save = G->Checksum;
        G->Checksum = 0;
        Crc = ExtCrc32c(Vol->ChecksumSeed, GroupLe, 4);
        Crc = ExtCrc32c(Crc, G, Vol->DescSize);
        G->Checksum = Save;
        G->Checksum = (USHORT)(Crc & 0xFFFF);
    } else {
        /* crc16 over uuid, group number, desc before csum, desc after csum */
        USHORT Crc = ExtCrc16(0xFFFF, Vol->Uuid, 16);
        Crc = ExtCrc16(Crc, GroupLe, 4);
        Crc = ExtCrc16(Crc, G, Offset);
        if (Vol->DescSize > Offset + 2) {
            Crc = ExtCrc16(Crc, (PUCHAR)G + Offset + 2, Vol->DescSize - (Offset + 2));
        }
        G->Checksum = Crc;
    }
}

VOID
ExtGroupTouched(PEXT_VOL Vol, ULONG Group)
{
    if (Group < Vol->GroupCount) {
        ExtGdCsum(Vol, Group);
        Vol->GroupDirty[Group >> 5] |= (1u << (Group & 31));
    }
}

/* ------------------------------------------------------------------ */
/* Where the group descriptor table and its backups live              */
/* ------------------------------------------------------------------ */

/* Groups that carry a superblock backup (ext2fs_bg_has_super) */
BOOLEAN
ExtGroupHasSuper(PEXT_VOL Vol, ULONG Group)
{
    ULONG t;
    if (Group == 0) return TRUE;
    if (Vol->FeatureCompat & EXT_FC_SPARSE_SUPER2)
        return (BOOLEAN)(Group == Vol->Super->BackupBgs[0] || Group == Vol->Super->BackupBgs[1]);
    if (Group <= 1 || !(Vol->FeatureRoCompat & EXT_RO_SPARSE_SUPER)) return TRUE;
    if ((Group & 1) == 0) return FALSE;
    for (t = 3; t <= Group; t *= 3) if (t == Group) return TRUE;
    for (t = 5; t <= Group; t *= 5) if (t == Group) return TRUE;
    for (t = 7; t <= Group; t *= 7) if (t == Group) return TRUE;
    return FALSE;
}

/* First block of group */
ULONGLONG
ExtGroupFirstBlock(PEXT_VOL Vol, ULONG Group)
{
    return (ULONGLONG)Group * Vol->BlocksPerGroup + Vol->FirstDataBlock;
}

/* Bits used in a group's block bitmap: its clusters (a last group may be short) */
ULONG
ExtGroupClusters(PEXT_VOL Vol, ULONG Group)
{
    ULONGLONG First = ExtGroupFirstBlock(Vol, Group);
    ULONGLONG Blocks = (First + Vol->BlocksPerGroup <= Vol->BlocksCount) ? Vol->BlocksPerGroup
                                                                         : Vol->BlocksCount - First;
    return (ULONG)((Blocks + (1u << Vol->ClusterBits) - 1) >> Vol->ClusterBits);
}

static ULONG
ExtDescPerBlock(PEXT_VOL Vol)
{
    return Vol->BlockSize / Vol->DescSize;
}

ULONG
ExtGdtBlockCount(PEXT_VOL Vol)
{
    return (Vol->GroupCount + ExtDescPerBlock(Vol) - 1) / ExtDescPerBlock(Vol);
}

/* Where the primary copy of descriptor block i lives
   (ext2fs_descriptor_block_loc2 for the primary) */
static ULONGLONG
ExtDescBlockLoc(PEXT_VOL Vol, ULONG i)
{
    ULONG Bg;
    if (!Vol->HasMetaBg || i < Vol->Super->FirstMetaBg)
        /* after the superblock's block (block 1 for 1 KB blocks, also with
           bigalloc, where the first data block is 0) */
        return ((Vol->FirstDataBlock == 0 && Vol->BlockSize == 1024) ? 1 : Vol->FirstDataBlock) + 1 + i;
    Bg = ExtDescPerBlock(Vol) * i;              /* meta_bg: in the first group of its metagroup */
    return ExtGroupFirstBlock(Vol, Bg) + (ExtGroupHasSuper(Vol, Bg) ? 1 : 0);
}

/* Descriptor blocks (and superblock backups) at the start of a group:
   ext4_num_base_meta_clusters */
ULONG
ExtGroupBaseMeta(PEXT_VOL Vol, ULONG Group)
{
    ULONG N = ExtGroupHasSuper(Vol, Group) ? 1 : 0;
    ULONG Dpb = ExtDescPerBlock(Vol);
    if (!Vol->HasMetaBg || Group < Vol->Super->FirstMetaBg * Dpb) {
        if (N) N += (Vol->HasMetaBg ? Vol->Super->FirstMetaBg : ExtGdtBlockCount(Vol)) + Vol->Super->ReservedGdtBlocks;
    } else {
        ULONG InMeta = Group % Dpb;            /* first, second and last of a metagroup hold a copy */
        if (InMeta == 0 || InMeta == 1 || InMeta == Dpb - 1) N += 1;
    }
    /* 1 KB blocks with bigalloc: group 0 starts at block 0, the boot block */
    if (Group == 0 && N && Vol->FirstDataBlock == 0 && Vol->BlockSize == 1024) N += 1;
    return N;
}

/* ------------------------------------------------------------------ */
/* Reading and writing group descriptors                              */
/* ------------------------------------------------------------------ */

static EXT_STATUS
ExtReadGroupDescs(PEXT_VOL Vol)
{
    ULONG GdtBlocks = ExtGdtBlockCount(Vol);
    ULONG b;
    EXT_STATUS S;

    Vol->GroupDescs = (PUCHAR)ExtHostAlloc(GdtBlocks * Vol->BlockSize);
    Vol->GroupDirty = (PULONG)ExtHostAlloc(((Vol->GroupCount >> 5) + 1) * sizeof(ULONG));
    if (Vol->GroupDescs == NULL || Vol->GroupDirty == NULL) {
        return EXT_NO_MEMORY;
    }
    ExtZero(Vol->GroupDirty, ((Vol->GroupCount >> 5) + 1) * sizeof(ULONG));

    for (b = 0; b < GdtBlocks; b++) {
        ULONGLONG Loc = ExtDescBlockLoc(Vol, b);
        if (Loc >= Vol->BlocksCount) return EXT_CORRUPT;
        S = ExtReadBlockInto(Vol, Loc, Vol->GroupDescs + (ULONGLONG)b * Vol->BlockSize);
        if (!NT_SUCCESS(S)) return S;
    }
    return EXT_OK;
}

/* Writes the primary copy of every descriptor block with a changed group.
   Like the Linux kernel, the backup copies are left as mkfs/fsck/resize
   made them (they only matter if the primary is lost). */
static EXT_STATUS
ExtWriteGroupDescs(PEXT_VOL Vol)
{
    ULONG Dpb = ExtDescPerBlock(Vol), GdtBlocks = ExtGdtBlockCount(Vol), b, g;
    EXT_STATUS S;

    for (b = 0; b < GdtBlocks; b++) {
        BOOLEAN Dirty = FALSE;
        EXT_BUF Buf;
        for (g = b * Dpb; g < (b + 1) * Dpb && g < Vol->GroupCount; g++)
            if (Vol->GroupDirty[g >> 5] & (1u << (g & 31))) { Dirty = TRUE; break; }
        if (!Dirty) continue;
        S = ExtMapBlocks(Vol, ExtDescBlockLoc(Vol, b), 1, EXT_MAP_WRITE, &Buf);
        if (!NT_SUCCESS(S)) return S;
        ExtCopy(Buf.Data, Vol->GroupDescs + (ULONGLONG)b * Vol->BlockSize, Vol->BlockSize);
        ExtSetDirty(Vol, &Buf);
        ExtHostUnmap(Vol, &Buf);
    }
    ExtZero(Vol->GroupDirty, ((Vol->GroupCount >> 5) + 1) * sizeof(ULONG));
    return EXT_OK;
}

/* ------------------------------------------------------------------ */
/* Superblock: write the primary and the backups                      */
/* ------------------------------------------------------------------ */

EXT_STATUS
ExtFlushSuper(PEXT_VOL Vol)
{
    EXT_BUF Buf;
    EXT_STATUS S;
    ULONG g;
    ULONG SbLen = (Vol->BlockSize > 1024) ? 1024 : Vol->BlockSize;

    if (Vol->ReadOnly) return EXT_OK;
    /* original-ext has no group descriptors or ext2-shaped superblock; its write
       engine maintains the on-disk superblock itself (see extold.c) */
    if (Vol->OldExt) return EXT_OK;

    S = ExtWriteGroupDescs(Vol);
    if (!NT_SUCCESS(S)) return S;

    /* in-memory counters -> superblock */
    Vol->Super->FreeBlocksCountLo = (ULONG)Vol->FreeBlocksCount;
    if (Vol->Has64Bit) Vol->Super->FreeBlocksCountHi = (ULONG)(Vol->FreeBlocksCount >> 32);
    Vol->Super->FreeInodesCount = Vol->FreeInodesCount;

    ExtSuperCsum(Vol);

    /* Primary: at byte 1024 */
    S = ExtHostMap(Vol, Vol->SuperSector, (SbLen + 511) / 512, EXT_MAP_WRITE, &Buf);
    if (!NT_SUCCESS(S)) return S;
    ExtCopy(Buf.Data, Vol->Super, SbLen);
    ExtSetDirty(Vol, &Buf);
    ExtHostUnmap(Vol, &Buf);

    /* Backups (block 0 of each backup group, s_block_group_nr set) only when
       something they describe changed: features, label (Linux likewise
       leaves them alone in normal operation) */
    if (Vol->BackupsStale) {
        for (g = 1; g < Vol->GroupCount; g++) {
            ULONGLONG BlockNo;
            if (!ExtGroupHasSuper(Vol, g)) continue;
            BlockNo = ExtGroupFirstBlock(Vol, g);
            Vol->Super->BlockGroupNr = (USHORT)g;
            ExtSuperCsum(Vol);
            S = ExtMapBlocks(Vol, BlockNo, 1, EXT_MAP_WRITE, &Buf);
            if (NT_SUCCESS(S)) {
                ExtCopy(Buf.Data, Vol->Super, SbLen);
                ExtSetDirty(Vol, &Buf);
                ExtHostUnmap(Vol, &Buf);
            }
        }
        Vol->BackupsStale = FALSE;
    }
    Vol->Super->BlockGroupNr = 0;
    ExtSuperCsum(Vol);
    return EXT_OK;
}

/* ------------------------------------------------------------------ */
/* Mount                                                               */
/* ------------------------------------------------------------------ */

BOOLEAN
ExtIsSuperblock(const VOID *Block1024)
{
    const EXT_SUPERBLOCK *Sb = (const EXT_SUPERBLOCK *)Block1024;
    /* ext2/3/4, or the original 1992 ext (magic 0x137D, read-only) */
    return (BOOLEAN)(Sb->Magic == EXT_SUPER_MAGIC || Sb->Magic == 0x137D);
}

EXT_STATUS
ExtMount(PEXT_VOL Vol, BOOLEAN ReadOnly)
{
    EXT_BUF Buf;
    PEXT_SUPERBLOCK Sb;
    EXT_STATUS S;
    ULONGLONG Blocks;
    ULONG i;

    Vol->ReadOnly = ReadOnly;
    Vol->Super = NULL;
    Vol->GroupDescs = NULL;
    Vol->GroupDirty = NULL;
    Vol->Journal = NULL;

    /* The superblock is 1024 bytes at byte offset 1024: sectors 2 and 3 */
    S = ExtHostMap(Vol, 2, 2, EXT_MAP_READ, &Buf);
    if (!NT_SUCCESS(S)) return S;
    Sb = (PEXT_SUPERBLOCK)Buf.Data;
    if (Sb->Magic != EXT_SUPER_MAGIC) {
        /* the original 1992 ext (pre-ext2): read-only, handled apart */
        if (Sb->Magic == 0x137D) {
            EXT_STATUS Old = ExtOldMount(Vol, (const UCHAR *)Buf.Data);
            ExtHostUnmap(Vol, &Buf);
            return Old;
        }
        ExtHostUnmap(Vol, &Buf);
        return EXT_UNRECOGNIZED;
    }
    if (Sb->LogBlockSize > 6) {       /* up to 64 KB blocks */
        ExtHostUnmap(Vol, &Buf);
        return EXT_CORRUPT;
    }

    Vol->BlockSize = 1024u << Sb->LogBlockSize;
    Vol->BlockShift = 10 + Sb->LogBlockSize;
    Vol->SectorsPerBlock = Vol->BlockSize / 512;

    Vol->Super = (PEXT_SUPERBLOCK)ExtHostAlloc(Vol->BlockSize > 1024 ? Vol->BlockSize : 1024);
    if (Vol->Super == NULL) {
        ExtHostUnmap(Vol, &Buf);
        return EXT_NO_MEMORY;
    }
    ExtZero(Vol->Super, Vol->BlockSize > 1024 ? Vol->BlockSize : 1024);
    ExtCopy(Vol->Super, Buf.Data, 1024);
    ExtHostUnmap(Vol, &Buf);
    Sb = Vol->Super;
    Vol->SuperSector = 2;

    Vol->FeatureCompat = Sb->FeatureCompat;
    Vol->FeatureIncompat = Sb->FeatureIncompat;
    Vol->FeatureRoCompat = Sb->FeatureRoCompat;

    /* Refuse unknown incompat features; force read-only on unknown ro-compat */
    if (Vol->FeatureIncompat & ~(EXT_FI_SUPPORTED | EXT_FI_READ_ONLY)) {
        return EXT_NOT_SUPPORTED;
    }
    Vol->ReadOnlyForced = FALSE;
    /* unknown ro_compat bits (quota, bigalloc, project, verity, orphan_present
       ...), read-only incompat features: readable, not writable */
    if ((Vol->FeatureRoCompat & ~EXT_RO_SUPPORTED) || (Vol->FeatureIncompat & EXT_FI_READ_ONLY)) {
        Vol->ReadOnly = TRUE;
        Vol->ReadOnlyForced = TRUE;
    }

    Vol->Has64Bit = (BOOLEAN)((Vol->FeatureIncompat & EXT_FI_64BIT) != 0);
    Vol->HasExtents = (BOOLEAN)((Vol->FeatureIncompat & EXT_FI_EXTENTS) != 0);
    Vol->HasFiletype = (BOOLEAN)((Vol->FeatureIncompat & EXT_FI_FILETYPE) != 0);
    Vol->HasInlineData = (BOOLEAN)((Vol->FeatureIncompat & EXT_FI_INLINE_DATA) != 0);
    Vol->HasFlexBg = (BOOLEAN)((Vol->FeatureIncompat & EXT_FI_FLEX_BG) != 0);
    Vol->HasMetaBg = (BOOLEAN)((Vol->FeatureIncompat & EXT_FI_META_BG) != 0);
    Vol->HasMetaCsum = (BOOLEAN)((Vol->FeatureRoCompat & EXT_RO_METADATA_CSUM) != 0);
    Vol->HasGdtCsum = (BOOLEAN)(!Vol->HasMetaCsum && (Vol->FeatureRoCompat & EXT_RO_GDT_CSUM) != 0);
    Vol->HasJournal = (BOOLEAN)((Vol->FeatureCompat & EXT_FC_HAS_JOURNAL) != 0);
    Vol->HasHtree = (BOOLEAN)((Vol->FeatureCompat & EXT_FC_DIR_INDEX) != 0);
    Vol->LogGroupsPerFlex = Vol->HasFlexBg ? Sb->LogGroupsPerFlex : 0;

    Vol->BlocksCount = Sb->BlocksCountLo;
    if (Vol->Has64Bit) Vol->BlocksCount |= ((ULONGLONG)Sb->BlocksCountHi) << 32;
    Vol->InodesCount = Sb->InodesCount;
    Vol->FirstDataBlock = Sb->FirstDataBlock;
    Vol->BlocksPerGroup = Sb->BlocksPerGroup;
    Vol->InodesPerGroup = Sb->InodesPerGroup;
    Vol->FirstIno = (Sb->RevLevel == 0) ? EXT_FIRST_NONRES_INO : Sb->FirstIno;
    Vol->InodeSize = (Sb->RevLevel == 0) ? EXT_GOOD_OLD_INODE_SIZE : Sb->InodeSize;
    if (Vol->InodeSize < EXT_GOOD_OLD_INODE_SIZE || Vol->InodeSize > Vol->BlockSize ||
        (Vol->InodeSize & (Vol->InodeSize - 1)) != 0) {
        return EXT_CORRUPT;
    }
    Vol->DescSize = (Vol->Has64Bit && Sb->DescSize >= 64) ? Sb->DescSize : 32;
    Vol->JournalInum = Sb->JournalInum;
    Vol->MkfsTime = Sb->MkfsTime;
    Vol->DefHashVersion = Sb->DefHashVersion;
    for (i = 0; i < 4; i++) Vol->HashSeed[i] = Sb->HashSeed[i];
    for (i = 0; i < 16; i++) Vol->Uuid[i] = Sb->Uuid[i];
    for (i = 0; i < 16; i++) Vol->VolumeName[i] = Sb->VolumeName[i];
    Vol->VolumeName[16] = 0;
    Vol->SerialNumber = *(ULONG *)&Sb->Uuid[0] ^ *(ULONG *)&Sb->Uuid[8];

    /* bigalloc: a bitmap bit is a cluster of 2^n blocks, and every
       allocation is a whole cluster */
    Vol->ClusterBits = 0;
    Vol->ClustersPerGroup = Vol->BlocksPerGroup;
    if (Vol->FeatureRoCompat & EXT_RO_BIGALLOC) {
        if (Sb->LogClusterSize < Sb->LogBlockSize || Sb->LogClusterSize - Sb->LogBlockSize > 20 ||
            Sb->ClustersPerGroup == 0 || Vol->FirstDataBlock != 0 ||
            ((ULONGLONG)Sb->ClustersPerGroup << (Sb->LogClusterSize - Sb->LogBlockSize)) != Vol->BlocksPerGroup) {
            return EXT_CORRUPT;
        }
        Vol->ClusterBits = Sb->LogClusterSize - Sb->LogBlockSize;
        Vol->ClustersPerGroup = Sb->ClustersPerGroup;
        if (!Vol->HasExtents) {         /* block-mapped files cannot follow the cluster rules */
            Vol->ReadOnly = TRUE;
            Vol->ReadOnlyForced = TRUE;
        }
    }
    Vol->SectorsPerCluster = Vol->SectorsPerBlock << Vol->ClusterBits;
    if (Vol->BlocksPerGroup == 0 || Vol->InodesPerGroup == 0 ||
        Vol->ClustersPerGroup > Vol->BlockSize * 8) {
        return EXT_CORRUPT;
    }

    Blocks = Vol->BlocksCount - Vol->FirstDataBlock;
    Vol->GroupCount = (ULONG)((Blocks + Vol->BlocksPerGroup - 1) / Vol->BlocksPerGroup);
    Vol->ItbPerGroup = (Vol->InodesPerGroup * Vol->InodeSize + Vol->BlockSize - 1) / Vol->BlockSize;

    /* The checksum seed: the superblock's own field, or crc32c(~0, uuid) */
    if ((Vol->FeatureIncompat & EXT_FI_CSUM_SEED) && Sb->ChecksumSeed != 0) {
        Vol->ChecksumSeed = Sb->ChecksumSeed;
    } else if (Vol->HasMetaCsum) {
        Vol->ChecksumSeed = ExtCrc32c(0xFFFFFFFF, Vol->Uuid, 16);
    } else {
        Vol->ChecksumSeed = 0;
    }

    Vol->Dirty = (BOOLEAN)((Sb->State & EXT_VALID_FS) == 0 || (Sb->State & EXT_ERROR_FS) != 0);
    Vol->NeedsRecovery = (BOOLEAN)((Vol->FeatureIncompat & EXT_FI_RECOVER) != 0);

    S = ExtReadGroupDescs(Vol);
    if (!NT_SUCCESS(S)) return S;

    Vol->FreeBlocksCount = Sb->FreeBlocksCountLo;
    if (Vol->Has64Bit) Vol->FreeBlocksCount |= ((ULONGLONG)Sb->FreeBlocksCountHi) << 32;
    Vol->FreeInodesCount = Sb->FreeInodesCount;

    Vol->LastAllocGroup = 0;
    return EXT_OK;
}

VOID
ExtUnmountFree(PEXT_VOL Vol)
{
    ExtJournalClose(Vol);
    if (Vol->GroupDescs) { ExtHostFree(Vol->GroupDescs); Vol->GroupDescs = NULL; }
    if (Vol->GroupDirty) { ExtHostFree(Vol->GroupDirty); Vol->GroupDirty = NULL; }
    if (Vol->Super) { ExtHostFree(Vol->Super); Vol->Super = NULL; }
    if (Vol->PendingFree) { ExtHostFree(Vol->PendingFree); Vol->PendingFree = NULL; }
    Vol->PendingCount = Vol->PendingCap = 0;
}

EXT_STATUS
ExtSetState(PEXT_VOL Vol, BOOLEAN Clean)
{
    if (Vol->ReadOnly) return EXT_OK;
    if (Clean) {
        Vol->Super->State |= EXT_VALID_FS;     /* errors stay until fsck clears them */
    } else {
        Vol->Super->State &= ~EXT_VALID_FS;
    }
    Vol->Super->WriteTime = ExtHostNow();
    return ExtFlushSuper(Vol);
}

EXT_STATUS
ExtSetLabel(PEXT_VOL Vol, const char *Label)
{
    ULONG i;
    if (Vol->ReadOnly) return EXT_READ_ONLY;
    Vol->BackupsStale = TRUE;
    for (i = 0; i < 16; i++) {
        Vol->Super->VolumeName[i] = (Label && Label[i]) ? Label[i] : 0;
        Vol->VolumeName[i] = Vol->Super->VolumeName[i];
        if (Label == NULL || Label[i] == 0) {
            for (; i < 16; i++) { Vol->Super->VolumeName[i] = 0; Vol->VolumeName[i] = 0; }
            break;
        }
    }
    Vol->VolumeName[16] = 0;
    return ExtFlushSuper(Vol);
}

/* ------------------------------------------------------------------ */
/* Inodes                                                              */
/* ------------------------------------------------------------------ */

EXT_STATUS
ExtInodeLocation(PEXT_VOL Vol, ULONG Ino, PULONGLONG Sector, PULONG Offset)
{
    ULONG Group, Index;
    ULONGLONG Itable, ByteOff, Blk;

    if (Ino == 0 || Ino > Vol->InodesCount) return EXT_CORRUPT;
    Group = (Ino - 1) / Vol->InodesPerGroup;
    Index = (Ino - 1) % Vol->InodesPerGroup;
    Itable = ExtGdInodeTable(Vol, Group);
    ByteOff = (ULONGLONG)Index * Vol->InodeSize;
    Blk = Itable + ByteOff / Vol->BlockSize;
    *Sector = Blk * Vol->SectorsPerBlock + (ByteOff % Vol->BlockSize) / 512;
    *Offset = (ULONG)(ByteOff % 512);
    return EXT_OK;
}

EXT_STATUS
ExtReadInode(PEXT_VOL Vol, ULONG Ino, PEXT_INODE Inode)
{
    ULONGLONG Sector;
    ULONG Offset, Need;
    EXT_BUF Buf;
    EXT_STATUS S;

    if (Vol->OldExt) return ExtOldReadInode(Vol, Ino, Inode);
    S = ExtInodeLocation(Vol, Ino, &Sector, &Offset);
    if (!NT_SUCCESS(S)) return S;

    /* The inode may straddle two sectors; map enough */
    Need = (Offset + Vol->InodeSize + 511) / 512;
    S = ExtHostMap(Vol, Sector, Need, EXT_MAP_READ, &Buf);
    if (!NT_SUCCESS(S)) return S;

    ExtZero(Inode, sizeof(EXT_INODE));
    ExtCopy(Inode, Buf.Data + Offset, MIN_UL(Vol->InodeSize, sizeof(EXT_INODE)));
    ExtHostUnmap(Vol, &Buf);
    return EXT_OK;
}

VOID
ExtInodeCsumImage(PEXT_VOL Vol, ULONG Ino, PUCHAR Image)
{
    UCHAR InoLe[4], GenLe[4];
    ULONG Crc, Gen;
    BOOLEAN HasHi;

    if (!Vol->HasMetaCsum) return;
    HasHi = (BOOLEAN)(Vol->InodeSize > EXT_GOOD_OLD_INODE_SIZE);
    Gen = *(ULONG *)(Image + 0x64);

    InoLe[0]=(UCHAR)Ino; InoLe[1]=(UCHAR)(Ino>>8); InoLe[2]=(UCHAR)(Ino>>16); InoLe[3]=(UCHAR)(Ino>>24);
    GenLe[0]=(UCHAR)Gen; GenLe[1]=(UCHAR)(Gen>>8); GenLe[2]=(UCHAR)(Gen>>16); GenLe[3]=(UCHAR)(Gen>>24);

    *(USHORT *)(Image + 0x7C) = 0;
    if (HasHi) *(USHORT *)(Image + 0x82) = 0;

    Crc = ExtCrc32c(Vol->ChecksumSeed, InoLe, 4);
    Crc = ExtCrc32c(Crc, GenLe, 4);
    Crc = ExtCrc32c(Crc, Image, Vol->InodeSize);

    *(USHORT *)(Image + 0x7C) = (USHORT)(Crc & 0xFFFF);
    if (HasHi) *(USHORT *)(Image + 0x82) = (USHORT)(Crc >> 16);
}

VOID
ExtInodeCsum(PEXT_VOL Vol, ULONG Ino, PEXT_INODE Inode)
{
    (VOID)Vol; (VOID)Ino; (VOID)Inode;   /* exact value set at write time */
}

/* Bytes of an inode that belong to the struct: 128 + i_extra_isize (the rest
   of a large inode holds in-inode xattrs, which ExtWriteInode leaves alone) */
ULONG
ExtInodeCoreSize(PEXT_VOL Vol, PEXT_INODE Inode)
{
    ULONG N = EXT_GOOD_OLD_INODE_SIZE;
    if (Vol->InodeSize > EXT_GOOD_OLD_INODE_SIZE && Inode->ExtraIsize &&
        EXT_GOOD_OLD_INODE_SIZE + (ULONG)Inode->ExtraIsize <= Vol->InodeSize)
        N += Inode->ExtraIsize;
    return MIN_UL(N, (ULONG)sizeof(EXT_INODE));
}

/* A freshly allocated inode starts from zeros on disk (like Linux), so no
   stale in-inode attributes of a previous owner survive */
EXT_STATUS
ExtZeroInodeImage(PEXT_VOL Vol, ULONG Ino)
{
    ULONGLONG Sector;
    ULONG Offset, Need;
    EXT_BUF Buf;
    EXT_STATUS S = ExtInodeLocation(Vol, Ino, &Sector, &Offset);
    if (!NT_SUCCESS(S)) return S;
    Need = (Offset + Vol->InodeSize + 511) / 512;
    S = ExtHostMap(Vol, Sector, Need, EXT_MAP_WRITE, &Buf);
    if (!NT_SUCCESS(S)) return S;
    ExtZero(Buf.Data + Offset, Vol->InodeSize);
    ExtSetDirty(Vol, &Buf);
    ExtHostUnmap(Vol, &Buf);
    return EXT_OK;
}

EXT_STATUS
ExtWriteInode(PEXT_VOL Vol, ULONG Ino, PEXT_INODE Inode)
{
    ULONGLONG Sector;
    ULONG Offset, Need;
    EXT_BUF Buf;
    EXT_STATUS S;

    if (Vol->ReadOnly) return EXT_READ_ONLY;
    S = ExtInodeLocation(Vol, Ino, &Sector, &Offset);
    if (!NT_SUCCESS(S)) return S;
    Need = (Offset + Vol->InodeSize + 511) / 512;
    S = ExtHostMap(Vol, Sector, Need, EXT_MAP_WRITE, &Buf);    /* read-modify-write */
    if (!NT_SUCCESS(S)) return S;
    ExtCopy(Buf.Data + Offset, Inode, ExtInodeCoreSize(Vol, Inode));
    ExtInodeCsumImage(Vol, Ino, Buf.Data + Offset);
    ExtSetDirty(Vol, &Buf);
    ExtHostUnmap(Vol, &Buf);
    return EXT_OK;
}

ULONGLONG
ExtInodeSize(PEXT_VOL Vol, PEXT_INODE Inode)
{
    ULONGLONG Size = Inode->SizeLo;
    USHORT Mode = Inode->Mode & EXT_S_IFMT;
    if (Mode == EXT_S_IFREG || Mode == EXT_S_IFDIR) {
        Size |= ((ULONGLONG)Inode->SizeHi) << 32;
    }
    return Size;
}

VOID
ExtInodeSetSize(PEXT_VOL Vol, PEXT_INODE Inode, ULONGLONG Size)
{
    Inode->SizeLo = (ULONG)Size;
    Inode->SizeHi = (ULONG)(Size >> 32);
    /* a regular file of 2 GB or more needs the large_file feature */
    if (Size > 0x7FFFFFFFULL && !(Vol->FeatureRoCompat & EXT_RO_LARGE_FILE) && Vol->Super) {
        Vol->FeatureRoCompat |= EXT_RO_LARGE_FILE;
        Vol->Super->FeatureRoCompat |= EXT_RO_LARGE_FILE;
        Vol->BackupsStale = TRUE;
    }
}

ULONGLONG
ExtInodeBlocks(PEXT_VOL Vol, PEXT_INODE Inode)
{
    ULONGLONG B = Inode->BlocksLo;
    if (Vol->FeatureRoCompat & EXT_RO_HUGE_FILE) {
        B |= ((ULONGLONG)Inode->BlocksHi) << 32;
        if (Inode->Flags & EXT_IF_HUGE_FILE) {
            /* measured in filesystem blocks, not 512-byte sectors */
            B *= Vol->SectorsPerBlock;
        }
    }
    return B;
}

VOID
ExtInodeSetBlocks(PEXT_VOL Vol, PEXT_INODE Inode, ULONGLONG Sectors512)
{
    Inode->Flags &= ~EXT_IF_HUGE_FILE;
    Inode->BlocksLo = (ULONG)Sectors512;
    if (Vol->FeatureRoCompat & EXT_RO_HUGE_FILE) {
        Inode->BlocksHi = (USHORT)(Sectors512 >> 32);
    }
}

/* ------------------------------------------------------------------ */
/* Block and inode bitmaps                                             */
/* ------------------------------------------------------------------ */

EXT_STATUS
ExtBitmapCsum(PEXT_VOL Vol, ULONG Group, PUCHAR Bitmap, ULONG Bytes, BOOLEAN Inode)
{
    PEXT_GROUP_DESC G = ExtGroupDesc(Vol, Group);
    ULONG Crc;
    if (!Vol->HasMetaCsum) return EXT_OK;
    Crc = ExtCrc32c(Vol->ChecksumSeed, Bitmap, Bytes);
    if (Inode) {
        G->InodeBitmapCsumLo = (USHORT)(Crc & 0xFFFF);
        if (Vol->DescSize >= 64) G->InodeBitmapCsumHi = (USHORT)(Crc >> 16);
    } else {
        G->BlockBitmapCsumLo = (USHORT)(Crc & 0xFFFF);
        if (Vol->DescSize >= 64) G->BlockBitmapCsumHi = (USHORT)(Crc >> 16);
    }
    return EXT_OK;
}

/* The block bitmap of a group, materialised (handles BLOCK_UNINIT) */
EXT_STATUS
ExtMapBlockBitmap(PEXT_VOL Vol, ULONG Group, PEXT_BUF Buf, PBOOLEAN WasUninit)
{
    PEXT_GROUP_DESC G = ExtGroupDesc(Vol, Group);
    ULONGLONG Bm = ExtGdBlockBitmap(Vol, Group);
    EXT_STATUS S;

    *WasUninit = (BOOLEAN)((G->Flags & EXT_BG_BLOCK_UNINIT) != 0);
    S = ExtMapBlocks(Vol, Bm, 1, EXT_MAP_WRITE, Buf);
    if (!NT_SUCCESS(S)) return S;
    if (*WasUninit) {
        /* Build the bitmap: metadata blocks of this group marked used */
        ULONG i;
        ULONGLONG First = ExtGroupFirstBlock(Vol, Group);
        ULONG Used = 0;
        ExtZero(Buf->Data, Vol->BlockSize);
        /* superblock + gdt copy in sparse groups */
        Used = ExtGroupBaseMeta(Vol, Group);
        /* bitmaps + inode table of this group (flex_bg puts them in group 0's
           area, but for a fresh group the classic layout is per group) */
        /* (a bit is a cluster: block offset >> ClusterBits) */
        for (i = 0; i < Used; i++) Buf->Data[(i >> Vol->ClusterBits) >> 3] |= (UCHAR)(1 << ((i >> Vol->ClusterBits) & 7));
        /* Mark this group's own bitmaps + inode table wherever they are */
        {
            ULONGLONG Bb = ExtGdBlockBitmap(Vol, Group);
            ULONGLONG Ib = ExtGdInodeBitmap(Vol, Group);
            ULONGLONG It = ExtGdInodeTable(Vol, Group);
            ULONGLONG p;
            ULONG rel;
            if (Bb >= First && Bb < First + Vol->BlocksPerGroup) {
                rel = (ULONG)(Bb - First) >> Vol->ClusterBits; Buf->Data[rel >> 3] |= (UCHAR)(1 << (rel & 7));
            }
            if (Ib >= First && Ib < First + Vol->BlocksPerGroup) {
                rel = (ULONG)(Ib - First) >> Vol->ClusterBits; Buf->Data[rel >> 3] |= (UCHAR)(1 << (rel & 7));
            }
            for (p = It; p < It + Vol->ItbPerGroup; p++) {
                if (p >= First && p < First + Vol->BlocksPerGroup) {
                    rel = (ULONG)(p - First) >> Vol->ClusterBits; Buf->Data[rel >> 3] |= (UCHAR)(1 << (rel & 7));
                }
            }
        }
        /* pad bits past the group's cluster count */
        {
            ULONG b;
            for (b = ExtGroupClusters(Vol, Group); b < Vol->BlockSize * 8; b++) {
                Buf->Data[b >> 3] |= (UCHAR)(1 << (b & 7));
            }
        }
        /* the built bitmap must reach the disk even if nothing is allocated
           (read-only: it only lives in this mapping) */
        if (!Vol->ReadOnly) {
            G->Flags &= ~EXT_BG_BLOCK_UNINIT;
            ExtBitmapCsum(Vol, Group, Buf->Data, Vol->ClustersPerGroup / 8, FALSE);
            ExtSetDirty(Vol, Buf);
            ExtGroupTouched(Vol, Group);
        }
    }
    return EXT_OK;
}

static EXT_STATUS
ExtMapInodeBitmap(PEXT_VOL Vol, ULONG Group, PEXT_BUF Buf)
{
    PEXT_GROUP_DESC G = ExtGroupDesc(Vol, Group);
    ULONGLONG Bm = ExtGdInodeBitmap(Vol, Group);
    EXT_STATUS S;
    BOOLEAN Uninit = (BOOLEAN)((G->Flags & EXT_BG_INODE_UNINIT) != 0);

    S = ExtMapBlocks(Vol, Bm, 1, EXT_MAP_WRITE, Buf);
    if (!NT_SUCCESS(S)) return S;
    if (Uninit) {
        ULONG b;
        ExtZero(Buf->Data, Vol->BlockSize);
        /* bits past inodes_per_group are set */
        for (b = Vol->InodesPerGroup; b < Vol->BlockSize * 8; b++) {
            Buf->Data[b >> 3] |= (UCHAR)(1 << (b & 7));
        }
        if (!Vol->ReadOnly) {
            G->Flags &= ~EXT_BG_INODE_UNINIT;
            ExtBitmapCsum(Vol, Group, Buf->Data, (Vol->InodesPerGroup + 7) / 8, TRUE);
            ExtSetDirty(Vol, Buf);
            ExtGroupTouched(Vol, Group);
        }
    }
    return EXT_OK;
}

static EXT_STATUS ExtAllocBlockInner(PEXT_VOL Vol, ULONGLONG Near, PULONGLONG Block);

EXT_STATUS
ExtAllocBlock(PEXT_VOL Vol, ULONGLONG Near, PULONGLONG Block)
{
    EXT_STATUS S = ExtAllocBlockInner(Vol, Near, Block);
    if (NT_SUCCESS(S)) Vol->AllocTotal++;
    return S;
}

static EXT_STATUS
ExtAllocBlockInner(PEXT_VOL Vol, ULONGLONG Near, PULONGLONG Block)
{
    ULONG g, Start, gi;
    ULONG NearGroup;

    if (Vol->ReadOnly) return EXT_READ_ONLY;
    if (Vol->FreeBlocksCount < (1u << Vol->ClusterBits)) return EXT_DISK_FULL;

    NearGroup = (Near >= Vol->FirstDataBlock && Near < Vol->BlocksCount)
                ? (ULONG)((Near - Vol->FirstDataBlock) / Vol->BlocksPerGroup) : Vol->LastAllocGroup;
    Start = NearGroup;

    for (gi = 0; gi < Vol->GroupCount; gi++) {
        EXT_BUF Buf;
        BOOLEAN Uninit;
        ULONG bit, Bits;
        ULONGLONG First;
        g = (Start + gi) % Vol->GroupCount;
        if (ExtGdFreeBlocks(Vol, g) == 0) continue;

        if (!NT_SUCCESS(ExtMapBlockBitmap(Vol, g, &Buf, &Uninit))) continue;
        First = ExtGroupFirstBlock(Vol, g);
        Bits = ExtGroupClusters(Vol, g);

        /* prefer the near block within this group */
        bit = 0;
        if (g == NearGroup && Near >= First && ((Near - First) >> Vol->ClusterBits) < Bits) {
            bit = (ULONG)((Near - First) >> Vol->ClusterBits);
        }
        for (; bit < Bits; bit++) {
            if (!(Buf.Data[bit >> 3] & (1 << (bit & 7)))) break;
        }
        if (bit >= Bits) {
            for (bit = 0; bit < Bits; bit++)
                if (!(Buf.Data[bit >> 3] & (1 << (bit & 7)))) break;
        }
        if (bit >= Bits) { ExtHostUnmap(Vol, &Buf); continue; }

        Buf.Data[bit >> 3] |= (UCHAR)(1 << (bit & 7));
        ExtBitmapCsum(Vol, g, Buf.Data, Vol->ClustersPerGroup / 8, FALSE);
        ExtSetDirty(Vol, &Buf);
        ExtHostUnmap(Vol, &Buf);

        ExtGdSetFreeBlocks(Vol, g, ExtGdFreeBlocks(Vol, g) - 1);     /* clusters */
        ExtGroupTouched(Vol, g);
        Vol->FreeBlocksCount -= 1u << Vol->ClusterBits;
        Vol->LastAllocGroup = g;
        *Block = First + ((ULONGLONG)bit << Vol->ClusterBits);    /* the cluster's first block */
        return EXT_OK;
    }
    return EXT_DISK_FULL;
}

static EXT_STATUS
ExtFreeBlockNow(PEXT_VOL Vol, ULONGLONG Block)
{
    ULONG g, bit;
    EXT_BUF Buf;
    BOOLEAN Uninit;
    ULONGLONG First;

    if (Vol->ReadOnly) return EXT_READ_ONLY;
    if (Block < Vol->FirstDataBlock || Block >= Vol->BlocksCount) return EXT_CORRUPT;
    g = (ULONG)((Block - Vol->FirstDataBlock) / Vol->BlocksPerGroup);
    First = ExtGroupFirstBlock(Vol, g);
    bit = (ULONG)((Block - First) >> Vol->ClusterBits);   /* the whole cluster goes */

    if (!NT_SUCCESS(ExtMapBlockBitmap(Vol, g, &Buf, &Uninit))) return EXT_IO_ERROR;
    if (Buf.Data[bit >> 3] & (1 << (bit & 7))) {
        Buf.Data[bit >> 3] &= (UCHAR)~(1 << (bit & 7));
        ExtBitmapCsum(Vol, g, Buf.Data, Vol->ClustersPerGroup / 8, FALSE);
        ExtSetDirty(Vol, &Buf);
        ExtHostUnmap(Vol, &Buf);
        ExtGdSetFreeBlocks(Vol, g, ExtGdFreeBlocks(Vol, g) + 1);
        ExtGroupTouched(Vol, g);
        Vol->FreeBlocksCount += 1u << Vol->ClusterBits;
    } else {
        ExtHostUnmap(Vol, &Buf);
    }
    return EXT_OK;
}

EXT_STATUS
ExtFreeBlock(PEXT_VOL Vol, ULONGLONG Block)
{
    if (Vol->ReadOnly) return EXT_READ_ONLY;
    if (Block < Vol->FirstDataBlock || Block >= Vol->BlocksCount) return EXT_CORRUPT;
    Vol->FreeTotal++;
    /* the contents are dead: never write a cached copy of them again
       (with bigalloc, the cluster's: callers free a cluster once, when the
       last of its blocks goes) */
    Block &= ~(((ULONGLONG)1 << Vol->ClusterBits) - 1);
    ExtHostFreed(Vol, ExtBlockSector(Vol, Block), Vol->SectorsPerCluster);
    if (Vol->Journal) {
        if (Vol->PendingCount == Vol->PendingCap) {
            ULONG NewCap = Vol->PendingCap ? Vol->PendingCap * 2 : 256;
            PULONGLONG N = (PULONGLONG)ExtHostAlloc(NewCap * sizeof(ULONGLONG));
            if (!N) return ExtFreeBlockNow(Vol, Block);     /* no memory: free at once */
            if (Vol->PendingFree) {
                ExtCopy(N, Vol->PendingFree, Vol->PendingCount * sizeof(ULONGLONG));
                ExtHostFree(Vol->PendingFree);
            }
            Vol->PendingFree = N;
            Vol->PendingCap = NewCap;
        }
        Vol->PendingFree[Vol->PendingCount++] = Block;
        return EXT_OK;
    }
    return ExtFreeBlockNow(Vol, Block);
}

/* Called at the start of a commit: the frees become part of that transaction */
EXT_STATUS
ExtApplyPendingFrees(PEXT_VOL Vol)
{
    ULONG i;
    EXT_STATUS S = EXT_OK;
    for (i = 0; i < Vol->PendingCount; i++) {
        EXT_STATUS R = ExtFreeBlockNow(Vol, Vol->PendingFree[i]);
        if (!NT_SUCCESS(R) && NT_SUCCESS(S)) S = R;
    }
    Vol->PendingCount = 0;
    return S;
}

EXT_STATUS
ExtAllocInode(PEXT_VOL Vol, BOOLEAN Directory, ULONG NearInode, PULONG Inode)
{
    ULONG g, Start, gi;

    if (Vol->ReadOnly) return EXT_READ_ONLY;
    if (Vol->FreeInodesCount == 0) return EXT_DISK_FULL;
    Start = (NearInode >= 1 && NearInode <= Vol->InodesCount)
            ? (NearInode - 1) / Vol->InodesPerGroup : 0;

    for (gi = 0; gi < Vol->GroupCount; gi++) {
        EXT_BUF Buf;
        ULONG bit, Max;
        g = (Start + gi) % Vol->GroupCount;
        if (ExtGdFreeInodes(Vol, g) == 0) continue;
        if (!NT_SUCCESS(ExtMapInodeBitmap(Vol, g, &Buf))) continue;
        Max = Vol->InodesPerGroup;
        for (bit = 0; bit < Max; bit++) {
            if (!(Buf.Data[bit >> 3] & (1 << (bit & 7)))) break;
        }
        if (bit >= Max) { ExtHostUnmap(Vol, &Buf); continue; }

        Buf.Data[bit >> 3] |= (UCHAR)(1 << (bit & 7));
        ExtBitmapCsum(Vol, g, Buf.Data, (Vol->InodesPerGroup + 7) / 8, TRUE);
        ExtSetDirty(Vol, &Buf);
        ExtHostUnmap(Vol, &Buf);

        ExtGdSetFreeInodes(Vol, g, ExtGdFreeInodes(Vol, g) - 1);
        if (Directory) ExtGdAddUsedDirs(Vol, g, 1);
        /* itable_unused: shrink if we went past it */
        {
            ULONG Unused = ExtGdItableUnused(Vol, g);
            ULONG FromEnd = Vol->InodesPerGroup - (bit + 1);
            if ((Vol->HasGdtCsum || Vol->HasMetaCsum) && Unused > FromEnd) {
                ExtGdSetItableUnused(Vol, g, FromEnd);
            }
        }
        ExtGroupTouched(Vol, g);
        Vol->FreeInodesCount--;
        *Inode = g * Vol->InodesPerGroup + bit + 1;
        return EXT_OK;
    }
    return EXT_DISK_FULL;
}

EXT_STATUS
ExtFreeInode(PEXT_VOL Vol, ULONG Ino, BOOLEAN Directory)
{
    ULONG g, bit;
    EXT_BUF Buf;

    if (Vol->ReadOnly) return EXT_READ_ONLY;
    if (Ino == 0 || Ino > Vol->InodesCount) return EXT_CORRUPT;
    g = (Ino - 1) / Vol->InodesPerGroup;
    bit = (Ino - 1) % Vol->InodesPerGroup;
    if (!NT_SUCCESS(ExtMapInodeBitmap(Vol, g, &Buf))) return EXT_IO_ERROR;
    if (Buf.Data[bit >> 3] & (1 << (bit & 7))) {
        Buf.Data[bit >> 3] &= (UCHAR)~(1 << (bit & 7));
        ExtBitmapCsum(Vol, g, Buf.Data, (Vol->InodesPerGroup + 7) / 8, TRUE);
        ExtSetDirty(Vol, &Buf);
        ExtHostUnmap(Vol, &Buf);
        ExtGdSetFreeInodes(Vol, g, ExtGdFreeInodes(Vol, g) + 1);
        if (Directory) ExtGdAddUsedDirs(Vol, g, -1);
        ExtGroupTouched(Vol, g);
        Vol->FreeInodesCount++;
    } else {
        ExtHostUnmap(Vol, &Buf);
    }
    return EXT_OK;
}

EXT_STATUS
ExtCountFree(PEXT_VOL Vol, PULONGLONG FreeBlocks, PULONG FreeInodes)
{
    ULONGLONG fb = 0;
    ULONG fi = 0, g;
    for (g = 0; g < Vol->GroupCount; g++) {
        fb += ExtGdFreeBlocks(Vol, g);
        fi += ExtGdFreeInodes(Vol, g);
    }
    *FreeBlocks = fb << Vol->ClusterBits;       /* descriptors count clusters */
    *FreeInodes = fi;
    return EXT_OK;
}

/* Linux's ext4_inode_is_fast_symlink: a symlink using no blocks of its own
   (an external xattr block does not count) keeps its target in i_block */
BOOLEAN
ExtIsFastSymlink(PEXT_VOL Vol, PEXT_INODE Inode)
{
    ULONGLONG Ea;
    if ((Inode->Mode & EXT_S_IFMT) != EXT_S_IFLNK) return FALSE;
    if (Inode->Flags & EXT_IF_INLINE_DATA) return FALSE;
    if (Inode->Flags & EXT_IF_EA_INODE)
        return (BOOLEAN)(ExtInodeSize(Vol, Inode) != 0 && ExtInodeSize(Vol, Inode) < 60);
    Ea = (Inode->FileAclLo || Inode->FileAclHi) ? Vol->SectorsPerCluster : 0;
    return (BOOLEAN)(ExtInodeBlocks(Vol, Inode) == Ea);
}
