/*
 * EXTNT - extended attributes.
 *
 * Reading merges the in-inode (ibody) attributes and those in the external
 * xattr block; values kept in their own inode (ea_inode) are read from it.
 *
 * Writing places every attribute of the inode in an external block and clears
 * the in-inode area - a layout Linux and e2fsck accept. Linux shares identical
 * xattr blocks between inodes (h_refcount > 1), so a shared block is never
 * changed in place: the inode gets its own copy and the shared one loses a
 * reference. Deleting an inode drops its block and ea_inode references,
 * freeing them at zero. EXTNT does not create ea_inode values; an inode that
 * has them can be read but its attributes are not rewritten.
 *
 * The NT driver maps NT EAs to and from these (user.* namespace).
 */

#ifdef EXT_KERNEL
#include "ext.h"
#else
#include "extusr.h"
#include "extsup.h"
#endif

#define IS_LAST(e)          (*(const ULONG *)(e) == 0)
#define NEXT_ENTRY(e)       ((PEXT_XATTR_ENTRY)((PUCHAR)(e) + EXT_XATTR_ENTRY_SIZE((e)->NameLen)))
#define MAX_EA_VALUE        65536

#define SET_MAX_ENTRIES     128
#define SET_ARENA           65536

typedef struct _XSET {
    ULONG   Count;
    BOOLEAN HasEaInode;             /* some value lives in its own inode */
    UCHAR   Index[SET_MAX_ENTRIES];
    UCHAR   NameLen[SET_MAX_ENTRIES];
    char    Name[SET_MAX_ENTRIES][256];
    ULONG   ValueOff[SET_MAX_ENTRIES];
    ULONG   ValueLen[SET_MAX_ENTRIES];
    ULONG   ArenaUsed;
    UCHAR   Arena[SET_ARENA];
} XSET, *PXSET;

/* Callback per attribute: Val is NULL only when it could not be read */
typedef VOID (*XATTR_CB)(PVOID Ctx, PEXT_XATTR_ENTRY E, const UCHAR *Val, ULONG ValLen);

static ULONGLONG
XattrBlockOf(PEXT_VOL Vol, PEXT_INODE Inode)
{
    ULONGLONG B = Inode->FileAclLo;
    if (Vol->Has64Bit) B |= (ULONGLONG)Inode->FileAclHi << 32;
    return B;
}

static VOID
SetXattrBlock(PEXT_VOL Vol, PEXT_INODE Inode, ULONGLONG B)
{
    Inode->FileAclLo = (ULONG)B;
    if (Vol->Has64Bit) Inode->FileAclHi = (USHORT)(B >> 32);
}

/* ------------------------------------------------------------------ */
/* Inode images                                                        */
/* ------------------------------------------------------------------ */

static EXT_STATUS
ReadInodeImage(PEXT_VOL Vol, ULONG Ino, PUCHAR Image)
{
    ULONGLONG Sector;
    ULONG Offset, Need;
    EXT_BUF Buf;
    EXT_STATUS S = ExtInodeLocation(Vol, Ino, &Sector, &Offset);
    if (!NT_SUCCESS(S)) return S;
    Need = (Offset + Vol->InodeSize + 511) / 512;
    S = ExtHostMap(Vol, Sector, Need, EXT_MAP_READ, &Buf);
    if (!NT_SUCCESS(S)) return S;
    ExtCopy(Image, Buf.Data + Offset, Vol->InodeSize);
    ExtHostUnmap(Vol, &Buf);
    return EXT_OK;
}

static EXT_STATUS
WriteInodeImage(PEXT_VOL Vol, ULONG Ino, PUCHAR Image)
{
    ULONGLONG Sector;
    ULONG Offset, Need;
    EXT_BUF Buf;
    EXT_STATUS S;
    if (Vol->ReadOnly) return EXT_READ_ONLY;
    S = ExtInodeLocation(Vol, Ino, &Sector, &Offset);
    if (!NT_SUCCESS(S)) return S;
    Need = (Offset + Vol->InodeSize + 511) / 512;
    S = ExtHostMap(Vol, Sector, Need, EXT_MAP_WRITE, &Buf);
    if (!NT_SUCCESS(S)) return S;
    ExtCopy(Buf.Data + Offset, Image, Vol->InodeSize);
    ExtInodeCsumImage(Vol, Ino, Buf.Data + Offset);
    ExtSetDirty(Vol, &Buf);
    ExtHostUnmap(Vol, &Buf);
    return EXT_OK;
}

/* ibody attributes begin at 128 + i_extra_isize, after a 4-byte magic */
static PUCHAR
IbodyHeader(PEXT_VOL Vol, PUCHAR Image)
{
    USHORT Extra;
    if (Vol->InodeSize <= EXT_GOOD_OLD_INODE_SIZE) return NULL;
    Extra = *(USHORT *)(Image + 0x80);
    if (Extra == 0 || (Extra & 3) || EXT_GOOD_OLD_INODE_SIZE + Extra + 4 > Vol->InodeSize) return NULL;
    return Image + EXT_GOOD_OLD_INODE_SIZE + Extra;
}

/* ------------------------------------------------------------------ */
/* Walking                                                             */
/* ------------------------------------------------------------------ */

/* Reads a value kept in its own inode */
static PUCHAR
ReadEaInode(PEXT_VOL Vol, ULONG EaIno, ULONG Size)
{
    EXT_INODE Ea;
    PUCHAR V;
    ULONG Got;
    if (Size > MAX_EA_VALUE || EaIno < EXT_FIRST_NONRES_INO || EaIno > Vol->InodesCount) return NULL;
    if (!NT_SUCCESS(ExtReadInode(Vol, EaIno, &Ea)) || !(Ea.Flags & EXT_IF_EA_INODE)) return NULL;
    V = (PUCHAR)ExtHostAlloc(Size ? Size : 1);
    if (!V) return NULL;
    if (!NT_SUCCESS(ExtReadFile(Vol, EaIno, &Ea, 0, Size, V, &Got)) || Got != Size) { ExtHostFree(V); return NULL; }
    return V;
}

/* Entries start at EntriesStart; e_value_offs counts from ValueBase (ibody:
   the entries start; block: the block start); everything ends before End */
static VOID
Walk(PEXT_VOL Vol, PUCHAR EntriesStart, PUCHAR ValueBase, PUCHAR End, XATTR_CB Cb, PVOID Ctx)
{
    PEXT_XATTR_ENTRY E = (PEXT_XATTR_ENTRY)EntriesStart;
    while ((PUCHAR)E + 4 <= End && !IS_LAST(E)) {
        PUCHAR Next = (PUCHAR)NEXT_ENTRY(E);
        if ((PUCHAR)E + sizeof(EXT_XATTR_ENTRY) > End || Next > End || E->NameLen == 0) break;
        if (E->ValueInum) {
            PUCHAR V = ReadEaInode(Vol, E->ValueInum, E->ValueSize);
            Cb(Ctx, E, V, E->ValueSize);
            if (V) ExtHostFree(V);
        } else if (E->ValueSize == 0) {
            Cb(Ctx, E, (const UCHAR *)"", 0);
        } else if (ValueBase + E->ValueOffs + E->ValueSize <= End && E->ValueSize <= MAX_EA_VALUE) {
            Cb(Ctx, E, ValueBase + E->ValueOffs, E->ValueSize);
        }
        E = (PEXT_XATTR_ENTRY)Next;
    }
}

/* Every attribute of the inode: ibody first, then the external block */
static EXT_STATUS
ForEach(PEXT_VOL Vol, ULONG Ino, PEXT_INODE Inode, XATTR_CB Cb, PVOID Ctx)
{
    PUCHAR Image = (PUCHAR)ExtHostAlloc(Vol->InodeSize);
    ULONGLONG Blk;
    EXT_STATUS S;

    if (!Image) return EXT_NO_MEMORY;
    S = ReadInodeImage(Vol, Ino, Image);
    if (NT_SUCCESS(S)) {
        PUCHAR H = IbodyHeader(Vol, Image);
        if (H && *(PULONG)H == EXT_XATTR_MAGIC)
            Walk(Vol, H + 4, H + 4, Image + Vol->InodeSize, Cb, Ctx);
    }
    ExtHostFree(Image);
    if (!NT_SUCCESS(S)) return S;

    Blk = XattrBlockOf(Vol, Inode);
    if (Blk) {
        EXT_BUF Buf;
        if (Blk <= Vol->FirstDataBlock || Blk >= Vol->BlocksCount) return EXT_EA_CORRUPT;
        S = ExtMapBlocks(Vol, Blk, 1, EXT_MAP_READ, &Buf);
        if (!NT_SUCCESS(S)) return S;
        if (((PEXT_XATTR_HEADER)Buf.Data)->Magic == EXT_XATTR_MAGIC)
            Walk(Vol, Buf.Data + sizeof(EXT_XATTR_HEADER), Buf.Data, Buf.Data + Vol->BlockSize, Cb, Ctx);
        ExtHostUnmap(Vol, &Buf);
    }
    return EXT_OK;
}

/* ------------------------------------------------------------------ */
/* List / get                                                          */
/* ------------------------------------------------------------------ */

typedef struct {
    VOID (*Report)(PVOID, UCHAR, const char *, ULONG, const UCHAR *, ULONG);
    PVOID Ctx;
} LIST_CTX;

static VOID
ListCb(PVOID C, PEXT_XATTR_ENTRY E, const UCHAR *Val, ULONG ValLen)
{
    LIST_CTX *L = (LIST_CTX *)C;
    L->Report(L->Ctx, E->NameIndex, E->Name, E->NameLen, Val, Val ? ValLen : 0);
}

EXT_STATUS
ExtListXattr(PEXT_VOL Vol, ULONG Ino, PEXT_INODE Inode,
             VOID (*Report)(PVOID, UCHAR, const char *, ULONG, const UCHAR *, ULONG), PVOID Ctx)
{
    LIST_CTX L;
    L.Report = Report; L.Ctx = Ctx;
    return ForEach(Vol, Ino, Inode, ListCb, &L);
}

typedef struct {
    UCHAR Index; const char *Name; ULONG NameLen;
    PUCHAR Out; ULONG OutSize; PULONG ValueLen;
    BOOLEAN Found; EXT_STATUS Status;
} GET_CTX;

static VOID
GetCb(PVOID C, PEXT_XATTR_ENTRY E, const UCHAR *Val, ULONG ValLen)
{
    GET_CTX *G = (GET_CTX *)C;
    if (G->Found || E->NameIndex != G->Index || E->NameLen != G->NameLen ||
        !ExtMemEq(E->Name, G->Name, G->NameLen)) return;
    G->Found = TRUE;
    if (!Val) { G->Status = EXT_EA_CORRUPT; return; }
    if (G->ValueLen) *G->ValueLen = ValLen;
    if (ValLen > G->OutSize) { G->Status = EXT_BUFFER_TOO_SMALL; return; }
    ExtCopy(G->Out, Val, ValLen);
    G->Status = EXT_OK;
}

EXT_STATUS
ExtGetXattr(PEXT_VOL Vol, ULONG Ino, PEXT_INODE Inode, UCHAR NameIndex, const char *Name,
            ULONG NameLen, PUCHAR Value, ULONG ValueSize, PULONG ValueLen)
{
    GET_CTX G;
    EXT_STATUS S;
    G.Index = NameIndex; G.Name = Name; G.NameLen = NameLen;
    G.Out = Value; G.OutSize = ValueSize; G.ValueLen = ValueLen;
    G.Found = FALSE; G.Status = EXT_NOT_FOUND;
    S = ForEach(Vol, Ino, Inode, GetCb, &G);
    if (!NT_SUCCESS(S)) return S;
    return G.Found ? G.Status : EXT_NOT_FOUND;
}

/* ------------------------------------------------------------------ */
/* The external block: hash, checksum, references                      */
/* ------------------------------------------------------------------ */

static ULONG
EntryHash(PEXT_XATTR_ENTRY E, PUCHAR BlockStart)
{
    ULONG Hash = 0, n;
    const UCHAR *P = (const UCHAR *)E->Name;
    for (n = 0; n < E->NameLen; n++) Hash = (Hash << 5) ^ (Hash >> 27) ^ *P++;
    if (E->ValueInum == 0 && E->ValueSize) {
        PULONG V = (PULONG)(BlockStart + E->ValueOffs);
        for (n = (E->ValueSize + EXT_XATTR_ROUND) >> 2; n; n--) Hash = (Hash << 16) ^ (Hash >> 16) ^ *V++;
    }
    return Hash;
}

static VOID
RehashBlock(PUCHAR Block)
{
    PEXT_XATTR_ENTRY E = (PEXT_XATTR_ENTRY)(Block + sizeof(EXT_XATTR_HEADER));
    ULONG Hash = 0;
    while (!IS_LAST(E)) {
        E->Hash = EntryHash(E, Block);
        if (E->Hash == 0) { Hash = 0; break; }
        Hash = (Hash << 16) ^ (Hash >> 16) ^ E->Hash;
        E = NEXT_ENTRY(E);
    }
    ((PEXT_XATTR_HEADER)Block)->Hash = Hash;
}

/* ext4_xattr_block_csum: seed, the 64-bit block number, the block with h_checksum as 0 */
static VOID
BlockCsum(PEXT_VOL Vol, ULONGLONG BlockNr, PUCHAR Block)
{
    PEXT_XATTR_HEADER H = (PEXT_XATTR_HEADER)Block;
    UCHAR Le[8];
    ULONG i;
    if (!Vol->HasMetaCsum) return;
    for (i = 0; i < 8; i++) Le[i] = (UCHAR)(BlockNr >> (8 * i));
    H->Checksum = 0;
    H->Checksum = ExtCrc32c(ExtCrc32c(Vol->ChecksumSeed, Le, 8), Block, Vol->BlockSize);
}

/* Drops one reference to an external block, freeing it at zero */
static EXT_STATUS
BlockDecRef(PEXT_VOL Vol, ULONGLONG Blk)
{
    EXT_BUF Buf;
    PEXT_XATTR_HEADER H;
    EXT_STATUS S;
    if (Blk <= Vol->FirstDataBlock || Blk >= Vol->BlocksCount) return EXT_EA_CORRUPT;
    S = ExtMapBlocks(Vol, Blk, 1, EXT_MAP_WRITE, &Buf);
    if (!NT_SUCCESS(S)) return S;
    H = (PEXT_XATTR_HEADER)Buf.Data;
    if (H->Magic == EXT_XATTR_MAGIC && H->Refcount > 1) {
        H->Refcount--;
        BlockCsum(Vol, Blk, Buf.Data);
        ExtSetDirty(Vol, &Buf);
        ExtHostUnmap(Vol, &Buf);
        return EXT_OK;
    }
    ExtHostUnmap(Vol, &Buf);
    return ExtFreeBlock(Vol, Blk);
}

static ULONG
BlockRefcount(PEXT_VOL Vol, ULONGLONG Blk)
{
    EXT_BUF Buf;
    ULONG R = 1;
    if (NT_SUCCESS(ExtMapBlocks(Vol, Blk, 1, EXT_MAP_READ, &Buf))) {
        if (((PEXT_XATTR_HEADER)Buf.Data)->Magic == EXT_XATTR_MAGIC) R = ((PEXT_XATTR_HEADER)Buf.Data)->Refcount;
        ExtHostUnmap(Vol, &Buf);
    }
    return R;
}

/* An ea_inode's reference count is i_ctime:i_version (Linux) */
static VOID
EaInodeDecRef(PEXT_VOL Vol, ULONG EaIno)
{
    EXT_INODE Ea;
    ULONGLONG Ref;
    if (EaIno < EXT_FIRST_NONRES_INO || EaIno > Vol->InodesCount) return;
    if (!NT_SUCCESS(ExtReadInode(Vol, EaIno, &Ea)) || !(Ea.Flags & EXT_IF_EA_INODE)) return;
    Ref = ((ULONGLONG)Ea.Ctime << 32) | Ea.Version;
    if (Ref > 1) {
        Ref--;
        Ea.Ctime = (ULONG)(Ref >> 32);
        Ea.Version = (ULONG)Ref;
        ExtWriteInode(Vol, EaIno, &Ea);
        return;
    }
    ExtFreeInodeData(Vol, EaIno, &Ea);
    Ea.LinksCount = 0;
    Ea.Dtime = ExtHostNow();
    ExtInodeSetSize(Vol, &Ea, 0);
    ExtWriteInode(Vol, EaIno, &Ea);
    ExtFreeInode(Vol, EaIno, FALSE);
}

typedef struct { PEXT_VOL Vol; } REL_CTX;
static VOID
ReleaseCb(PVOID C, PEXT_XATTR_ENTRY E, const UCHAR *Val, ULONG ValLen)
{
    (VOID)Val; (VOID)ValLen;
    if (E->ValueInum) EaInodeDecRef(((REL_CTX *)C)->Vol, E->ValueInum);
}

/* For a deleted inode: release ea_inode values and the external block */
EXT_STATUS
ExtXattrRelease(PEXT_VOL Vol, ULONG Ino, PEXT_INODE Inode)
{
    REL_CTX C;
    ULONGLONG Blk = XattrBlockOf(Vol, Inode);
    C.Vol = Vol;
    if (Vol->FeatureIncompat & EXT_FI_EA_INODE) ForEach(Vol, Ino, Inode, ReleaseCb, &C);
    if (Blk) {
        BlockDecRef(Vol, Blk);
        SetXattrBlock(Vol, Inode, 0);
    }
    return EXT_OK;
}

/* ------------------------------------------------------------------ */
/* Set / remove                                                        */
/* ------------------------------------------------------------------ */

static VOID
GatherCb(PVOID C, PEXT_XATTR_ENTRY E, const UCHAR *Val, ULONG ValLen)
{
    PXSET Set = (PXSET)C;
    ULONG i;
    if (E->ValueInum) { Set->HasEaInode = TRUE; return; }
    for (i = 0; i < Set->Count; i++)          /* ibody wins over a duplicate in the block */
        if (Set->Index[i] == E->NameIndex && Set->NameLen[i] == E->NameLen &&
            ExtMemEq(Set->Name[i], E->Name, E->NameLen)) return;
    if (!Val || Set->Count >= SET_MAX_ENTRIES || Set->ArenaUsed + ValLen > SET_ARENA) {
        Set->HasEaInode = TRUE;               /* can't carry it over: refuse the rewrite */
        return;
    }
    i = Set->Count++;
    Set->Index[i] = E->NameIndex;
    Set->NameLen[i] = E->NameLen;
    ExtCopy(Set->Name[i], E->Name, E->NameLen);
    Set->ValueOff[i] = Set->ArenaUsed;
    Set->ValueLen[i] = ValLen;
    ExtCopy(Set->Arena + Set->ArenaUsed, Val, ValLen);
    Set->ArenaUsed += ValLen;
}

static EXT_STATUS
BuildBlock(PEXT_VOL Vol, PXSET Set, PUCHAR Block)
{
    PEXT_XATTR_HEADER H = (PEXT_XATTR_HEADER)Block;
    PUCHAR EntryPtr = Block + sizeof(EXT_XATTR_HEADER);
    ULONG ValueEnd = Vol->BlockSize, i;

    ExtZero(Block, Vol->BlockSize);
    H->Magic = EXT_XATTR_MAGIC;
    H->Refcount = 1;
    H->Blocks = 1;
    for (i = 0; i < Set->Count; i++) {
        PEXT_XATTR_ENTRY E = (PEXT_XATTR_ENTRY)EntryPtr;
        ULONG EntSz = EXT_XATTR_ENTRY_SIZE(Set->NameLen[i]);
        ULONG ValSz = EXT_XATTR_SIZE(Set->ValueLen[i]);
        if (EntryPtr + EntSz + 4 > Block + ValueEnd - ValSz) return EXT_NO_SPACE;
        E->NameLen = Set->NameLen[i];
        E->NameIndex = Set->Index[i];
        E->ValueSize = Set->ValueLen[i];
        ExtCopy(E->Name, Set->Name[i], Set->NameLen[i]);
        ValueEnd -= ValSz;
        E->ValueOffs = (USHORT)(Set->ValueLen[i] ? ValueEnd : 0);
        if (Set->ValueLen[i]) ExtCopy(Block + ValueEnd, Set->Arena + Set->ValueOff[i], Set->ValueLen[i]);
        EntryPtr += EntSz;
    }
    RehashBlock(Block);
    return EXT_OK;
}

EXT_STATUS
ExtSetXattr(PEXT_VOL Vol, ULONG Ino, PEXT_INODE Inode, UCHAR NameIndex, const char *Name,
            ULONG NameLen, const UCHAR *Value, ULONG ValueLen)
{
    PXSET Set = NULL;
    PUCHAR Image = NULL, BlockImg = NULL;
    ULONGLONG OldBlk, NewBlk = 0;
    ULONG i;
    BOOLEAN Found = FALSE;
    EXT_STATUS S;

    if (Vol->ReadOnly) return EXT_READ_ONLY;
    if (NameLen == 0 || NameLen > 255) return EXT_BAD_NAME;
    if (Value && ValueLen > Vol->BlockSize) return EXT_NO_SPACE;
    /* revision 0 has no feature fields to announce xattrs with */
    if (Value && Vol->Super->RevLevel == 0) return EXT_NOT_SUPPORTED;
    if (Value && !(Vol->FeatureCompat & EXT_FC_EXT_ATTR)) {
        Vol->FeatureCompat |= EXT_FC_EXT_ATTR;
        Vol->Super->FeatureCompat |= EXT_FC_EXT_ATTR;
        Vol->BackupsStale = TRUE;
    }

    /* all attributes move to the external block below, but an inline inode's
       system.data must stay in the inode: take the data out of line first */
    if ((Inode->Flags & EXT_IF_INLINE_DATA) &&
        !(NameIndex == EXT_XATTR_INDEX_SYSTEM && NameLen == 4 && ExtMemEq(Name, "data", 4))) {
        S = ExtUninline(Vol, Ino, Inode);
        if (!NT_SUCCESS(S)) return S;
    }

    Set = (PXSET)ExtHostAlloc(sizeof(XSET));
    Image = (PUCHAR)ExtHostAlloc(Vol->InodeSize);
    BlockImg = (PUCHAR)ExtHostAlloc(Vol->BlockSize);
    if (!Set || !Image || !BlockImg) { S = EXT_NO_MEMORY; goto done; }
    ExtZero(Set, sizeof(XSET));

    S = ForEach(Vol, Ino, Inode, GatherCb, Set);
    if (!NT_SUCCESS(S)) goto done;
    if (Set->HasEaInode) { S = EXT_NOT_SUPPORTED; goto done; }

    /* apply the change */
    for (i = 0; i < Set->Count; i++) {
        if (Set->Index[i] != NameIndex || Set->NameLen[i] != NameLen || !ExtMemEq(Set->Name[i], Name, NameLen))
            continue;
        Found = TRUE;
        if (Value == NULL) {
            ULONG k;
            for (k = i; k + 1 < Set->Count; k++) {
                Set->Index[k] = Set->Index[k + 1];
                Set->NameLen[k] = Set->NameLen[k + 1];
                ExtCopy(Set->Name[k], Set->Name[k + 1], 256);
                Set->ValueOff[k] = Set->ValueOff[k + 1];
                Set->ValueLen[k] = Set->ValueLen[k + 1];
            }
            Set->Count--;
        } else {
            if (Set->ArenaUsed + ValueLen > SET_ARENA) { S = EXT_NO_SPACE; goto done; }
            Set->ValueOff[i] = Set->ArenaUsed;
            Set->ValueLen[i] = ValueLen;
            ExtCopy(Set->Arena + Set->ArenaUsed, Value, ValueLen);
            Set->ArenaUsed += ValueLen;
        }
        break;
    }
    if (!Found) {
        if (Value == NULL) { S = EXT_OK; goto done; }          /* nothing to remove */
        if (Set->Count >= SET_MAX_ENTRIES || Set->ArenaUsed + ValueLen > SET_ARENA) { S = EXT_NO_SPACE; goto done; }
        i = Set->Count++;
        Set->Index[i] = NameIndex;
        Set->NameLen[i] = (UCHAR)NameLen;
        ExtCopy(Set->Name[i], Name, NameLen);
        Set->ValueOff[i] = Set->ArenaUsed;
        Set->ValueLen[i] = ValueLen;
        ExtCopy(Set->Arena + Set->ArenaUsed, Value, ValueLen);
        Set->ArenaUsed += ValueLen;
    }

    if (Set->Count) {
        S = BuildBlock(Vol, Set, BlockImg);
        if (!NT_SUCCESS(S)) goto done;
    }

    /* the inode's image: in-inode attributes are cleared */
    S = ReadInodeImage(Vol, Ino, Image);
    if (!NT_SUCCESS(S)) goto done;
    {
        PUCHAR H = IbodyHeader(Vol, Image);
        if (H) ExtZero(H, (ULONG)(Image + Vol->InodeSize - H));
    }

    OldBlk = XattrBlockOf(Vol, Inode);
    if (Set->Count) {
        EXT_BUF Buf;
        if (OldBlk && BlockRefcount(Vol, OldBlk) <= 1) {
            NewBlk = OldBlk;                                    /* ours alone: rewrite in place */
        } else {
            S = ExtAllocBlock(Vol, OldBlk ? OldBlk : ExtGroupBase(Vol, Ino), &NewBlk);
            if (!NT_SUCCESS(S)) goto done;
            if (OldBlk) BlockDecRef(Vol, OldBlk);               /* shared: leave it to the others */
        }
        BlockCsum(Vol, NewBlk, BlockImg);
        S = ExtMapBlocks(Vol, NewBlk, 1, EXT_MAP_NEW, &Buf);
        if (!NT_SUCCESS(S)) goto done;
        ExtCopy(Buf.Data, BlockImg, Vol->BlockSize);
        ExtSetDirty(Vol, &Buf);
        ExtHostUnmap(Vol, &Buf);
        SetXattrBlock(Vol, Inode, NewBlk);
        if (!OldBlk) ExtInodeSetBlocks(Vol, Inode, ExtInodeBlocks(Vol, Inode) + Vol->SectorsPerCluster);
    } else if (OldBlk) {
        BlockDecRef(Vol, OldBlk);
        SetXattrBlock(Vol, Inode, 0);
        if (ExtInodeBlocks(Vol, Inode) >= Vol->SectorsPerCluster)
            ExtInodeSetBlocks(Vol, Inode, ExtInodeBlocks(Vol, Inode) - Vol->SectorsPerCluster);
    }

    /* persist: the struct's fields over the image's low part */
    Inode->Ctime = ExtHostNow();
    ExtCopy(Image, Inode, ExtInodeCoreSize(Vol, Inode));
    S = WriteInodeImage(Vol, Ino, Image);

done:
    if (Set) ExtHostFree(Set);
    if (Image) ExtHostFree(Image);
    if (BlockImg) ExtHostFree(BlockImg);
    return S;
}

/* For the checker: sets the checksum of an external xattr block image */
VOID
ExtXattrBlockCsum(PEXT_VOL Vol, ULONGLONG BlockNr, PUCHAR Block)
{
    BlockCsum(Vol, BlockNr, Block);
}
