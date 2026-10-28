/*
 * EXTNT - directories.
 *
 * Reading: plain directories are scanned block by block. Hash-indexed (htree)
 * directories are searched through their index (the name's hash picks the
 * one leaf block that can hold it), and enumerated linearly - an htree
 * directory's names all live in ordinary leaf blocks, and its index blocks
 * look like empty entries to a linear scan. Inline directories (inline_data:
 * a small directory kept inside its inode) are read in place.
 *
 * Writing: a directory is changed as a plain linear directory. On its first
 * change an htree directory loses its index (block 0 becomes a plain "."/".."
 * block, index nodes become empty blocks; the leaves are untouched) and an
 * inline directory is moved out to a block. Both results are what Linux and
 * e2fsck accept as ordinary directories.
 *
 * Record lengths go through RecLen()/SetRecLen() for the 64 KB block size,
 * where a length of 65536 is stored as 65535.
 */

#ifdef EXT_KERNEL
#include "ext.h"
#else
#include "extusr.h"
#include "extsup.h"
#endif

#define INLINE_SIZE         60      /* i_block */
#define INLINE_DOTDOT       4       /* parent inode number at the front */

static ULONG
RecLen(PEXT_VOL Vol, PEXT_DIR_ENTRY De)
{
    ULONG r = De->RecLen;
    if (Vol->BlockSize < 65536) return r;
    if (r == 65535 || r == 0) return 65536;
    return (r & 65532) | ((r & 3) << 16);
}

static VOID
SetRecLen(PEXT_VOL Vol, PEXT_DIR_ENTRY De, ULONG Len)
{
    if (Vol->BlockSize < 65536) { De->RecLen = (USHORT)Len; return; }
    De->RecLen = (USHORT)(Len == 65536 ? 65535 : ((Len & 65532) | ((Len >> 16) & 3)));
}

static BOOLEAN
IsCsumTail(PEXT_DIR_ENTRY De)
{
    return (BOOLEAN)(De->Inode == 0 && De->RecLen == sizeof(EXT_DIR_TAIL) &&
                     De->NameLen == 0 && De->FileType == EXT_FT_DIR_CSUM);
}

/* Where entries end in a mapped directory block: before the csum tail when
   the block has one, else at the block end. */
static ULONG
BlockEnd(PEXT_VOL Vol, PUCHAR Block)
{
    if (Vol->HasMetaCsum &&
        IsCsumTail((PEXT_DIR_ENTRY)(Block + Vol->BlockSize - sizeof(EXT_DIR_TAIL))))
        return Vol->BlockSize - (ULONG)sizeof(EXT_DIR_TAIL);
    return Vol->BlockSize;
}

/* Entries end for a block we are building */
static ULONG
NewBlockEnd(PEXT_VOL Vol)
{
    return Vol->HasMetaCsum ? (Vol->BlockSize - (ULONG)sizeof(EXT_DIR_TAIL)) : Vol->BlockSize;
}

VOID
ExtDirBlockCsum(PEXT_VOL Vol, ULONG DirIno, PEXT_INODE Dir, PUCHAR Block)
{
    PEXT_DIR_TAIL Tail;
    UCHAR Le[8];
    ULONG Crc, Gen = Dir->Generation;

    if (!Vol->HasMetaCsum) return;
    Tail = (PEXT_DIR_TAIL)(Block + Vol->BlockSize - sizeof(EXT_DIR_TAIL));
    Tail->ReservedZero1 = 0;
    Tail->RecLen = (USHORT)sizeof(EXT_DIR_TAIL);
    Tail->ReservedZero2 = 0;
    Tail->Reserved0xDE = EXT_FT_DIR_CSUM;
    Le[0] = (UCHAR)DirIno; Le[1] = (UCHAR)(DirIno >> 8); Le[2] = (UCHAR)(DirIno >> 16); Le[3] = (UCHAR)(DirIno >> 24);
    Le[4] = (UCHAR)Gen; Le[5] = (UCHAR)(Gen >> 8); Le[6] = (UCHAR)(Gen >> 16); Le[7] = (UCHAR)(Gen >> 24);
    Crc = ExtCrc32c(Vol->ChecksumSeed, Le, 4);
    Crc = ExtCrc32c(Crc, Le + 4, 4);
    Crc = ExtCrc32c(Crc, Block, Vol->BlockSize - (ULONG)sizeof(EXT_DIR_TAIL));
    Tail->Checksum = Crc;
}

static BOOLEAN
EntryValid(PEXT_VOL Vol, PEXT_DIR_ENTRY De, ULONG Offset, ULONG End)
{
    ULONG Len;
    if (Offset + EXT_DIR_HEADER > End) return FALSE;
    Len = RecLen(Vol, De);
    if (Len < EXT_DIR_HEADER || (Len & 3) != 0) return FALSE;
    if (Offset + Len > End) return FALSE;
    if (De->Inode != 0 && EXT_DIR_HEADER + (ULONG)De->NameLen > Len) return FALSE;
    return TRUE;
}

static BOOLEAN
IsDots(PEXT_DIR_ENTRY De)
{
    return (BOOLEAN)((De->NameLen == 1 && De->Name[0] == '.') ||
                     (De->NameLen == 2 && De->Name[0] == '.' && De->Name[1] == '.'));
}

static VOID
FillEntry(PEXT_VOL Vol, PEXT_ENTRY Entry, PEXT_DIR_ENTRY De, ULONGLONG Cookie)
{
    Entry->Inode = De->Inode;
    Entry->FileType = Vol->HasFiletype ? De->FileType : 0;
    Entry->NameLength = De->NameLen;
    ExtCopy(Entry->Name, De->Name, De->NameLen);
    Entry->Name[De->NameLen] = 0;
    Entry->Cookie = Cookie;
}

/* ------------------------------------------------------------------ */
/* Inline data (shared with extfile.c)                                 */
/* ------------------------------------------------------------------ */

/* Assembles an inline inode's data: i_block (60 bytes) followed by the value
   of its system.data attribute. *Data is allocated; free with ExtHostFree. */
EXT_STATUS
ExtInlineLoad(PEXT_VOL Vol, ULONG Ino, PEXT_INODE Inode, PUCHAR *Data, PULONG Len)
{
    ULONG Cap = INLINE_SIZE + Vol->InodeSize, VLen = 0;
    PUCHAR D = (PUCHAR)ExtHostAlloc(Cap);
    EXT_STATUS S;
    if (!D) return EXT_NO_MEMORY;
    ExtCopy(D, Inode->Block, INLINE_SIZE);
    S = ExtGetXattr(Vol, Ino, Inode, EXT_XATTR_INDEX_SYSTEM, "data", 4, D + INLINE_SIZE,
                    Cap - INLINE_SIZE, &VLen);
    if (S == EXT_NOT_FOUND) { VLen = 0; S = EXT_OK; }
    if (!NT_SUCCESS(S)) { ExtHostFree(D); return S; }
    *Data = D;
    *Len = INLINE_SIZE + VLen;
    return EXT_OK;
}

/* Walks an inline directory's two entry regions from byte Pos on. Calls Cb
   for each live entry; stops when Cb returns TRUE. Returns that entry's
   offset in *At, or FALSE if the walk ended. */
static BOOLEAN
InlineWalk(PEXT_VOL Vol, PUCHAR D, ULONG Len, ULONG Pos,
           BOOLEAN (*Cb)(PVOID, PEXT_DIR_ENTRY, ULONG), PVOID Ctx, PULONG At)
{
    ULONG Regions[2][2];
    int r;
    Regions[0][0] = INLINE_DOTDOT; Regions[0][1] = INLINE_SIZE;
    Regions[1][0] = INLINE_SIZE;   Regions[1][1] = Len;
    for (r = 0; r < 2; r++) {
        ULONG Off = Regions[r][0], End = Regions[r][1];
        while (Off < End) {
            PEXT_DIR_ENTRY De = (PEXT_DIR_ENTRY)(D + Off);
            ULONG L;
            if (Off + EXT_DIR_HEADER > End) break;
            L = De->RecLen;
            if (L < EXT_DIR_HEADER || (L & 3) || Off + L > End) break;
            if (De->Inode != 0 && EXT_DIR_HEADER + (ULONG)De->NameLen > L) break;
            if (Off >= Pos && De->Inode != 0 && De->NameLen && Cb(Ctx, De, Off)) { *At = Off; return TRUE; }
            Off += L;
        }
    }
    return FALSE;
}

static BOOLEAN AnyCb(PVOID Ctx, PEXT_DIR_ENTRY De, ULONG Off) { (VOID)Ctx; (VOID)De; (VOID)Off; return TRUE; }

typedef struct { const char *Name; ULONG Len; } NAME_CTX;
static BOOLEAN
NameCb(PVOID Ctx, PEXT_DIR_ENTRY De, ULONG Off)
{
    NAME_CTX *N = (NAME_CTX *)Ctx;
    (VOID)Off;
    return (BOOLEAN)(De->NameLen == N->Len && ExtMemEq(De->Name, N->Name, N->Len));
}

/* Moves an inline directory into an ordinary one-block directory */
EXT_STATUS
ExtUninlineDir(PEXT_VOL Vol, ULONG DirIno, PEXT_INODE Dir)
{
    PUCHAR D, Blk;
    ULONG Len, Pos, At, Parent, Off, End = NewBlockEnd(Vol);
    PEXT_DIR_ENTRY Last = NULL, De;
    ULONGLONG Disk;
    EXT_BUF Buf;
    EXT_STATUS S;

    S = ExtInlineLoad(Vol, DirIno, Dir, &D, &Len);
    if (!NT_SUCCESS(S)) return S;
    Blk = (PUCHAR)ExtHostAlloc(Vol->BlockSize);
    if (!Blk) { ExtHostFree(D); return EXT_NO_MEMORY; }
    ExtZero(Blk, Vol->BlockSize);
    Parent = *(PULONG)D;

    /* ".", "..", then every entry, packed */
    De = (PEXT_DIR_ENTRY)Blk;
    De->Inode = DirIno; De->NameLen = 1; De->FileType = Vol->HasFiletype ? EXT_FT_DIR : 0;
    De->Name[0] = '.'; SetRecLen(Vol, De, EXT_DIR_REC_LEN(1));
    Off = EXT_DIR_REC_LEN(1);
    De = (PEXT_DIR_ENTRY)(Blk + Off);
    De->Inode = Parent; De->NameLen = 2; De->FileType = Vol->HasFiletype ? EXT_FT_DIR : 0;
    De->Name[0] = '.'; De->Name[1] = '.'; SetRecLen(Vol, De, EXT_DIR_REC_LEN(2));
    Last = De;
    Off += EXT_DIR_REC_LEN(2);
    for (Pos = 0; InlineWalk(Vol, D, Len, Pos, AnyCb, NULL, &At); Pos = At + 1) {
        PEXT_DIR_ENTRY Src = (PEXT_DIR_ENTRY)(D + At);
        ULONG Need = EXT_DIR_REC_LEN(Src->NameLen);
        if (Off + Need > End) { ExtHostFree(D); ExtHostFree(Blk); return EXT_FILE_CORRUPT; }
        De = (PEXT_DIR_ENTRY)(Blk + Off);
        ExtCopy(De, Src, EXT_DIR_HEADER + Src->NameLen);
        SetRecLen(Vol, De, Need);
        Last = De;
        Off += Need;
    }
    SetRecLen(Vol, Last, End - (ULONG)((PUCHAR)Last - Blk));
    ExtHostFree(D);

    /* the inode becomes a normal directory: no inline flag, empty map */
    Dir->Flags &= ~EXT_IF_INLINE_DATA;
    ExtZero(Dir->Block, sizeof(Dir->Block));
    if (Vol->HasExtents) {
        PEXT_EXTENT_HEADER H = (PEXT_EXTENT_HEADER)Dir->Block;
        H->Magic = EXT_EXTENT_MAGIC; H->Max = 4;
        Dir->Flags |= EXT_IF_EXTENTS;
    }
    ExtInodeSetSize(Vol, Dir, 0);
    /* drop system.data (this also persists the inode) */
    S = ExtSetXattr(Vol, DirIno, Dir, EXT_XATTR_INDEX_SYSTEM, "data", 4, NULL, 0);
    if (!NT_SUCCESS(S)) { ExtHostFree(Blk); return S; }

    S = ExtGetFileBlock(Vol, DirIno, Dir, 0, &Disk, NULL);
    if (NT_SUCCESS(S)) S = ExtMapBlocks(Vol, Disk, 1, EXT_MAP_NEW, &Buf);
    if (!NT_SUCCESS(S)) { ExtHostFree(Blk); return S; }
    ExtCopy(Buf.Data, Blk, Vol->BlockSize);
    ExtDirBlockCsum(Vol, DirIno, Dir, Buf.Data);
    ExtSetDirty(Vol, &Buf);
    ExtHostUnmap(Vol, &Buf);
    ExtHostFree(Blk);
    ExtInodeSetSize(Vol, Dir, Vol->BlockSize);
    return ExtWriteInode(Vol, DirIno, Dir);
}

/* Sets the parent of an inline directory (the ".." it keeps in i_block) */
EXT_STATUS
ExtInlineSetParent(PEXT_VOL Vol, ULONG DirIno, PEXT_INODE Dir, ULONG Parent)
{
    Dir->Block[0] = Parent;
    return ExtWriteInode(Vol, DirIno, Dir);
}

/* ------------------------------------------------------------------ */
/* htree                                                               */
/* ------------------------------------------------------------------ */

/* dx_root: "." (12) + ".." (12 header) then the root info at 24 */
#define DX_INFO_OFF         24
#define DX_LIMIT_MAX(Vol)   (((Vol)->BlockSize - 8) / 8)

static UCHAR
DirHashVersion(PEXT_VOL Vol, UCHAR V)
{
    if (V <= EXT_DX_HASH_TEA && (Vol->Super->Flags & 0x0002 /* unsigned hash */)) V += 3;
    return V;
}

/* Finds the leaf (logical block) that may hold Name, via the index. Returns
   EXT_NOT_SUPPORTED when the index can't be used (then search linearly).
   NextHash and NextLeaf describe the following leaf for hash collisions
   (*NextLeaf = ~0 when none). */
static EXT_STATUS
DxFindLeaf(PEXT_VOL Vol, ULONG DirIno, PEXT_INODE Dir, const char *Name, ULONG NameLen,
           PULONG Leaf, PULONG Hash, PULONG NextHash, PULONG NextLeaf)
{
    ULONGLONG Disk;
    ULONG Run, Levels, Level, Block = 0, H, Minor;
    UCHAR Version, InfoLen;
    EXT_BUF Buf;
    EXT_STATUS S;

    *NextLeaf = 0xFFFFFFFF;
    S = ExtMapFileBlock(Vol, DirIno, Dir, 0, &Disk, &Run);
    if (!NT_SUCCESS(S)) return S;
    if (Disk == 0) return EXT_NOT_SUPPORTED;
    S = ExtMapBlocks(Vol, Disk, 1, EXT_MAP_READ, &Buf);
    if (!NT_SUCCESS(S)) return S;
    Version = Buf.Data[DX_INFO_OFF + 4];
    InfoLen = Buf.Data[DX_INFO_OFF + 5];
    Levels = Buf.Data[DX_INFO_OFF + 6];
    if (*(PULONG)(Buf.Data + DX_INFO_OFF) != 0 || InfoLen != 8 || Version > EXT_DX_HASH_TEA ||
        Levels > 2 || Buf.Data[DX_INFO_OFF + 7] != 0) {
        ExtHostUnmap(Vol, &Buf);
        return EXT_NOT_SUPPORTED;
    }
    H = ExtNameHash(Vol, DirHashVersion(Vol, Version), Name, NameLen, &Minor);
    *Hash = H;

    {
        ULONG Off = DX_INFO_OFF + InfoLen;
        for (Level = 0; ; Level++) {
            USHORT Limit = *(PUSHORT)(Buf.Data + Off), Count = *(PUSHORT)(Buf.Data + Off + 2);
            PULONG E = (PULONG)(Buf.Data + Off);      /* entry i: hash E[2i], block E[2i+1]; E[0] = countlimit */
            ULONG Lo = 1, Hi, Pick;
            if (Count == 0 || Count > Limit || Limit > DX_LIMIT_MAX(Vol) ||
                Off + (ULONG)Limit * 8 > Vol->BlockSize) {
                ExtHostUnmap(Vol, &Buf);
                return EXT_NOT_SUPPORTED;
            }
            /* last entry with hash <= H (entry 0 has an implicit hash of 0) */
            Hi = Count - 1;
            Pick = 0;
            while (Lo <= Hi) {
                ULONG Mid = (Lo + Hi) / 2;
                if (E[2 * Mid] > H) Hi = Mid - 1; else { Pick = Mid; Lo = Mid + 1; }
            }
            Block = E[2 * Pick + 1] & 0x0FFFFFFF;
            if (Pick + 1 < Count) { *NextHash = E[2 * (Pick + 1)]; *NextLeaf = E[2 * (Pick + 1) + 1] & 0x0FFFFFFF; }
            if (Level == Levels) break;
            ExtHostUnmap(Vol, &Buf);
            S = ExtMapFileBlock(Vol, DirIno, Dir, Block, &Disk, &Run);
            if (!NT_SUCCESS(S)) return S;
            if (Disk == 0) return EXT_NOT_SUPPORTED;
            S = ExtMapBlocks(Vol, Disk, 1, EXT_MAP_READ, &Buf);
            if (!NT_SUCCESS(S)) return S;
            Off = 8;            /* fake empty entry, then countlimit */
        }
    }
    ExtHostUnmap(Vol, &Buf);
    *Leaf = Block;
    return EXT_OK;
}

/* ------------------------------------------------------------------ */
/* Enumeration and lookup                                              */
/* ------------------------------------------------------------------ */

EXT_STATUS
ExtEnum(PEXT_VOL Vol, ULONG DirIno, PEXT_INODE Dir, ULONGLONG Cookie, PEXT_ENTRY Entry)
{
    if (Vol->OldExt) return ExtOldEnum(Vol, DirIno, Dir, Cookie, Entry);
    ULONGLONG Size = ExtInodeSize(Vol, Dir);
    ULONGLONG Pos = Cookie;

    if (Dir->Flags & EXT_IF_ENCRYPT) return EXT_ACCESS_DENIED;     /* names are ciphertext */
    if (Dir->Flags & EXT_IF_INLINE_DATA) {
        PUCHAR D; ULONG Len, At;
        EXT_STATUS S = ExtInlineLoad(Vol, DirIno, Dir, &D, &Len);
        if (!NT_SUCCESS(S)) return S;
        if (Pos > Len || !InlineWalk(Vol, D, Len, (ULONG)Pos, AnyCb, NULL, &At)) {
            ExtHostFree(D);
            return EXT_NO_MORE_ENTRIES;
        }
        FillEntry(Vol, Entry, (PEXT_DIR_ENTRY)(D + At), At + 1);
        ExtHostFree(D);
        return EXT_OK;
    }

    while (Pos < Size) {
        ULONGLONG FileBlk = Pos >> Vol->BlockShift;
        ULONG InBlk = (ULONG)(Pos & (Vol->BlockSize - 1)), End, Off;
        ULONGLONG Disk;
        ULONG Run;
        EXT_BUF Buf;
        EXT_STATUS S;

        S = ExtMapFileBlock(Vol, DirIno, Dir, FileBlk, &Disk, &Run);
        if (!NT_SUCCESS(S)) return S;
        if (Disk == 0) { Pos = (FileBlk + 1) << Vol->BlockShift; continue; }
        S = ExtMapBlocks(Vol, Disk, 1, EXT_MAP_READ, &Buf);
        if (!NT_SUCCESS(S)) return S;
        End = BlockEnd(Vol, Buf.Data);
        /* walk from the block start so a cookie can't land mid-entry */
        for (Off = 0; Off < End; ) {
            PEXT_DIR_ENTRY De = (PEXT_DIR_ENTRY)(Buf.Data + Off);
            if (!EntryValid(Vol, De, Off, End)) break;
            if (Off >= InBlk && De->Inode != 0 && De->NameLen != 0 && !IsDots(De)) {
                FillEntry(Vol, Entry, De, ((FileBlk << Vol->BlockShift) | Off) + RecLen(Vol, De));
                ExtHostUnmap(Vol, &Buf);
                return EXT_OK;
            }
            Off += RecLen(Vol, De);
        }
        ExtHostUnmap(Vol, &Buf);
        Pos = (FileBlk + 1) << Vol->BlockShift;
    }
    return EXT_NO_MORE_ENTRIES;
}

/* Searches one directory block for Name */
static EXT_STATUS
SearchBlock(PEXT_VOL Vol, ULONG DirIno, PEXT_INODE Dir, ULONGLONG FileBlk,
            const char *Name, ULONG NameLen, PEXT_ENTRY Entry)
{
    ULONGLONG Disk;
    ULONG Run, Off, End;
    EXT_BUF Buf;
    EXT_STATUS S = ExtMapFileBlock(Vol, DirIno, Dir, FileBlk, &Disk, &Run);
    if (!NT_SUCCESS(S)) return S;
    if (Disk == 0) return EXT_NOT_FOUND;
    S = ExtMapBlocks(Vol, Disk, 1, EXT_MAP_READ, &Buf);
    if (!NT_SUCCESS(S)) return S;
    End = BlockEnd(Vol, Buf.Data);
    for (Off = 0; Off < End; ) {
        PEXT_DIR_ENTRY De = (PEXT_DIR_ENTRY)(Buf.Data + Off);
        if (!EntryValid(Vol, De, Off, End)) break;
        if (De->Inode != 0 && De->NameLen == NameLen && ExtMemEq(De->Name, Name, NameLen)) {
            FillEntry(Vol, Entry, De, (FileBlk << Vol->BlockShift) + Off);
            ExtHostUnmap(Vol, &Buf);
            return EXT_OK;
        }
        Off += RecLen(Vol, De);
    }
    ExtHostUnmap(Vol, &Buf);
    return EXT_NOT_FOUND;
}

EXT_STATUS
ExtLookup(PEXT_VOL Vol, ULONG DirIno, PEXT_INODE Dir, const char *Name, ULONG NameLen, PEXT_ENTRY Entry)
{
    if (Vol->OldExt) return ExtOldLookup(Vol, DirIno, Dir, Name, NameLen, Entry);
    ULONGLONG Size = ExtInodeSize(Vol, Dir);
    ULONGLONG FileBlk, Blocks = (Size + Vol->BlockSize - 1) >> Vol->BlockShift;
    EXT_STATUS S;

    if (NameLen == 0 || NameLen > EXT_MAX_NAME) return EXT_BAD_NAME;
    if (Dir->Flags & EXT_IF_ENCRYPT) return EXT_ACCESS_DENIED;

    if (Dir->Flags & EXT_IF_INLINE_DATA) {
        PUCHAR D; ULONG Len, At;
        NAME_CTX N;
        S = ExtInlineLoad(Vol, DirIno, Dir, &D, &Len);
        if (!NT_SUCCESS(S)) return S;
        N.Name = Name; N.Len = NameLen;
        if (InlineWalk(Vol, D, Len, 0, NameCb, &N, &At)) {
            FillEntry(Vol, Entry, (PEXT_DIR_ENTRY)(D + At), At);
            S = EXT_OK;
        } else {
            S = EXT_NOT_FOUND;
        }
        ExtHostFree(D);
        return S;
    }

    /* a case-folded directory hashes the folded name: search it linearly */
    if ((Dir->Flags & EXT_IF_INDEX) && Vol->HasHtree && !(Dir->Flags & EXT_IF_CASEFOLD)) {
        ULONG Leaf, Hash, NextHash = 0, NextLeaf;
        S = DxFindLeaf(Vol, DirIno, Dir, Name, NameLen, &Leaf, &Hash, &NextHash, &NextLeaf);
        if (NT_SUCCESS(S)) {
            S = SearchBlock(Vol, DirIno, Dir, Leaf, Name, NameLen, Entry);
            if (S != EXT_NOT_FOUND) return S;
            /* a run of names with one hash may spill into the next leaf, which
               is then marked by bit 0 of its starting hash: rare, so just
               fall back to the linear search below */
            if (NextLeaf == 0xFFFFFFFF || (NextHash & ~1U) != Hash || !(NextHash & 1))
                return EXT_NOT_FOUND;
            S = EXT_NOT_SUPPORTED;
        }
        if (S != EXT_NOT_SUPPORTED) return S;
        /* fall through: unusable index, search linearly */
    }

    for (FileBlk = 0; FileBlk < Blocks; FileBlk++) {
        S = SearchBlock(Vol, DirIno, Dir, FileBlk, Name, NameLen, Entry);
        if (S != EXT_NOT_FOUND) return S;
    }
    return EXT_NOT_FOUND;
}

EXT_STATUS
ExtDirIsEmpty(PEXT_VOL Vol, ULONG DirIno, PEXT_INODE Dir, PBOOLEAN Empty)
{
    EXT_ENTRY E;
    EXT_STATUS S = ExtEnum(Vol, DirIno, Dir, 0, &E);
    if (S == EXT_NO_MORE_ENTRIES) { *Empty = TRUE; return EXT_OK; }
    if (NT_SUCCESS(S)) { *Empty = FALSE; return EXT_OK; }
    return S;
}

/* ------------------------------------------------------------------ */
/* Dropping the htree index                                            */
/* ------------------------------------------------------------------ */

/* Rewrites a directory block as one empty entry (plus the csum tail) */
static EXT_STATUS
EmptyDirBlock(PEXT_VOL Vol, ULONG DirIno, PEXT_INODE Dir, ULONG FileBlk)
{
    ULONGLONG Disk;
    ULONG Run;
    EXT_BUF Buf;
    EXT_STATUS S = ExtMapFileBlock(Vol, DirIno, Dir, FileBlk, &Disk, &Run);
    if (!NT_SUCCESS(S) || Disk == 0) return S;
    S = ExtMapBlocks(Vol, Disk, 1, EXT_MAP_NEW, &Buf);
    if (!NT_SUCCESS(S)) return S;
    SetRecLen(Vol, (PEXT_DIR_ENTRY)Buf.Data, NewBlockEnd(Vol));
    ExtDirBlockCsum(Vol, DirIno, Dir, Buf.Data);
    ExtSetDirty(Vol, &Buf);
    ExtHostUnmap(Vol, &Buf);
    return EXT_OK;
}

/* Collects the index node blocks below a dx node (Levels left above leaves) */
static EXT_STATUS
DxCollect(PEXT_VOL Vol, ULONG DirIno, PEXT_INODE Dir, PUCHAR Node, ULONG Off, ULONG Levels,
          PULONG List, PULONG Count, ULONG Max)
{
    USHORT Limit = *(PUSHORT)(Node + Off), Cnt = *(PUSHORT)(Node + Off + 2);
    PULONG E = (PULONG)(Node + Off);
    ULONG i;
    if (Levels == 0) return EXT_OK;
    if (Cnt > Limit || Off + (ULONG)Cnt * 8 > Vol->BlockSize) return EXT_FILE_CORRUPT;
    for (i = 0; i < Cnt; i++) {
        ULONG Child = E[2 * i + 1] & 0x0FFFFFFF;
        ULONGLONG Disk;
        ULONG Run;
        EXT_BUF Buf;
        EXT_STATUS S;
        if (*Count >= Max) return EXT_FILE_CORRUPT;
        List[(*Count)++] = Child;
        if (Levels > 1) {
            S = ExtMapFileBlock(Vol, DirIno, Dir, Child, &Disk, &Run);
            if (!NT_SUCCESS(S)) return S;
            if (Disk == 0) continue;
            S = ExtMapBlocks(Vol, Disk, 1, EXT_MAP_READ, &Buf);
            if (!NT_SUCCESS(S)) return S;
            S = DxCollect(Vol, DirIno, Dir, Buf.Data, 8, Levels - 1, List, Count, Max);
            ExtHostUnmap(Vol, &Buf);
            if (!NT_SUCCESS(S)) return S;
        }
    }
    return EXT_OK;
}

static EXT_STATUS
Deindex(PEXT_VOL Vol, ULONG DirIno, PEXT_INODE Dir)
{
    ULONGLONG Disk;
    ULONG Run, DotIno, ParentIno, Off, End = NewBlockEnd(Vol);
    ULONG *Nodes = NULL, NodeCount = 0, Max, i;
    EXT_BUF Buf;
    EXT_STATUS S;
    PEXT_DIR_ENTRY Dot, DotDot;

    if (!(Dir->Flags & EXT_IF_INDEX)) return EXT_OK;

    S = ExtMapFileBlock(Vol, DirIno, Dir, 0, &Disk, &Run);
    if (!NT_SUCCESS(S)) return S;
    if (Disk == 0) return EXT_FILE_CORRUPT;
    S = ExtMapBlocks(Vol, Disk, 1, EXT_MAP_WRITE, &Buf);
    if (!NT_SUCCESS(S)) return S;

    /* index nodes below the root (2- and 3-level trees) become empty blocks */
    {
        ULONG Levels = Buf.Data[DX_INFO_OFF + 6], InfoLen = Buf.Data[DX_INFO_OFF + 5];
        if (Levels > 0 && Levels <= 2 && InfoLen == 8) {
            Max = (ULONG)((ExtInodeSize(Vol, Dir) >> Vol->BlockShift) + 1);
            Nodes = (ULONG *)ExtHostAlloc(Max * sizeof(ULONG));
            if (!Nodes) { ExtHostUnmap(Vol, &Buf); return EXT_NO_MEMORY; }
            S = DxCollect(Vol, DirIno, Dir, Buf.Data, DX_INFO_OFF + InfoLen, Levels, Nodes, &NodeCount, Max);
            if (!NT_SUCCESS(S)) NodeCount = 0;      /* corrupt index: leave nodes; fsck sees empty-ish blocks */
        }
    }

    Dot = (PEXT_DIR_ENTRY)Buf.Data;
    DotIno = Dot->Inode ? Dot->Inode : DirIno;
    DotDot = (PEXT_DIR_ENTRY)(Buf.Data + EXT_DIR_REC_LEN(1));
    ParentIno = (DotDot->NameLen == 2) ? DotDot->Inode : DirIno;

    ExtZero(Buf.Data, Vol->BlockSize);
    Dot = (PEXT_DIR_ENTRY)Buf.Data;
    Dot->Inode = DotIno; Dot->NameLen = 1; Dot->FileType = Vol->HasFiletype ? EXT_FT_DIR : 0;
    Dot->Name[0] = '.'; SetRecLen(Vol, Dot, EXT_DIR_REC_LEN(1));
    Off = EXT_DIR_REC_LEN(1);
    DotDot = (PEXT_DIR_ENTRY)(Buf.Data + Off);
    DotDot->Inode = ParentIno; DotDot->NameLen = 2; DotDot->FileType = Vol->HasFiletype ? EXT_FT_DIR : 0;
    DotDot->Name[0] = '.'; DotDot->Name[1] = '.'; SetRecLen(Vol, DotDot, End - Off);
    ExtDirBlockCsum(Vol, DirIno, Dir, Buf.Data);
    ExtSetDirty(Vol, &Buf);
    ExtHostUnmap(Vol, &Buf);

    for (i = 0; i < NodeCount; i++) EmptyDirBlock(Vol, DirIno, Dir, Nodes[i]);
    if (Nodes) ExtHostFree(Nodes);

    Dir->Flags &= ~EXT_IF_INDEX;
    return ExtWriteInode(Vol, DirIno, Dir);
}

/* Make a directory plain and block-based before changing it */
static EXT_STATUS
PrepareForChange(PEXT_VOL Vol, ULONG DirIno, PEXT_INODE Dir)
{
    if (Vol->ReadOnly) return EXT_READ_ONLY;
    if (Dir->Flags & EXT_IF_INLINE_DATA) {
        EXT_STATUS S = ExtUninlineDir(Vol, DirIno, Dir);
        if (!NT_SUCCESS(S)) return S;
    }
    if (Dir->Flags & EXT_IF_INDEX) return Deindex(Vol, DirIno, Dir);
    return EXT_OK;
}

/* ------------------------------------------------------------------ */
/* Insert / remove                                                     */
/* ------------------------------------------------------------------ */

EXT_STATUS
ExtAddEntry(PEXT_VOL Vol, ULONG DirIno, PEXT_INODE Dir, const char *Name, ULONG NameLen,
            ULONG Inode, UCHAR FileType)
{
    ULONGLONG Size, FileBlk, Blocks, Disk;
    ULONG Need = EXT_DIR_REC_LEN(NameLen), Run;
    EXT_STATUS S;

    if (NameLen == 0 || NameLen > EXT_MAX_NAME) return EXT_BAD_NAME;
    S = PrepareForChange(Vol, DirIno, Dir);
    if (!NT_SUCCESS(S)) return S;

    Size = ExtInodeSize(Vol, Dir);
    Blocks = (Size + Vol->BlockSize - 1) >> Vol->BlockShift;

    for (FileBlk = 0; FileBlk < Blocks; FileBlk++) {
        EXT_BUF Buf;
        ULONG Off, End;
        S = ExtMapFileBlock(Vol, DirIno, Dir, FileBlk, &Disk, &Run);
        if (!NT_SUCCESS(S)) return S;
        if (Disk == 0) continue;
        S = ExtMapBlocks(Vol, Disk, 1, EXT_MAP_WRITE, &Buf);
        if (!NT_SUCCESS(S)) return S;
        End = BlockEnd(Vol, Buf.Data);
        if (Vol->HasMetaCsum && End == Vol->BlockSize) {   /* no csum tail: leave this block alone */
            ExtHostUnmap(Vol, &Buf);
            continue;
        }
        for (Off = 0; Off < End; ) {
            PEXT_DIR_ENTRY De = (PEXT_DIR_ENTRY)(Buf.Data + Off);
            ULONG L, Actual;
            if (!EntryValid(Vol, De, Off, End)) break;
            L = RecLen(Vol, De);
            Actual = (De->Inode == 0) ? 0 : EXT_DIR_REC_LEN(De->NameLen);
            if (De->Inode != 0 && De->NameLen == NameLen && ExtMemEq(De->Name, Name, NameLen)) {
                ExtHostUnmap(Vol, &Buf);
                return EXT_EXISTS;
            }
            if (L - Actual >= Need) {
                PEXT_DIR_ENTRY New = De;
                if (De->Inode != 0) {
                    SetRecLen(Vol, De, Actual);
                    New = (PEXT_DIR_ENTRY)(Buf.Data + Off + Actual);
                    SetRecLen(Vol, New, L - Actual);
                }
                New->Inode = Inode;
                New->NameLen = (UCHAR)NameLen;
                New->FileType = Vol->HasFiletype ? FileType : 0;
                ExtCopy(New->Name, Name, NameLen);
                ExtDirBlockCsum(Vol, DirIno, Dir, Buf.Data);
                ExtSetDirty(Vol, &Buf);
                ExtHostUnmap(Vol, &Buf);
                return EXT_OK;
            }
            Off += L;
        }
        ExtHostUnmap(Vol, &Buf);
    }

    /* no room: append a block */
    {
        EXT_BUF Buf;
        PEXT_DIR_ENTRY De;
        FileBlk = Blocks;
        S = ExtGetFileBlock(Vol, DirIno, Dir, FileBlk, &Disk, NULL);
        if (!NT_SUCCESS(S)) return S;
        S = ExtMapBlocks(Vol, Disk, 1, EXT_MAP_NEW, &Buf);
        if (!NT_SUCCESS(S)) return S;
        De = (PEXT_DIR_ENTRY)Buf.Data;
        De->Inode = Inode;
        SetRecLen(Vol, De, NewBlockEnd(Vol));
        De->NameLen = (UCHAR)NameLen;
        De->FileType = Vol->HasFiletype ? FileType : 0;
        ExtCopy(De->Name, Name, NameLen);
        ExtDirBlockCsum(Vol, DirIno, Dir, Buf.Data);
        ExtSetDirty(Vol, &Buf);
        ExtHostUnmap(Vol, &Buf);
        ExtInodeSetSize(Vol, Dir, (FileBlk + 1) << Vol->BlockShift);
        return ExtWriteInode(Vol, DirIno, Dir);
    }
}

EXT_STATUS
ExtRemoveEntry(PEXT_VOL Vol, ULONG DirIno, PEXT_INODE Dir, const char *Name, ULONG NameLen)
{
    ULONGLONG Size, FileBlk, Blocks, Disk;
    ULONG Run;
    EXT_STATUS S = PrepareForChange(Vol, DirIno, Dir);
    if (!NT_SUCCESS(S)) return S;

    Size = ExtInodeSize(Vol, Dir);
    Blocks = (Size + Vol->BlockSize - 1) >> Vol->BlockShift;

    for (FileBlk = 0; FileBlk < Blocks; FileBlk++) {
        EXT_BUF Buf;
        ULONG Off, Prev = (ULONG)-1, End;
        S = ExtMapFileBlock(Vol, DirIno, Dir, FileBlk, &Disk, &Run);
        if (!NT_SUCCESS(S)) return S;
        if (Disk == 0) continue;
        S = ExtMapBlocks(Vol, Disk, 1, EXT_MAP_WRITE, &Buf);
        if (!NT_SUCCESS(S)) return S;
        End = BlockEnd(Vol, Buf.Data);
        for (Off = 0; Off < End; ) {
            PEXT_DIR_ENTRY De = (PEXT_DIR_ENTRY)(Buf.Data + Off);
            if (!EntryValid(Vol, De, Off, End)) break;
            if (De->Inode != 0 && De->NameLen == NameLen && ExtMemEq(De->Name, Name, NameLen)) {
                if (Prev != (ULONG)-1) {
                    PEXT_DIR_ENTRY P = (PEXT_DIR_ENTRY)(Buf.Data + Prev);
                    SetRecLen(Vol, P, RecLen(Vol, P) + RecLen(Vol, De));
                } else {
                    De->Inode = 0;
                }
                ExtDirBlockCsum(Vol, DirIno, Dir, Buf.Data);
                ExtSetDirty(Vol, &Buf);
                ExtHostUnmap(Vol, &Buf);
                return EXT_OK;
            }
            Prev = Off;
            Off += RecLen(Vol, De);
        }
        ExtHostUnmap(Vol, &Buf);
    }
    return EXT_NOT_FOUND;
}

/* Points the ".." of directory DirIno at NewParent (after a move) */
EXT_STATUS
ExtSetDotDot(PEXT_VOL Vol, ULONG DirIno, PEXT_INODE Dir, ULONG NewParent)
{
    ULONGLONG Disk;
    ULONG Run;
    EXT_BUF Buf;
    EXT_STATUS S;
    if (Dir->Flags & EXT_IF_INLINE_DATA) return ExtInlineSetParent(Vol, DirIno, Dir, NewParent);
    /* an htree root's dx checksum covers "..": drop the index first */
    S = Deindex(Vol, DirIno, Dir);
    if (!NT_SUCCESS(S)) return S;
    S = ExtMapFileBlock(Vol, DirIno, Dir, 0, &Disk, &Run);
    if (!NT_SUCCESS(S)) return S;
    if (Disk == 0) return EXT_FILE_CORRUPT;
    S = ExtMapBlocks(Vol, Disk, 1, EXT_MAP_WRITE, &Buf);
    if (!NT_SUCCESS(S)) return S;
    {
        PEXT_DIR_ENTRY Dot = (PEXT_DIR_ENTRY)Buf.Data;
        PEXT_DIR_ENTRY DotDot = (PEXT_DIR_ENTRY)(Buf.Data + RecLen(Vol, Dot));
        if (RecLen(Vol, Dot) < Vol->BlockSize && DotDot->NameLen == 2 && DotDot->Name[0] == '.') {
            DotDot->Inode = NewParent;
            ExtDirBlockCsum(Vol, DirIno, Dir, Buf.Data);
            ExtSetDirty(Vol, &Buf);
        }
    }
    ExtHostUnmap(Vol, &Buf);
    return EXT_OK;
}

/* The parent of a directory (its ".."), for walking a link target */
EXT_STATUS
ExtGetParent(PEXT_VOL Vol, ULONG DirIno, PEXT_INODE Dir, PULONG Parent)
{
    ULONGLONG Disk;
    ULONG Run;
    EXT_BUF Buf;
    EXT_STATUS S;

    if (DirIno == EXT_ROOT_INO) { *Parent = EXT_ROOT_INO; return EXT_OK; }
    if (Dir->Flags & EXT_IF_INLINE_DATA) { *Parent = Dir->Block[0]; return EXT_OK; }
    S = ExtMapFileBlock(Vol, DirIno, Dir, 0, &Disk, &Run);
    if (!NT_SUCCESS(S)) return S;
    if (Disk == 0) return EXT_FILE_CORRUPT;
    S = ExtMapBlocks(Vol, Disk, 1, EXT_MAP_READ, &Buf);
    if (!NT_SUCCESS(S)) return S;
    {
        PEXT_DIR_ENTRY Dot = (PEXT_DIR_ENTRY)Buf.Data;
        PEXT_DIR_ENTRY DotDot = (PEXT_DIR_ENTRY)(Buf.Data + RecLen(Vol, Dot));
        if (RecLen(Vol, Dot) >= Vol->BlockSize || DotDot->NameLen != 2 || DotDot->Name[0] != '.') {
            S = EXT_FILE_CORRUPT;
        } else {
            *Parent = DotDot->Inode;
        }
    }
    ExtHostUnmap(Vol, &Buf);
    return S;
}
