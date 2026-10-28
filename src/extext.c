/*
 * EXTNT - ext4 extent trees, any depth.
 *
 * Reading walks the tree with mapped buffers (never a block-sized copy on the
 * stack: the NT kernel stack is 12 KB). Uninitialised (fallocated) extents read
 * as zeros.
 *
 * Writing has a fast path for the common case - the new block extends the
 * extent just before it, in the same leaf - which patches that one leaf entry
 * in place. Anything structural (a new extent, filling a hole between extents,
 * splitting an uninitialised extent, truncation) loads the whole extent list
 * into memory, edits and normalises it (adjacent compatible extents merged),
 * and rebuilds the tree at its minimum height, reusing the old tree blocks.
 * That keeps every tree we write fully valid for e2fsck and the Linux kernel
 * without the split/merge machinery of a B-tree.
 *
 * Block ownership: every block this file allocates or frees for an inode
 * (data or tree) is charged to i_blocks by the public entry points, from the
 * change in the volume's free-block count.
 */

#ifdef EXT_KERNEL
#include "ext.h"
#else
#include "extusr.h"
#include "extsup.h"
#endif

#define XROOT(Inode)        ((PEXT_EXTENT_HEADER)(Inode)->Block)
#define XROOT_MAX           ((ULONG)((sizeof(((PEXT_INODE)0)->Block) - sizeof(EXT_EXTENT_HEADER)) / sizeof(EXT_EXTENT)))
#define XNODE_MAX(Vol)      ((ULONG)(((Vol)->BlockSize - sizeof(EXT_EXTENT_HEADER)) / sizeof(EXT_EXTENT)))
#define XMAX_DEPTH          5

#define XLEN(Raw)           ((ULONG)((Raw) > EXT_EXTENT_INIT_MAX ? (Raw) - EXT_EXTENT_INIT_MAX : (Raw)))
#define XUNINIT(Raw)        ((Raw) > EXT_EXTENT_INIT_MAX)
#define XSTART(E)           ((E)->StartLo | ((ULONGLONG)(E)->StartHi << 32))
#define XCHILD(I)           ((I)->LeafLo | ((ULONGLONG)(I)->LeafHi << 32))

/* ------------------------------------------------------------------ */
/* Checksums                                                           */
/* ------------------------------------------------------------------ */

VOID
ExtExtentBlockCsum(PEXT_VOL Vol, ULONG Ino, PEXT_INODE Inode, PUCHAR Block)
{
    PEXT_EXTENT_HEADER H = (PEXT_EXTENT_HEADER)Block;
    ULONG Off = (ULONG)sizeof(EXT_EXTENT_HEADER) + (ULONG)H->Max * (ULONG)sizeof(EXT_EXTENT);
    UCHAR Le[8];
    ULONG Crc, Gen = Inode->Generation;

    if (!Vol->HasMetaCsum) return;
    if (Off + sizeof(EXT_EXTENT_TAIL) > Vol->BlockSize) return;
    Le[0] = (UCHAR)Ino; Le[1] = (UCHAR)(Ino >> 8); Le[2] = (UCHAR)(Ino >> 16); Le[3] = (UCHAR)(Ino >> 24);
    Le[4] = (UCHAR)Gen; Le[5] = (UCHAR)(Gen >> 8); Le[6] = (UCHAR)(Gen >> 16); Le[7] = (UCHAR)(Gen >> 24);
    Crc = ExtCrc32c(Vol->ChecksumSeed, Le, 4);
    Crc = ExtCrc32c(Crc, Le + 4, 4);
    Crc = ExtCrc32c(Crc, Block, Off);
    ((PEXT_EXTENT_TAIL)(Block + Off))->Checksum = Crc;
}

static BOOLEAN
XHeaderOk(PEXT_VOL Vol, PEXT_EXTENT_HEADER H, ULONG Depth, ULONG Max)
{
    if (H->Magic != EXT_EXTENT_MAGIC) return FALSE;
    if (H->Depth != Depth || H->Depth > XMAX_DEPTH) return FALSE;
    if (H->Max == 0 || H->Max > Max || H->Entries > H->Max) return FALSE;
    (VOID)Vol;
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* Lookup (read side)                                                  */
/* ------------------------------------------------------------------ */

/* Walks to the leaf for FileBlk. On return Leaf points into either the inode
   (Buf->Host == NULL) or a mapped node block that the caller must unmap.
   Mode is the map mode for the leaf node (index nodes are read-only). */
static EXT_STATUS
XFindLeaf(PEXT_VOL Vol, PEXT_INODE Inode, ULONGLONG FileBlk, ULONG Mode,
          PEXT_EXTENT_HEADER *Leaf, PEXT_BUF LeafBuf, PULONGLONG LeafBlock)
{
    PEXT_EXTENT_HEADER H = XROOT(Inode);
    EXT_BUF Cur;
    ULONG Depth;

    LeafBuf->Host = NULL;
    *LeafBlock = 0;
    if (!XHeaderOk(Vol, H, H->Depth, XROOT_MAX)) return EXT_FILE_CORRUPT;
    Depth = H->Depth;
    Cur.Host = NULL;

    while (Depth > 0) {
        PEXT_EXTENT_IDX Ix = (PEXT_EXTENT_IDX)(H + 1);
        ULONG i, n = H->Entries;
        ULONGLONG Child;
        EXT_BUF Next;
        EXT_STATUS S;

        if (n == 0) { if (Cur.Host) ExtHostUnmap(Vol, &Cur); return EXT_FILE_CORRUPT; }
        for (i = 0; i + 1 < n; i++) if (FileBlk < Ix[i + 1].Block) break;
        Child = XCHILD(&Ix[i]);
        if (Child <= Vol->FirstDataBlock || Child >= Vol->BlocksCount) {
            if (Cur.Host) ExtHostUnmap(Vol, &Cur);
            return EXT_FILE_CORRUPT;
        }
        S = ExtMapBlocks(Vol, Child, 1, Depth == 1 ? Mode : EXT_MAP_READ, &Next);
        if (Cur.Host) ExtHostUnmap(Vol, &Cur);
        if (!NT_SUCCESS(S)) return S;
        Cur = Next;
        H = (PEXT_EXTENT_HEADER)Cur.Data;
        Depth--;
        if (!XHeaderOk(Vol, H, Depth, XNODE_MAX(Vol))) { ExtHostUnmap(Vol, &Cur); return EXT_FILE_CORRUPT; }
        *LeafBlock = Child;
    }
    *Leaf = H;
    *LeafBuf = Cur;
    return EXT_OK;
}

EXT_STATUS
ExtExtentMap(PEXT_VOL Vol, PEXT_INODE Inode, ULONGLONG FileBlk, PULONGLONG Disk, PULONG RunLen)
{
    PEXT_EXTENT_HEADER H;
    EXT_BUF Buf;
    ULONGLONG LeafBlk;
    PEXT_EXTENT Ex;
    ULONG i, n;
    EXT_STATUS S = XFindLeaf(Vol, Inode, FileBlk, EXT_MAP_READ, &H, &Buf, &LeafBlk);
    if (!NT_SUCCESS(S)) return S;

    *Disk = 0;
    *RunLen = 1;
    Ex = (PEXT_EXTENT)(H + 1);
    n = H->Entries;
    for (i = 0; i < n; i++) {
        ULONG Len = XLEN(Ex[i].Len);
        if (FileBlk < Ex[i].Block) {
            ULONGLONG Gap = Ex[i].Block - FileBlk;
            *RunLen = Gap > 0x10000000 ? 0x10000000 : (ULONG)Gap;
            break;
        }
        if (FileBlk < (ULONGLONG)Ex[i].Block + Len) {
            ULONG Off = (ULONG)(FileBlk - Ex[i].Block);
            *RunLen = Len - Off;
            if (!XUNINIT(Ex[i].Len)) *Disk = XSTART(&Ex[i]) + Off;   /* uninit reads as a hole */
            break;
        }
    }
    if (Buf.Host) ExtHostUnmap(Vol, &Buf);
    return EXT_OK;
}

/* ------------------------------------------------------------------ */
/* The in-memory extent list                                           */
/* ------------------------------------------------------------------ */

typedef struct _XENT {
    ULONG       Block;
    ULONG       Len;            /* raw on-disk length (uninit encoded) */
    ULONGLONG   Start;
} XENT;

typedef struct _XLIST {
    XENT       *E;
    ULONG       Count, Cap;
    ULONGLONG  *Node;           /* tree blocks of the current tree */
    ULONG       NodeCount, NodeCap;
} XLIST;

static BOOLEAN
XGrow(PVOID *Arr, ULONG *Cap, ULONG Need, ULONG ElemSize)
{
    ULONG NewCap;
    PVOID N;
    if (Need <= *Cap) return TRUE;
    NewCap = *Cap ? *Cap : 16;
    while (NewCap < Need) NewCap *= 2;
    N = ExtHostAlloc(NewCap * ElemSize);
    if (!N) return FALSE;
    if (*Arr) { ExtCopy(N, *Arr, *Cap * ElemSize); ExtHostFree(*Arr); }
    *Arr = N;
    *Cap = NewCap;
    return TRUE;
}

static VOID
XFreeList(XLIST *L)
{
    if (L->E) ExtHostFree(L->E);
    if (L->Node) ExtHostFree(L->Node);
    ExtZero(L, sizeof(*L));
}

static EXT_STATUS
XAddExtent(XLIST *L, ULONG Block, ULONG Len, ULONGLONG Start)
{
    if (!XGrow((PVOID *)&L->E, &L->Cap, L->Count + 1, sizeof(XENT))) return EXT_NO_MEMORY;
    L->E[L->Count].Block = Block;
    L->E[L->Count].Len = Len;
    L->E[L->Count].Start = Start;
    L->Count++;
    return EXT_OK;
}

static EXT_STATUS
XLoadNode(PEXT_VOL Vol, PEXT_EXTENT_HEADER H, XLIST *L, ULONG Depth)
{
    ULONG i;
    EXT_STATUS S;

    if (Depth == 0) {
        PEXT_EXTENT Ex = (PEXT_EXTENT)(H + 1);
        for (i = 0; i < H->Entries; i++) {
            ULONG Len = XLEN(Ex[i].Len);
            ULONGLONG Start = XSTART(&Ex[i]);
            if (Len == 0 || Start + Len > Vol->BlocksCount) return EXT_FILE_CORRUPT;
            if (L->Count && (ULONGLONG)L->E[L->Count - 1].Block + XLEN(L->E[L->Count - 1].Len) > Ex[i].Block)
                return EXT_FILE_CORRUPT;            /* overlapping / out of order */
            S = XAddExtent(L, Ex[i].Block, Ex[i].Len, Start);
            if (!NT_SUCCESS(S)) return S;
        }
        return EXT_OK;
    }
    {
        PEXT_EXTENT_IDX Ix = (PEXT_EXTENT_IDX)(H + 1);
        for (i = 0; i < H->Entries; i++) {
            ULONGLONG Child = XCHILD(&Ix[i]);
            EXT_BUF Buf;
            if (Child <= Vol->FirstDataBlock || Child >= Vol->BlocksCount) return EXT_FILE_CORRUPT;
            if (!XGrow((PVOID *)&L->Node, &L->NodeCap, L->NodeCount + 1, sizeof(ULONGLONG))) return EXT_NO_MEMORY;
            L->Node[L->NodeCount++] = Child;
            S = ExtMapBlocks(Vol, Child, 1, EXT_MAP_READ, &Buf);
            if (!NT_SUCCESS(S)) return S;
            if (!XHeaderOk(Vol, (PEXT_EXTENT_HEADER)Buf.Data, Depth - 1, XNODE_MAX(Vol))) {
                ExtHostUnmap(Vol, &Buf);
                return EXT_FILE_CORRUPT;
            }
            S = XLoadNode(Vol, (PEXT_EXTENT_HEADER)Buf.Data, L, Depth - 1);
            ExtHostUnmap(Vol, &Buf);
            if (!NT_SUCCESS(S)) return S;
        }
    }
    return EXT_OK;
}

static EXT_STATUS
XLoad(PEXT_VOL Vol, PEXT_INODE Inode, XLIST *L)
{
    PEXT_EXTENT_HEADER H = XROOT(Inode);
    EXT_STATUS S;
    ExtZero(L, sizeof(*L));
    if (!XHeaderOk(Vol, H, H->Depth, XROOT_MAX)) return EXT_FILE_CORRUPT;
    S = XLoadNode(Vol, H, L, H->Depth);
    if (!NT_SUCCESS(S)) XFreeList(L);
    return S;
}

/* Merge neighbours that are logically and physically contiguous and in the
   same state, within the length limits */
static VOID
XNormalize(XLIST *L)
{
    ULONG r, w = 0;
    for (r = 0; r < L->Count; r++) {
        XENT *C = &L->E[r];
        if (XLEN(C->Len) == 0) continue;
        if (w > 0) {
            XENT *P = &L->E[w - 1];
            ULONG PL = XLEN(P->Len), CL = XLEN(C->Len);
            BOOLEAN PU = XUNINIT(P->Len), CU = XUNINIT(C->Len);
            ULONG Limit = PU ? EXT_EXTENT_INIT_MAX - 1 : EXT_EXTENT_INIT_MAX;
            if (PU == CU && (ULONGLONG)P->Block + PL == C->Block && P->Start + PL == C->Start &&
                PL + CL <= Limit) {
                P->Len = PU ? (PL + CL + EXT_EXTENT_INIT_MAX) : (PL + CL);
                continue;
            }
        }
        L->E[w++] = *C;
    }
    L->Count = w;
}

static VOID
XPutExtent(PEXT_EXTENT D, const XENT *S)
{
    D->Block = S->Block;
    D->Len = (USHORT)S->Len;
    D->StartLo = (ULONG)S->Start;
    D->StartHi = (USHORT)(S->Start >> 32);
}

/* Writes the list back as a tree of minimum height, reusing L->Node blocks */
static EXT_STATUS
XStore(PEXT_VOL Vol, ULONG Ino, PEXT_INODE Inode, XLIST *L, ULONGLONG Goal)
{
    PEXT_EXTENT_HEADER Root = XROOT(Inode);
    ULONG NodeMax = XNODE_MAX(Vol);
    ULONG LevelCount[XMAX_DEPTH + 1];
    ULONG Levels = 0, Total = 0, Used = 0, i, l;
    ULONGLONG *Blocks = NULL;       /* new tree blocks, level by level (leaves first) */
    ULONG *Keys = NULL;             /* first file block of each node, same order */
    EXT_STATUS S = EXT_OK;

    if (L->Count <= XROOT_MAX) {
        Levels = 0;
    } else {
        ULONG n = L->Count;
        do {
            n = (n + NodeMax - 1) / NodeMax;
            if (Levels >= XMAX_DEPTH) return EXT_TOO_LARGE;
            LevelCount[Levels++] = n;
            Total += n;
        } while (n > XROOT_MAX);
    }

    if (Total) {
        Blocks = (ULONGLONG *)ExtHostAlloc(Total * sizeof(ULONGLONG));
        Keys = (ULONG *)ExtHostAlloc(Total * sizeof(ULONG));
        if (!Blocks || !Keys) { S = EXT_NO_MEMORY; goto out; }
        /* reuse old tree blocks first, then allocate */
        for (i = 0; i < Total; i++) {
            if (i < L->NodeCount) {
                Blocks[i] = L->Node[i];
            } else {
                S = ExtAllocBlock(Vol, i ? Blocks[i - 1] : Goal, &Blocks[i]);
                if (!NT_SUCCESS(S)) {
                    while (i > L->NodeCount) ExtFreeBlock(Vol, Blocks[--i]);
                    goto out;
                }
            }
        }
    }
    /* old tree blocks not reused */
    for (i = Total; i < L->NodeCount; i++) ExtFreeBlock(Vol, L->Node[i]);

    /* leaves */
    for (i = 0; i < (Levels ? LevelCount[0] : 0); i++) {
        EXT_BUF Buf;
        PEXT_EXTENT_HEADER H;
        ULONG First = i * NodeMax, Cnt = MIN_UL(NodeMax, L->Count - First), k;
        S = ExtMapBlocks(Vol, Blocks[i], 1, EXT_MAP_NEW, &Buf);
        if (!NT_SUCCESS(S)) goto out;
        H = (PEXT_EXTENT_HEADER)Buf.Data;
        H->Magic = EXT_EXTENT_MAGIC; H->Depth = 0; H->Max = (USHORT)NodeMax;
        H->Entries = (USHORT)Cnt; H->Generation = 0;
        for (k = 0; k < Cnt; k++) XPutExtent((PEXT_EXTENT)(H + 1) + k, &L->E[First + k]);
        Keys[i] = L->E[First].Block;
        ExtExtentBlockCsum(Vol, Ino, Inode, Buf.Data);
        ExtSetDirty(Vol, &Buf);
        ExtHostUnmap(Vol, &Buf);
    }
    /* index levels above the leaves */
    Used = Levels ? LevelCount[0] : 0;
    for (l = 1; l < Levels; l++) {
        ULONG Below = Used - LevelCount[l - 1];        /* first node of level l-1 */
        for (i = 0; i < LevelCount[l]; i++) {
            EXT_BUF Buf;
            PEXT_EXTENT_HEADER H;
            ULONG First = i * NodeMax, Cnt = MIN_UL(NodeMax, LevelCount[l - 1] - First), k;
            S = ExtMapBlocks(Vol, Blocks[Used + i], 1, EXT_MAP_NEW, &Buf);
            if (!NT_SUCCESS(S)) goto out;
            H = (PEXT_EXTENT_HEADER)Buf.Data;
            H->Magic = EXT_EXTENT_MAGIC; H->Depth = (USHORT)l; H->Max = (USHORT)NodeMax;
            H->Entries = (USHORT)Cnt; H->Generation = 0;
            for (k = 0; k < Cnt; k++) {
                PEXT_EXTENT_IDX Ix = (PEXT_EXTENT_IDX)(H + 1) + k;
                ULONGLONG Child = Blocks[Below + First + k];
                Ix->Block = Keys[Below + First + k];
                Ix->LeafLo = (ULONG)Child; Ix->LeafHi = (USHORT)(Child >> 32); Ix->Unused = 0;
            }
            Keys[Used + i] = Keys[Below + First];
            ExtExtentBlockCsum(Vol, Ino, Inode, Buf.Data);
            ExtSetDirty(Vol, &Buf);
            ExtHostUnmap(Vol, &Buf);
        }
        Used += LevelCount[l];
    }

    /* the root in the inode */
    ExtZero(Inode->Block, sizeof(Inode->Block));
    Root->Magic = EXT_EXTENT_MAGIC;
    Root->Max = (USHORT)XROOT_MAX;
    Root->Depth = (USHORT)Levels;
    if (Levels == 0) {
        Root->Entries = (USHORT)L->Count;
        for (i = 0; i < L->Count; i++) XPutExtent((PEXT_EXTENT)(Root + 1) + i, &L->E[i]);
    } else {
        ULONG Top = LevelCount[Levels - 1], Base = Total - Top;
        Root->Entries = (USHORT)Top;
        for (i = 0; i < Top; i++) {
            PEXT_EXTENT_IDX Ix = (PEXT_EXTENT_IDX)(Root + 1) + i;
            Ix->Block = Keys[Base + i];
            Ix->LeafLo = (ULONG)Blocks[Base + i]; Ix->LeafHi = (USHORT)(Blocks[Base + i] >> 32);
        }
    }

out:
    if (Blocks) ExtHostFree(Blocks);
    if (Keys) ExtHostFree(Keys);
    return S;
}

/*
 * Frees the blocks of a run. With bigalloc a cluster is freed once, through
 * any of its blocks: *Last is the last cluster freed (runs come in file
 * order, so the pieces of one cluster are next to each other) and Keep a
 * cluster that must stay (still holding kept blocks); ~0 for none.
 */
static VOID
XFreeRunC(PEXT_VOL Vol, ULONGLONG Start, ULONG Len, PULONGLONG Last, ULONGLONG Keep)
{
    ULONGLONG b, End = Start + Len;
    for (b = Start; b < End; ) {
        ULONGLONG C = b >> Vol->ClusterBits;
        if (C != *Last && C != Keep) {
            ExtFreeBlock(Vol, b);
            *Last = C;
        }
        b = (C + 1) << Vol->ClusterBits;
    }
}


/* the inode gains/loses every block allocated/freed since the snapshot */
#define XSNAP(Vol)  ((Vol)->AllocTotal - (Vol)->FreeTotal)
static VOID
XCharge(PEXT_VOL Vol, PEXT_INODE Inode, ULONGLONG Before)
{
    ULONGLONG Have = ExtInodeBlocks(Vol, Inode), Now = XSNAP(Vol);
    if (Now >= Before) {
        ExtInodeSetBlocks(Vol, Inode, Have + (Now - Before) * Vol->SectorsPerCluster);
    } else {
        ULONGLONG Freed = (Before - Now) * Vol->SectorsPerCluster;
        ExtInodeSetBlocks(Vol, Inode, Have > Freed ? Have - Freed : 0);
    }
}

/* ------------------------------------------------------------------ */
/* Make one file block present                                         */
/* ------------------------------------------------------------------ */

/* Structural change: insert the new single-block extent / split an uninit one */
static EXT_STATUS
XInsertSlow(PEXT_VOL Vol, ULONG Ino, PEXT_INODE Inode, ULONGLONG FileBlk, ULONGLONG NewBlk,
            PULONGLONG Disk)
{
    XLIST L;
    ULONG i, Pos;
    EXT_STATUS S = XLoad(Vol, Inode, &L);
    if (!NT_SUCCESS(S)) return S;

    for (Pos = 0; Pos < L.Count; Pos++) {
        XENT *E = &L.E[Pos];
        ULONG Len = XLEN(E->Len);
        if (FileBlk < E->Block) break;
        if (FileBlk < (ULONGLONG)E->Block + Len) {
            /* inside an uninitialised extent: split into [before][block][after] */
            ULONG Off = (ULONG)(FileBlk - E->Block);
            XENT Before = *E, Mid, After = *E;
            if (!XUNINIT(E->Len)) { *Disk = E->Start + Off; XFreeList(&L); return EXT_OK; }
            Before.Len = Off + EXT_EXTENT_INIT_MAX;
            Mid.Block = (ULONG)FileBlk; Mid.Len = 1; Mid.Start = E->Start + Off;
            After.Block = (ULONG)FileBlk + 1; After.Start = E->Start + Off + 1;
            After.Len = (Len - Off - 1) + EXT_EXTENT_INIT_MAX;
            if (Len - Off - 1 == 0) After.Len = 0;
            if (Off == 0) Before.Len = 0;
            if (!XGrow((PVOID *)&L.E, &L.Cap, L.Count + 2, sizeof(XENT))) { XFreeList(&L); return EXT_NO_MEMORY; }
            for (i = L.Count; i > Pos + 1; i--) L.E[i + 1] = L.E[i - 1];
            L.E[Pos] = Before; L.E[Pos + 1] = Mid; L.E[Pos + 2] = After;
            L.Count += 2;
            *Disk = Mid.Start;
            goto store;
        }
    }
    /* a hole: insert at Pos */
    if (NewBlk == 0) { XFreeList(&L); return EXT_CORRUPT; }
    if (!XGrow((PVOID *)&L.E, &L.Cap, L.Count + 1, sizeof(XENT))) { XFreeList(&L); return EXT_NO_MEMORY; }
    for (i = L.Count; i > Pos; i--) L.E[i] = L.E[i - 1];
    L.E[Pos].Block = (ULONG)FileBlk; L.E[Pos].Len = 1; L.E[Pos].Start = NewBlk;
    L.Count++;
    *Disk = NewBlk;

store:
    XNormalize(&L);
    S = XStore(Vol, Ino, Inode, &L, *Disk);
    XFreeList(&L);
    return S;
}

/* The range of file blocks [*Lo, *Hi) the leaf holding FileBlk is for */
static EXT_STATUS
XLeafRange(PEXT_VOL Vol, PEXT_INODE Inode, ULONGLONG FileBlk, PULONGLONG Lo, PULONGLONG Hi)
{
    PEXT_EXTENT_HEADER H = XROOT(Inode);
    EXT_BUF Cur;
    ULONG Depth = H->Depth;

    *Lo = 0;
    *Hi = (ULONGLONG)1 << 32;
    Cur.Host = NULL;
    while (Depth > 0) {
        PEXT_EXTENT_IDX Ix = (PEXT_EXTENT_IDX)(H + 1);
        ULONG i, n = H->Entries;
        ULONGLONG Child;
        EXT_BUF Next;
        EXT_STATUS S;
        if (n == 0) { if (Cur.Host) ExtHostUnmap(Vol, &Cur); return EXT_FILE_CORRUPT; }
        for (i = 0; i + 1 < n; i++) if (FileBlk < Ix[i + 1].Block) break;
        if (Ix[i].Block > *Lo) *Lo = Ix[i].Block;
        if (i + 1 < n && Ix[i + 1].Block < *Hi) *Hi = Ix[i + 1].Block;
        Child = XCHILD(&Ix[i]);
        if (Depth == 1) break;
        if (Child <= Vol->FirstDataBlock || Child >= Vol->BlocksCount) {
            if (Cur.Host) ExtHostUnmap(Vol, &Cur);
            return EXT_FILE_CORRUPT;
        }
        S = ExtMapBlocks(Vol, Child, 1, EXT_MAP_READ, &Next);
        if (Cur.Host) ExtHostUnmap(Vol, &Cur);
        if (!NT_SUCCESS(S)) return S;
        Cur = Next;
        H = (PEXT_EXTENT_HEADER)Cur.Data;
        Depth--;
        if (!XHeaderOk(Vol, H, Depth, XNODE_MAX(Vol))) { ExtHostUnmap(Vol, &Cur); return EXT_FILE_CORRUPT; }
    }
    if (Cur.Host) ExtHostUnmap(Vol, &Cur);
    return EXT_OK;
}

/*
 * bigalloc: the physical block for FileBlk if another block of its logical
 * cluster is mapped already (logical and physical offsets in a cluster are
 * the same), else *Phys = 0. H is the leaf for FileBlk, Prev its last extent
 * at or before FileBlk; a leaf below the root may not see the whole cluster,
 * and then the full list decides.
 */
static EXT_STATUS
XClusterHome(PEXT_VOL Vol, PEXT_INODE Inode, PEXT_EXTENT_HEADER H, BOOLEAN NotRoot,
             ULONGLONG FileBlk, LONG Prev, PULONGLONG Phys)
{
    ULONGLONG C0 = FileBlk & ~(((ULONGLONG)1 << Vol->ClusterBits) - 1);
    ULONGLONG C1 = C0 + (1u << Vol->ClusterBits);
    PEXT_EXTENT Ex = (PEXT_EXTENT)(H + 1);
    ULONG n = H->Entries, i;
    XLIST L;
    EXT_STATUS S;

    *Phys = 0;
    if (Prev >= 0 && (ULONGLONG)Ex[Prev].Block + XLEN(Ex[Prev].Len) > C0) {
        *Phys = XSTART(&Ex[Prev]) + (FileBlk - Ex[Prev].Block);
        return EXT_OK;
    }
    if ((ULONG)(Prev + 1) < n && Ex[Prev + 1].Block < C1) {
        *Phys = XSTART(&Ex[Prev + 1]) - (Ex[Prev + 1].Block - FileBlk);
        return EXT_OK;
    }
    /* the leaf saw the whole cluster: nothing there */
    if (!NotRoot || ((Prev >= 0 || C0 == FileBlk) && ((ULONG)(Prev + 1) < n || FileBlk + 1 == C1))) {
        return EXT_OK;
    }
    {
        /* ... or the cluster lies within the leaf's key range */
        ULONGLONG Lo, Hi;
        if (NT_SUCCESS(XLeafRange(Vol, Inode, FileBlk, &Lo, &Hi)) &&
            (Prev >= 0 || C0 >= Lo) && ((ULONG)(Prev + 1) < n || C1 <= Hi)) {
            return EXT_OK;
        }
    }
    S = XLoad(Vol, Inode, &L);
    if (!NT_SUCCESS(S)) return S;
    for (i = 0; i < L.Count; i++) {
        if ((ULONGLONG)L.E[i].Block + XLEN(L.E[i].Len) > C0 && L.E[i].Block < C1) {
            *Phys = L.E[i].Start + (FileBlk - L.E[i].Block);
            break;
        }
    }
    XFreeList(&L);
    return EXT_OK;
}

EXT_STATUS
ExtExtentGet(PEXT_VOL Vol, ULONG Ino, PEXT_INODE Inode, ULONGLONG FileBlk,
             PULONGLONG Disk, PBOOLEAN Fresh)
{
    ULONGLONG Before = XSNAP(Vol);
    PEXT_EXTENT_HEADER H;
    EXT_BUF Buf;
    ULONGLONG LeafBlk, Goal, NewBlk = 0;
    PEXT_EXTENT Ex;
    LONG Prev = -1;
    ULONG i, n;
    BOOLEAN NeedSlow = FALSE, Allocated = FALSE;
    EXT_STATUS S;

    *Fresh = FALSE;
    if (FileBlk > 0xFFFFFFFEULL) return EXT_TOO_LARGE;

    S = XFindLeaf(Vol, Inode, FileBlk, EXT_MAP_WRITE, &H, &Buf, &LeafBlk);
    if (!NT_SUCCESS(S)) return S;
    Ex = (PEXT_EXTENT)(H + 1);
    n = H->Entries;
    for (i = 0; i < n; i++) {
        if (FileBlk < Ex[i].Block) break;
        Prev = (LONG)i;
    }
    if (Prev >= 0) {
        PEXT_EXTENT P = &Ex[Prev];
        ULONG Len = XLEN(P->Len);
        if (FileBlk < (ULONGLONG)P->Block + Len) {
            if (!XUNINIT(P->Len)) {
                *Disk = XSTART(P) + (FileBlk - P->Block);
                if (Buf.Host) ExtHostUnmap(Vol, &Buf);
                return EXT_OK;
            }
            NeedSlow = TRUE;            /* split an uninitialised extent */
        }
    }

    if (!NeedSlow) {
        /* allocate the data block, aiming to keep logical and physical in step */
        if (Prev >= 0) Goal = XSTART(&Ex[Prev]) + (FileBlk - Ex[Prev].Block);
        else if (n > 0 && XSTART(&Ex[0]) > (Ex[0].Block - FileBlk)) Goal = XSTART(&Ex[0]) - (Ex[0].Block - FileBlk);
        else Goal = ExtGroupBase(Vol, Ino);
        if (Vol->ClusterBits) {
            /* bigalloc: a block whose logical cluster already has a physical
               one goes there, at the same offset */
            S = XClusterHome(Vol, Inode, H, Buf.Host != NULL, FileBlk, Prev, &NewBlk);
            if (!NT_SUCCESS(S)) { if (Buf.Host) ExtHostUnmap(Vol, &Buf); return S; }
        }
        if (NewBlk == 0) {
            S = ExtAllocBlock(Vol, Goal, &NewBlk);
            if (!NT_SUCCESS(S)) { if (Buf.Host) ExtHostUnmap(Vol, &Buf); return S; }
            NewBlk += FileBlk & ((1u << Vol->ClusterBits) - 1);
            Allocated = TRUE;
        }
        *Fresh = TRUE;

        /* fast path 1: extend the previous extent */
        if (Prev >= 0) {
            PEXT_EXTENT P = &Ex[Prev];
            ULONG Len = XLEN(P->Len);
            if (!XUNINIT(P->Len) && (ULONGLONG)P->Block + Len == FileBlk &&
                Len < EXT_EXTENT_INIT_MAX && XSTART(P) + Len == NewBlk) {
                P->Len = (USHORT)(Len + 1);
                goto patched;
            }
        }
        /* fast path 2: prepend to the next extent (not the first of a non-root leaf,
           whose index key in the parent would then be stale) */
        if ((ULONG)(Prev + 1) < n && (Buf.Host == NULL || Prev + 1 > 0)) {
            PEXT_EXTENT N = &Ex[Prev + 1];
            ULONG Len = XLEN(N->Len);
            if (!XUNINIT(N->Len) && N->Block == FileBlk + 1 && Len < EXT_EXTENT_INIT_MAX &&
                XSTART(N) == NewBlk + 1) {
                N->Block--; N->Len = (USHORT)(Len + 1);
                N->StartLo = (ULONG)NewBlk; N->StartHi = (USHORT)(NewBlk >> 32);
                goto patched;
            }
        }
        /* fast path 3: room in this leaf for a new entry */
        if (n < H->Max && (Buf.Host == NULL || Prev >= 0)) {
            ULONG At = (ULONG)(Prev + 1);
            for (i = n; i > At; i--) Ex[i] = Ex[i - 1];
            Ex[At].Block = (ULONG)FileBlk; Ex[At].Len = 1;
            Ex[At].StartLo = (ULONG)NewBlk; Ex[At].StartHi = (USHORT)(NewBlk >> 32);
            H->Entries = (USHORT)(n + 1);
            goto patched;
        }
    }

    if (Buf.Host) ExtHostUnmap(Vol, &Buf);
    S = XInsertSlow(Vol, Ino, Inode, FileBlk, NewBlk, Disk);
    if (!NT_SUCCESS(S) && Allocated) ExtFreeBlock(Vol, NewBlk);
    if (NeedSlow && NT_SUCCESS(S)) *Fresh = TRUE;      /* split uninit block: contents undefined */
    XCharge(Vol, Inode, Before);
    return S;

patched:
    *Disk = NewBlk;
    if (Buf.Host) {
        ExtExtentBlockCsum(Vol, Ino, Inode, Buf.Data);
        ExtSetDirty(Vol, &Buf);
        ExtHostUnmap(Vol, &Buf);
    }
    XCharge(Vol, Inode, Before);
    return EXT_OK;
}

/* ------------------------------------------------------------------ */
/* Truncation / freeing                                                */
/* ------------------------------------------------------------------ */

EXT_STATUS
ExtExtentTruncate(PEXT_VOL Vol, ULONG Ino, PEXT_INODE Inode, ULONGLONG NewBlocks)
{
    ULONGLONG Before = XSNAP(Vol);
    ULONGLONG Last = ~(ULONGLONG)0, KeepC = ~(ULONGLONG)0;
    XLIST L;
    ULONG i, w = 0;
    EXT_STATUS S = XLoad(Vol, Inode, &L);
    if (!NT_SUCCESS(S)) return S;

    /* bigalloc: the cluster holding the last kept block stays */
    if (Vol->ClusterBits && NewBlocks != 0) {
        ULONGLONG Lc0 = (NewBlocks - 1) & ~(((ULONGLONG)1 << Vol->ClusterBits) - 1);
        for (i = 0; i < L.Count; i++) {
            XENT *E = &L.E[i];
            ULONGLONG End = (ULONGLONG)E->Block + XLEN(E->Len);
            if (End > Lc0 && E->Block < NewBlocks) {
                ULONGLONG Lb = (End < NewBlocks ? End : NewBlocks) - 1;
                KeepC = (E->Start + (Lb - E->Block)) >> Vol->ClusterBits;
            }
        }
    }

    for (i = 0; i < L.Count; i++) {
        XENT *E = &L.E[i];
        ULONG Len = XLEN(E->Len);
        if (E->Block >= NewBlocks) {
            XFreeRunC(Vol, E->Start, Len, &Last, KeepC);
            continue;
        }
        if ((ULONGLONG)E->Block + Len > NewBlocks) {
            ULONG Keep = (ULONG)(NewBlocks - E->Block);
            XFreeRunC(Vol, E->Start + Keep, Len - Keep, &Last, KeepC);
            E->Len = XUNINIT(E->Len) ? Keep + EXT_EXTENT_INIT_MAX : Keep;
        }
        L.E[w++] = *E;
    }
    L.Count = w;
    S = XStore(Vol, Ino, Inode, &L, ExtGroupBase(Vol, Ino));
    XFreeList(&L);
    XCharge(Vol, Inode, Before);
    return S;
}

EXT_STATUS
ExtExtentFreeAll(PEXT_VOL Vol, ULONG Ino, PEXT_INODE Inode)
{
    XLIST L;
    ULONG i;
    EXT_STATUS S = XLoad(Vol, Inode, &L);
    if (!NT_SUCCESS(S)) return S;
    {
        ULONGLONG Last = ~(ULONGLONG)0;
        for (i = 0; i < L.Count; i++) XFreeRunC(Vol, L.E[i].Start, XLEN(L.E[i].Len), &Last, ~(ULONGLONG)0);
    }
    for (i = 0; i < L.NodeCount; i++) ExtFreeBlock(Vol, L.Node[i]);
    XFreeList(&L);
    ExtZero(Inode->Block, sizeof(Inode->Block));
    XROOT(Inode)->Magic = EXT_EXTENT_MAGIC;
    XROOT(Inode)->Max = (USHORT)XROOT_MAX;
    (VOID)Ino;
    return EXT_OK;
}

/* For the checker: makes Inode's tree anew from the given extents (sorted,
   not overlapping); the old tree blocks are not freed (the checker rebuilds
   the bitmaps itself) */
EXT_STATUS
ExtExtentRebuild(PEXT_VOL Vol, ULONG Ino, PEXT_INODE Inode, ULONG Count,
                 const ULONG *FileBlock, const ULONG *RawLen, const ULONGLONG *Start)
{
    XLIST L;
    ULONG i;
    EXT_STATUS S = EXT_OK;
    PEXT_EXTENT_HEADER Root = XROOT(Inode);

    ExtZero(&L, sizeof(L));
    for (i = 0; i < Count && NT_SUCCESS(S); i++) S = XAddExtent(&L, FileBlock[i], RawLen[i], Start[i]);
    if (NT_SUCCESS(S)) {
        ExtZero(Inode->Block, sizeof(Inode->Block));
        Root->Magic = EXT_EXTENT_MAGIC;
        Root->Max = (USHORT)XROOT_MAX;
        S = XStore(Vol, Ino, Inode, &L, Count ? Start[0] : ExtGroupBase(Vol, Ino));
    }
    XFreeList(&L);
    return S;
}

/* The extents of an inode as three arrays (ExtHostAlloc'd; free each with
   ExtHostFree). RawLen keeps the uninitialised flag. */
EXT_STATUS
ExtExtentGetList(PEXT_VOL Vol, PEXT_INODE Inode, PULONG Count, PULONG *Block, PULONG *RawLen, PULONGLONG *Start)
{
    XLIST L;
    ULONG i, n;
    EXT_STATUS S = XLoad(Vol, Inode, &L);
    if (!NT_SUCCESS(S)) return S;
    n = L.Count ? L.Count : 1;
    *Block = (PULONG)ExtHostAlloc(n * sizeof(ULONG));
    *RawLen = (PULONG)ExtHostAlloc(n * sizeof(ULONG));
    *Start = (PULONGLONG)ExtHostAlloc(n * sizeof(ULONGLONG));
    if (!*Block || !*RawLen || !*Start) {
        if (*Block) ExtHostFree(*Block);
        if (*RawLen) ExtHostFree(*RawLen);
        if (*Start) ExtHostFree(*Start);
        XFreeList(&L);
        return EXT_NO_MEMORY;
    }
    for (i = 0; i < L.Count; i++) {
        (*Block)[i] = L.E[i].Block;
        (*RawLen)[i] = L.E[i].Len;
        (*Start)[i] = L.E[i].Start;
    }
    *Count = L.Count;
    XFreeList(&L);
    return EXT_OK;
}

/* Replaces the extents of an inode by the given ones (same logical blocks,
   moved data); the tree blocks are reused or freed and charged to i_blocks.
   The data blocks themselves are the caller's to allocate and free. */
EXT_STATUS
ExtExtentRemap(PEXT_VOL Vol, ULONG Ino, PEXT_INODE Inode, ULONG Count,
               const ULONG *Block, const ULONG *RawLen, const ULONGLONG *Start)
{
    XLIST L;
    ULONG i;
    ULONGLONG Before;
    EXT_STATUS S = XLoad(Vol, Inode, &L);
    if (!NT_SUCCESS(S)) return S;
    L.Count = 0;
    for (i = 0; i < Count && NT_SUCCESS(S); i++) S = XAddExtent(&L, Block[i], RawLen[i], Start[i]);
    if (NT_SUCCESS(S)) {
        XNormalize(&L);
        Before = XSNAP(Vol);
        S = XStore(Vol, Ino, Inode, &L, Count ? Start[0] : ExtGroupBase(Vol, Ino));
        XCharge(Vol, Inode, Before);
    }
    XFreeList(&L);
    return S;
}
