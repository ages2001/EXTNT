/*
 * EXTNT - the FAT16 view that lets Windows NT start from an ext volume
 *
 * NTLDR reads only FAT, NTFS (and on 3.x HPFS). So the files it needs are
 * also given a FAT16 view, kept inside the ext volume:
 *
 *   sectors 0-1     the EXTNT boot loader with a FAT16 BPB (the 1 KB ext
 *                   leaves free before its superblock)
 *   boot area       the blocks of inode 5, the ext "boot loader inode"
 *                   (extfmt /BOOT reserves it low on the disk): the FAT,
 *                   the root directory, then the FAT subdirectories
 *   clusters        cluster n is ext block Base + n - 2, where Base is the
 *                   block after the root directory: a FAT file's clusters
 *                   are the very blocks of the ext file, so the view always
 *                   shows the current contents
 *
 * The view holds NTLDR, NTDETECT.COM, BOOT.INI (and the other loader files
 * of the root), and from the Windows directory what the loader reads: the
 * kernel and HAL, the SYSTEM hive, the NLS tables and fonts, and every
 * driver in SYSTEM32\DRIVERS. Files lying outside the 65518 blocks the FAT
 * can reach are moved inside. Clusters the view does not use are marked
 * bad, so a FAT driver would never put anything there. Each FAT entry keeps
 * its ext inode number (in the creation time fields), which lets the
 * driver bring the view up to date when one of these files changes.
 */

#ifdef EXT_KERNEL
#include "ext.h"
#else
#include "extusr.h"
#include "extsup.h"
#endif
#include "extshim.h"

/* The boot area: FAT then a 16-sector root directory (as extfmt /BOOT lays it out) */
#ifndef EXT_BOOT_FAT_SECTORS
#define EXT_BOOT_FAT_SECTORS    (256 + 16)
#endif

typedef struct _SFILE {
    UCHAR       Name[11];
    UCHAR       Case;               /* NT case flags */
    UCHAR       Attr;
    ULONG       Ino;
    ULONG       Size;
    ULONG       Mtime;
    int         Dir;                /* subdirectory index, or -1 */
    ULONG       First;              /* first cluster */
} SFILE;

typedef struct _SDIR {
    ULONG       Ino;
    int         Parent;
    SFILE      *F;
    ULONG       Count, Cap;
    ULONG       First;              /* first cluster (0 = the root) */
    ULONG       Clusters;
} SDIR;

#define SHIM_MAX_DIRS   16

typedef struct _SHIM {
    PEXT_SHIM_HOST H;
    PEXT_VOL    Vol;
    PEXT_SHIM_RESULT R;
    ULONG       Bs, Spb;
    ULONGLONG   A, Base, WinEnd;    /* boot area, first cluster block, end of the files' window */
    ULONGLONG   FatEnd;             /* end of what the FAT describes */
    ULONG       K, M;               /* FAT + root blocks, boot area blocks */
    ULONG       NextDirBlock;
    SDIR        D[SHIM_MAX_DIRS];
    ULONG       NDirs;
    USHORT     *Fat;
    ULONG       Clusters;
} SHIM;

/* ------------------------------------------------------------------ */
/* Small helpers (no C library: this file is built into the driver)    */
/* ------------------------------------------------------------------ */

static PVOID
ShAlloc(ULONG Bytes)
{
    PVOID P = ExtHostAlloc(Bytes);
    if (P) ExtZero(P, Bytes);
    return P;
}

static VOID
ShFree(PVOID P)
{
    if (P) ExtHostFree(P);
}

/* Grows an array to hold Need elements */
static BOOLEAN
ShGrow(PVOID *Arr, PULONG Cap, ULONG Need, ULONG Elem)
{
    ULONG N;
    PVOID X;
    if (Need <= *Cap) return TRUE;
    N = *Cap ? *Cap : 16;
    while (N < Need) N *= 2;
    X = ShAlloc(N * Elem);
    if (!X) return FALSE;
    if (*Arr) { ExtCopy(X, *Arr, *Cap * Elem); ExtHostFree(*Arr); }
    *Arr = X;
    *Cap = N;
    return TRUE;
}

static VOID
ShFill(PVOID P, int C, ULONG N)
{
    PUCHAR B = (PUCHAR)P;
    while (N--) *B++ = (UCHAR)C;
}

static int
ShCmp(const VOID *A, const VOID *B, ULONG N)
{
    return ExtMemEq(A, B, N) ? 0 : 1;
}

static ULONG
ShLen(const char *S)
{
    ULONG N = 0;
    while (S[N]) N++;
    return N;
}

static const char *
ShChr(const char *S, char C)
{
    for (; *S; S++) if (*S == C) return S;
    return 0;
}

static int
Fail(SHIM *S, const char *Msg)
{
    if (!S->R->Error) S->R->Error = Msg;
    return -1;
}

static VOID
FailName(SHIM *S, const char *Msg, const UCHAR *Name)
{
    if (S->R->Error) return;
    S->R->Error = Msg;
    ExtCopy(S->R->ErrorName, Name, 11);
    S->R->ErrorName[11] = 0;
}

/* Disk access in pieces of at most 32 sectors (the driver bounces them through nonpaged pool) */
static int
ShRead(SHIM *S, ULONGLONG Sector, ULONG Count, PVOID Buf)
{
    PUCHAR P = (PUCHAR)Buf;
    while (Count) {
        ULONG n = Count > 32 ? 32 : Count;
        if (!NT_SUCCESS(S->H->Read(S->H->Ctx, Sector, n, P))) return -1;
        Sector += n; Count -= n; P += n * 512;
    }
    return 0;
}

static int
ShWrite(SHIM *S, ULONGLONG Sector, ULONG Count, const VOID *Buf)
{
    const UCHAR *P = (const UCHAR *)Buf;
    while (Count) {
        ULONG n = Count > 32 ? 32 : Count;
        if (!NT_SUCCESS(S->H->Write(S->H->Ctx, Sector, n, P))) return -1;
        Sector += n; Count -= n; P += n * 512;
    }
    return 0;
}

static VOID
ShCommit(SHIM *S)
{
    if (S->H->Commit) (VOID)S->H->Commit(S->H->Ctx);
}

BOOLEAN
ExtShimPresent(const UCHAR *Sector0)
{
    return (BOOLEAN)(ExtMemEq(Sector0 + 3, EXT_SHIM_OEM, 8) && Sector0[510] == 0x55 && Sector0[511] == 0xAA);
}

/* ------------------------------------------------------------------ */
/* The files of the view                                               */
/* ------------------------------------------------------------------ */

/* An ext name as a FAT 8.3 name (upper case, space padded); FALSE if it is not one */
static BOOLEAN
To83(const char *Name, ULONG Len, UCHAR *Out, UCHAR *Case)
{
    static const char Ok[] = "$%'-_@~`!(){}^#&";
    ULONG i, Dot = Len, n;
    BOOLEAN BaseLower = FALSE, BaseUpper = FALSE, ExtLower = FALSE, ExtUpper = FALSE;
    ShFill(Out, ' ', 11);
    *Case = 0;
    if (Len == 0 || Name[0] == '.') return FALSE;
    for (i = 0; i < Len; i++) if (Name[i] == '.') { if (Dot != Len) return FALSE; Dot = i; }
    if (Dot == 0 || Dot > 8 || (Dot < Len && Len - Dot - 1 > 3) || Dot == Len - 1) return FALSE;
    for (i = 0, n = 0; i < Len; i++) {
        char C = Name[i];
        BOOLEAN InExt = (BOOLEAN)(i > Dot);
        if (i == Dot) continue;
        if (C >= 'a' && C <= 'z') { if (InExt) ExtLower = TRUE; else BaseLower = TRUE; C = (char)(C - 32); }
        else if (C >= 'A' && C <= 'Z') { if (InExt) ExtUpper = TRUE; else BaseUpper = TRUE; }
        else if (!(C >= '0' && C <= '9') && !ShChr(Ok, C)) return FALSE;
        Out[InExt ? 8 + (i - Dot - 1) : n++] = (UCHAR)C;
    }
    if (BaseLower && !BaseUpper) *Case |= 0x08;
    if (ExtLower && !ExtUpper) *Case |= 0x10;
    return TRUE;
}

static BOOLEAN
Match(const char *Name, ULONG Len, const char *Want)
{
    ULONG i;
    if (ShLen(Want) != Len) return FALSE;
    for (i = 0; i < Len; i++) {
        char a = Name[i], b = Want[i];
        if (a >= 'a' && a <= 'z') a = (char)(a - 32);
        if (b >= 'a' && b <= 'z') b = (char)(b - 32);
        if (a != b) return FALSE;
    }
    return TRUE;
}

/* Wildcard match of an 8.3 pattern ("HAL*.DLL") against an ext name, no case */
static BOOLEAN
Pattern(const char *Name, ULONG Len, const char *Pat)
{
    if (*Pat == 0) return (BOOLEAN)(Len == 0);
    if (*Pat == '*') {
        ULONG i;
        for (i = 0; i <= Len; i++) if (Pattern(Name + i, Len - i, Pat + 1)) return TRUE;
        return FALSE;
    }
    if (Len == 0) return FALSE;
    {
        char a = *Name, b = *Pat;
        if (a >= 'a' && a <= 'z') a = (char)(a - 32);
        if (b >= 'a' && b <= 'z') b = (char)(b - 32);
        if (a != b && b != '?') return FALSE;
    }
    return Pattern(Name + 1, Len - 1, Pat + 1);
}

/* Finds Name (any case) in directory Dir */
static ULONG
Find(SHIM *S, ULONG Dir, const char *Name, BOOLEAN WantDir)
{
    EXT_INODE I;
    EXT_ENTRY E;
    ULONGLONG Cookie = 0;
    if (!NT_SUCCESS(ExtReadInode(S->Vol, Dir, &I))) return 0;
    while (NT_SUCCESS(ExtEnum(S->Vol, Dir, &I, Cookie, &E))) {
        Cookie = E.Cookie;
        if (Match(E.Name, E.NameLength, Name)) {
            EXT_INODE C;
            if (!NT_SUCCESS(ExtReadInode(S->Vol, E.Inode, &C))) return 0;
            if (((C.Mode & EXT_S_IFMT) == EXT_S_IFDIR) != WantDir) return 0;
            return E.Inode;
        }
    }
    return 0;
}

static int
AddEntry(SHIM *S, int Dir, const char *Name, ULONG NameLen, ULONG Ino, int SubDir)
{
    SDIR *D = &S->D[Dir];
    SFILE *F;
    EXT_INODE I;
    ULONG k;
    UCHAR N83[11], Case;
    if (!To83(Name, NameLen, N83, &Case)) return 0;
    for (k = 0; k < D->Count; k++) if (!ShCmp(D->F[k].Name, N83, 11)) return 0;   /* two ext names, one 8.3 */
    if (!NT_SUCCESS(ExtReadInode(S->Vol, Ino, &I))) return 0;
    if (SubDir < 0 && ExtInodeSize(S->Vol, &I) >= 0xFFFFFFFFULL) return 0;
    if (!ShGrow((PVOID *)&D->F, &D->Cap, D->Count + 1, sizeof(SFILE))) return -1;
    F = &D->F[D->Count++];
    ShFill(F, 0, sizeof(*F));
    ExtCopy(F->Name, N83, 11);
    F->Case = Case;
    F->Ino = Ino;
    F->Dir = SubDir;
    F->Attr = (UCHAR)(SubDir >= 0 ? 0x10 : 0x20);
    F->Size = SubDir >= 0 ? 0 : (ULONG)ExtInodeSize(S->Vol, &I);
    F->Mtime = I.Mtime;
    return 1;
}

static int
NewDir(SHIM *S, int Parent, const char *Name, ULONG Ino)
{
    int Idx;
    if (S->NDirs == sizeof(S->D) / sizeof(S->D[0])) return -1;
    Idx = (int)S->NDirs++;
    ShFill(&S->D[Idx], 0, sizeof(SDIR));
    S->D[Idx].Ino = Ino;
    S->D[Idx].Parent = Parent;
    if (Parent >= 0 && AddEntry(S, Parent, Name, (ULONG)ShLen(Name), Ino, Idx) <= 0) return -1;
    return Idx;
}

/* Every file of ext directory Ino whose name matches one of the patterns (NULL = all) */
static int
AddFiles(SHIM *S, int Dir, ULONG Ino, const char *const *Pats)
{
    EXT_INODE I;
    EXT_ENTRY E;
    ULONGLONG Cookie = 0;
    if (!NT_SUCCESS(ExtReadInode(S->Vol, Ino, &I))) return -1;
    while (NT_SUCCESS(ExtEnum(S->Vol, Ino, &I, Cookie, &E))) {
        EXT_INODE C;
        int k, Take = (Pats == NULL);
        Cookie = E.Cookie;
        if (E.Name[0] == '.' ) continue;
        for (k = 0; Pats && Pats[k] && !Take; k++) if (Pattern(E.Name, E.NameLength, Pats[k])) Take = 1;
        if (!Take) continue;
        if (!NT_SUCCESS(ExtReadInode(S->Vol, E.Inode, &C)) || (C.Mode & EXT_S_IFMT) != EXT_S_IFREG) continue;
        if (AddEntry(S, Dir, E.Name, E.NameLength, E.Inode, -1) < 0) return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Files inside the window                                             */
/* ------------------------------------------------------------------ */

#define MOVE_IN     1       /* into the window */
#define MOVE_OUT    0       /* out of the window */

static BOOLEAN
Misplaced(SHIM *S, ULONGLONG Blk, int Dir)
{
    BOOLEAN In = (BOOLEAN)(Blk >= S->Base && Blk < S->WinEnd);
    return (BOOLEAN)(Dir == MOVE_IN ? !In : In);
}

/* TRUE if every block of the file is mapped (no hole, nothing uninitialised) inside the window */
static BOOLEAN
InWindow(SHIM *S, ULONG Ino)
{
    EXT_INODE I;
    ULONGLONG N, B, Disk;
    ULONG Run;
    if (!NT_SUCCESS(ExtReadInode(S->Vol, Ino, &I))) return FALSE;
    if (I.Flags & EXT_IF_INLINE_DATA) return FALSE;
    N = (ExtInodeSize(S->Vol, &I) + S->Bs - 1) / S->Bs;
    for (B = 0; B < N; B += Run) {
        Run = 0;
        if (!NT_SUCCESS(ExtMapFileBlock(S->Vol, Ino, &I, B, &Disk, &Run)) || Disk == 0 || Run == 0) return FALSE;
        if (Run > N - B) Run = (ULONG)(N - B);
        if (Disk < S->Base || Disk + Run > S->WinEnd) return FALSE;
    }
    return TRUE;
}

/* Blocks of the file that would have to move into the window (holes count) */
static ULONGLONG
BlocksToMove(SHIM *S, ULONG Ino)
{
    EXT_INODE I;
    ULONGLONG N, B, Disk, Need = 0, k;
    ULONG Run;
    if (!NT_SUCCESS(ExtReadInode(S->Vol, Ino, &I))) return 0;
    N = (ExtInodeSize(S->Vol, &I) + S->Bs - 1) / S->Bs;
    if (I.Flags & EXT_IF_INLINE_DATA) return N;
    for (B = 0; B < N; B += Run) {
        Run = 0;
        if (!NT_SUCCESS(ExtMapFileBlock(S->Vol, Ino, &I, B, &Disk, &Run)) || Run == 0) return N;
        if (Run > N - B) Run = (ULONG)(N - B);
        if (Disk == 0) { Need += Run; continue; }
        for (k = 0; k < Run; k++) if (Misplaced(S, Disk + k, MOVE_IN)) Need++;
    }
    if (!(I.Flags & EXT_IF_EXTENTS)) Need += N / (S->Bs / 4) + 2;     /* indirect blocks */
    return Need;
}

/* Free blocks inside the window */
static ULONGLONG
WindowFree(SHIM *S)
{
    ULONGLONG Free = 0, First, b;
    ULONG g;
    for (g = 0; g < S->Vol->GroupCount; g++) {
        EXT_BUF Buf;
        BOOLEAN Uninit;
        First = ExtGroupFirstBlock(S->Vol, g);
        if (First >= S->WinEnd) break;
        if (First + S->Vol->BlocksPerGroup <= S->Base) continue;
        if (!NT_SUCCESS(ExtMapBlockBitmap(S->Vol, g, &Buf, &Uninit))) continue;
        for (b = 0; b < S->Vol->BlocksPerGroup && First + b < S->Vol->BlocksCount; b++) {
            ULONGLONG Blk = First + b;
            if (Blk < S->Base || Blk >= S->WinEnd) continue;
            if (!(Buf.Data[b >> 3] & (1 << (b & 7)))) Free++;
        }
        ExtHostUnmap(S->Vol, &Buf);
    }
    return Free;
}

/* A free block inside the window: the allocator is shown only the window */
static EXT_STATUS
AllocIn(SHIM *S, ULONGLONG Near, PULONGLONG Block)
{
    ULONGLONG Count = S->Vol->BlocksCount, Held[64];
    ULONG Groups = S->Vol->GroupCount, NHeld = 0, k;
    EXT_STATUS St;
    if (Near < S->Base || Near >= S->WinEnd) Near = S->Base;
    for (;;) {
        S->Vol->BlocksCount = S->WinEnd;
        S->Vol->GroupCount = (ULONG)((S->WinEnd - S->Vol->FirstDataBlock + S->Vol->BlocksPerGroup - 1) / S->Vol->BlocksPerGroup);
        St = ExtAllocBlock(S->Vol, Near, Block);
        S->Vol->BlocksCount = Count;
        S->Vol->GroupCount = Groups;
        if (!NT_SUCCESS(St) || *Block >= S->Base) break;
        if (NHeld == 64) { ExtFreeBlock(S->Vol, *Block); St = EXT_DISK_FULL; break; }
        Held[NHeld++] = *Block;             /* below the clusters: keep it from coming back */
        Near = S->Base;
    }
    for (k = 0; k < NHeld; k++) ExtFreeBlock(S->Vol, Held[k]);
    return St;
}

/* A free block outside the window */
static EXT_STATUS
AllocOut(SHIM *S, ULONGLONG Near, PULONGLONG Block)
{
    int Try;
    if (Near < S->WinEnd) Near = S->WinEnd;
    for (Try = 0; Try < 2; Try++) {
        EXT_STATUS St;
        if (Near >= S->Vol->BlocksCount) Near = S->WinEnd;
        St = ExtAllocBlock(S->Vol, Near, Block);
        if (!NT_SUCCESS(St)) return St;
        if (!Misplaced(S, *Block, MOVE_OUT)) return EXT_OK;
        ExtFreeBlock(S->Vol, *Block);
        /* the group holding the window's end is full past it: the next one */
        Near = ExtGroupFirstBlock(S->Vol, (ULONG)((S->WinEnd - S->Vol->FirstDataBlock) / S->Vol->BlocksPerGroup) + 1);
    }
    return EXT_DISK_FULL;
}

/* Points file block FileBlk of an indirect-mapped file at NewBlk */
static EXT_STATUS
IndSet(SHIM *S, PEXT_INODE I, ULONGLONG FileBlk, ULONGLONG NewBlk)
{
    ULONG P = S->Bs / 4, Path[4], Depth, Ptr, d;
    ULONGLONG b = FileBlk;
    if (b < 12) { I->Block[b] = (ULONG)NewBlk; return EXT_OK; }
    b -= 12;
    if (b < P) { Depth = 1; Ptr = I->Block[12]; Path[0] = (ULONG)b; }
    else {
        b -= P;
        if (b < (ULONGLONG)P * P) { Depth = 2; Ptr = I->Block[13]; Path[0] = (ULONG)(b / P); Path[1] = (ULONG)(b % P); }
        else {
            b -= (ULONGLONG)P * P;
            Depth = 3; Ptr = I->Block[14];
            Path[0] = (ULONG)(b / ((ULONGLONG)P * P)); Path[1] = (ULONG)((b / P) % P); Path[2] = (ULONG)(b % P);
        }
    }
    for (d = 0; d < Depth; d++) {
        EXT_BUF Buf;
        PULONG Arr;
        EXT_STATUS St;
        if (Ptr == 0) return EXT_FILE_CORRUPT;
        St = ExtMapBlocks(S->Vol, Ptr, 1, d + 1 == Depth ? EXT_MAP_WRITE : EXT_MAP_READ, &Buf);
        if (!NT_SUCCESS(St)) return St;
        Arr = (PULONG)Buf.Data;
        if (d + 1 == Depth) {
            Arr[Path[d]] = (ULONG)NewBlk;
            ExtSetDirty(S->Vol, &Buf);
        } else {
            Ptr = Arr[Path[d]];
        }
        ExtHostUnmap(S->Vol, &Buf);
    }
    return EXT_OK;
}

/* Copies Count blocks */
static EXT_STATUS
CopyData(SHIM *S, ULONGLONG From, ULONGLONG To, ULONG Count, PUCHAR Tmp)
{
    if (ShRead(S, From * S->Spb, Count * S->Spb, Tmp) != 0) return EXT_IO_ERROR;
    if (ShWrite(S, To * S->Spb, Count * S->Spb, Tmp) != 0) return EXT_IO_ERROR;
    return EXT_OK;
}


/*
 * Moves the misplaced blocks of a file into (Dir = MOVE_IN) or out of the
 * window: new blocks, the data copied, the block map pointed at them, the old
 * blocks freed. Returns the blocks moved, or -1.
 */
static LONGLONG
MoveBlocks(SHIM *S, ULONG Ino, int Dir)
{
    EXT_INODE I;
    ULONGLONG N, Moved = 0;
    PUCHAR Tmp;
    PULONGLONG Old = NULL;
    ULONG NOld = 0, COld = 0, k;
    EXT_STATUS St = EXT_OK;
    ULONGLONG Near = Dir == MOVE_IN ? S->Base : S->WinEnd;
    ULONG Chunk = S->H->CopyBlocks ? S->H->CopyBlocks : 1;
    BOOLEAN Touched = FALSE;

    if (!NT_SUCCESS(ExtReadInode(S->Vol, Ino, &I))) return -1;
    if (Dir == MOVE_IN) {
        if (I.Flags & EXT_IF_INLINE_DATA) {
            Touched = TRUE;
            if (!NT_SUCCESS(ExtUninline(S->Vol, Ino, &I)) || !NT_SUCCESS(ExtReadInode(S->Vol, Ino, &I))) {
                if (S->H->Moved) S->H->Moved(S->H->Ctx, Ino);
                return -1;
            }
        }
        /* holes and fallocated parts: written out as zeros first */
        {
            ULONGLONG B, Disk;
            ULONG Run;
            N = (ExtInodeSize(S->Vol, &I) + S->Bs - 1) / S->Bs;
            for (B = 0; B < N; B += Run) {
                Run = 0;
                if (!NT_SUCCESS(ExtMapFileBlock(S->Vol, Ino, &I, B, &Disk, &Run)) || Run == 0) return -1;
                if (Disk == 0) break;
            }
            if (B < N) {
                ULONGLONG Size = ExtInodeSize(S->Vol, &I);
                PUCHAR Data = (PUCHAR)ShAlloc((ULONG)Size);
                ULONG Got = 0, Put = 0;
                if (!Data) return -1;
                Touched = TRUE;
                St = ExtReadFile(S->Vol, Ino, &I, 0, (ULONG)Size, Data, &Got);
                if (NT_SUCCESS(St)) St = ExtWriteFile(S->Vol, Ino, &I, 0, (ULONG)Size, Data, &Put);
                if (NT_SUCCESS(St)) St = ExtWriteInode(S->Vol, Ino, &I);
                ShFree(Data);
                if (!NT_SUCCESS(St) || !NT_SUCCESS(ExtReadInode(S->Vol, Ino, &I))) {
                    if (S->H->Moved) S->H->Moved(S->H->Ctx, Ino);
                    return -1;
                }
            }
        }
    } else if (I.Flags & EXT_IF_INLINE_DATA) {
        return 0;
    }
    if (ExtIsFastSymlink(S->Vol, &I)) return 0;
    Tmp = (PUCHAR)ShAlloc(S->Bs * Chunk);
    if (!Tmp) return -1;

    if (I.Flags & EXT_IF_EXTENTS) {
        ULONG Count, *Blk, *Raw, NCount = 0, NCap;
        ULONGLONG *Start;
        ULONG *NBlk, *NRaw;
        ULONGLONG *NStart;
        St = ExtExtentGetList(S->Vol, &I, &Count, &Blk, &Raw, &Start);
        if (!NT_SUCCESS(St)) { ShFree(Tmp); return -1; }
        NCap = Count * 2 + 16;
        NBlk = (ULONG *)ShAlloc(NCap * sizeof(ULONG));
        NRaw = (ULONG *)ShAlloc(NCap * sizeof(ULONG));
        NStart = (ULONGLONG *)ShAlloc(NCap * sizeof(ULONGLONG));
        if (!NBlk || !NRaw || !NStart) St = EXT_NO_MEMORY;
        for (k = 0; k < Count && NT_SUCCESS(St); k++) {
            ULONG Uninit = Raw[k] > 32768, Len = Uninit ? Raw[k] - 32768 : Raw[k], j = 0;
            while (j < Len && NT_SUCCESS(St)) {
                /* a stretch of blocks that stay, or of blocks that move */
                BOOLEAN Mis = Misplaced(S, Start[k] + j, Dir);
                ULONG e = j, Piece;
                ULONGLONG To = 0;
                while (e < Len && Misplaced(S, Start[k] + e, Dir) == Mis) e++;
                while (j < e && NT_SUCCESS(St)) {
                    Piece = 0;
                    if (!Mis) { Piece = e - j; To = Start[k] + j; }
                    else {
                        /* new blocks, contiguous as far as they come */
                        while (j + Piece < e && Piece < Chunk) {
                            ULONGLONG Got;
                            St = Dir == MOVE_IN ? AllocIn(S, Near, &Got) : AllocOut(S, Near, &Got);
                            if (!NT_SUCCESS(St)) break;
                            if (Piece && Got != To + Piece) { ExtFreeBlock(S->Vol, Got); break; }
                            if (!Piece) To = Got;
                            Piece++;
                            Near = Got + 1;
                        }
                        if (Piece == 0) break;
                        St = CopyData(S, Start[k] + j, To, Piece, Tmp);
                        if (!NT_SUCCESS(St)) break;
                        if (!ShGrow((PVOID *)&Old, &COld, NOld + Piece, sizeof(ULONGLONG))) { St = EXT_NO_MEMORY; break; }
                        { ULONG q; for (q = 0; q < Piece; q++) Old[NOld++] = Start[k] + j + q; }
                        Moved += Piece;
                    }
                    if (NCount == NCap) {
                        ULONG C1 = NCap, C2 = NCap, C3 = NCap;
                        if (!ShGrow((PVOID *)&NBlk, &C1, NCap + 1, sizeof(ULONG)) ||
                            !ShGrow((PVOID *)&NRaw, &C2, NCap + 1, sizeof(ULONG)) ||
                            !ShGrow((PVOID *)&NStart, &C3, NCap + 1, sizeof(ULONGLONG))) { St = EXT_NO_MEMORY; break; }
                        NCap = C1;
                    }
                    NBlk[NCount] = Blk[k] + j;
                    NRaw[NCount] = Uninit ? Piece + 32768 : Piece;
                    NStart[NCount] = To;
                    NCount++;
                    j += Piece;
                }
            }
        }
        if (NT_SUCCESS(St) && Moved) St = ExtExtentRemap(S->Vol, Ino, &I, NCount, NBlk, NRaw, NStart);
        ExtHostFree(Blk); ExtHostFree(Raw); ExtHostFree(Start);
        ShFree(NBlk); ShFree(NRaw); ShFree(NStart);
    } else {
        ULONGLONG B, Disk;
        ULONG Run;
        N = (ExtInodeSize(S->Vol, &I) + S->Bs - 1) / S->Bs;
        for (B = 0; B < N && NT_SUCCESS(St); B += Run) {
            ULONG q;
            Run = 0;
            St = ExtMapFileBlock(S->Vol, Ino, &I, B, &Disk, &Run);
            if (!NT_SUCCESS(St) || Run == 0) { if (NT_SUCCESS(St)) St = EXT_FILE_CORRUPT; break; }
            if (Run > N - B) Run = (ULONG)(N - B);
            if (Disk == 0) continue;
            for (q = 0; q < Run && NT_SUCCESS(St); q++) {
                ULONGLONG To;
                if (!Misplaced(S, Disk + q, Dir)) continue;
                St = Dir == MOVE_IN ? AllocIn(S, Near, &To) : AllocOut(S, Near, &To);
                if (!NT_SUCCESS(St)) break;
                Near = To + 1;
                St = CopyData(S, Disk + q, To, 1, Tmp);
                if (NT_SUCCESS(St)) St = IndSet(S, &I, B + q, To);
                if (!NT_SUCCESS(St)) { ExtFreeBlock(S->Vol, To); break; }
                if (!ShGrow((PVOID *)&Old, &COld, NOld + 1, sizeof(ULONGLONG))) { St = EXT_NO_MEMORY; break; }
                Old[NOld++] = Disk + q;
                Moved++;
            }
        }
    }
    if (NT_SUCCESS(St) && Moved) St = ExtWriteInode(S->Vol, Ino, &I);
    if (NT_SUCCESS(St)) {
        for (k = 0; k < NOld; k++) ExtFreeBlock(S->Vol, Old[k]);
    }
    ShFree(Old);
    ShFree(Tmp);
    if ((Moved || Touched) && S->H->Moved) S->H->Moved(S->H->Ctx, Ino);
    return NT_SUCCESS(St) ? (LONGLONG)Moved : -1;
}

static BOOLEAN
IsBootFile(SHIM *S, ULONG Ino)
{
    ULONG d, k;
    for (d = 0; d < S->NDirs; d++) {
        if (S->D[d].Ino == Ino) return TRUE;
        for (k = 0; k < S->D[d].Count; k++) if (S->D[d].F[k].Ino == Ino) return TRUE;
    }
    return FALSE;
}

/* Moves other files out of the window until Need blocks are free in it */
static int
MakeRoom(SHIM *S, ULONGLONG Need)
{
    ULONGLONG Free = WindowFree(S);
    ULONG g;
    if (Free >= Need) return 0;
    for (g = 0; g < S->Vol->GroupCount && Free < Need; g++) {
        PEXT_GROUP_DESC G = ExtGroupDesc(S->Vol, g);
        EXT_BUF Bm;
        ULONG i, Used = S->Vol->InodesPerGroup;
        PUCHAR Bits;
        if (G->Flags & EXT_BG_INODE_UNINIT) continue;
        if (S->Vol->HasGdtCsum || S->Vol->HasMetaCsum) {
            ULONG Unused = G->ItableUnusedLo | (S->Vol->DescSize >= 64 ? ((ULONG)G->ItableUnusedHi << 16) : 0);
            Used = Unused < Used ? Used - Unused : 0;
        }
        if (!NT_SUCCESS(ExtMapBlocks(S->Vol, ExtGdInodeBitmap(S->Vol, g), 1, EXT_MAP_READ, &Bm))) continue;
        Bits = (PUCHAR)ShAlloc(S->Bs);
        if (!Bits) { ExtHostUnmap(S->Vol, &Bm); return -1; }
        ExtCopy(Bits, Bm.Data, S->Bs);
        ExtHostUnmap(S->Vol, &Bm);
        for (i = 0; i < Used && Free < Need; i++) {
            ULONG Ino = g * S->Vol->InodesPerGroup + i + 1;
            EXT_INODE I;
            LONGLONG M;
            USHORT Fmt;
            if (!(Bits[i >> 3] & (1 << (i & 7)))) continue;
            if ((Ino < S->Vol->FirstIno && Ino != EXT_ROOT_INO) || Ino == S->Vol->JournalInum) continue;
            if (IsBootFile(S, Ino)) continue;
            if (S->H->CanMove && !S->H->CanMove(S->H->Ctx, Ino)) continue;
            if (!NT_SUCCESS(ExtReadInode(S->Vol, Ino, &I)) || I.LinksCount == 0) continue;
            Fmt = (USHORT)(I.Mode & EXT_S_IFMT);
            if (Fmt != EXT_S_IFREG && Fmt != EXT_S_IFDIR && Fmt != EXT_S_IFLNK) continue;
            M = MoveBlocks(S, Ino, MOVE_OUT);
            if (M < 0) {
                continue;
            }
            if (M) { Free += (ULONGLONG)M; S->R->MovedOut++; }
            ShCommit(S);
        }
        ShFree(Bits);
    }
    Free = WindowFree(S);
    return Free >= Need ? 0 : -1;
}

/* Chains a file's blocks in the FAT; returns its first cluster */
static ULONG
Chain(SHIM *S, ULONG Ino)
{
    EXT_INODE I;
    ULONGLONG N, B, Disk;
    ULONG Run, Prev = 0, First = 0, k;
    if (!NT_SUCCESS(ExtReadInode(S->Vol, Ino, &I))) return 0;
    N = (ExtInodeSize(S->Vol, &I) + S->Bs - 1) / S->Bs;
    for (B = 0; B < N; B += Run) {
        Run = 0;
        if (!NT_SUCCESS(ExtMapFileBlock(S->Vol, Ino, &I, B, &Disk, &Run)) || Disk == 0 || Run == 0) return 0;
        if (Run > N - B) Run = (ULONG)(N - B);
        for (k = 0; k < Run; k++) {
            ULONG C = (ULONG)(Disk + k - S->Base) + 2;
            if (Prev) S->Fat[Prev] = (USHORT)C; else First = C;
            Prev = C;
        }
    }
    if (Prev) S->Fat[Prev] = 0xFFFF;
    return First;
}

/* ------------------------------------------------------------------ */
/* Writing the view                                                    */
/* ------------------------------------------------------------------ */

static void
DosTime(ULONG Unix, PUCHAR Time, PUCHAR Date)
{
    ULONG Days = Unix / 86400, Secs = Unix % 86400, Y = 1970, M, D;
    static const UCHAR Ml[12] = { 31,28,31,30,31,30,31,31,30,31,30,31 };
    for (;;) {
        ULONG Yd = ((Y % 4 == 0 && Y % 100 != 0) || Y % 400 == 0) ? 366 : 365;
        if (Days < Yd) break;
        Days -= Yd; Y++;
    }
    for (M = 0; M < 12; M++) {
        ULONG L = Ml[M] + (M == 1 && ((Y % 4 == 0 && Y % 100 != 0) || Y % 400 == 0));
        if (Days < L) break;
        Days -= L;
    }
    D = Days + 1;
    if (Y < 1980) { Y = 1980; M = 0; D = 1; }
    {
        ULONG T = ((Secs / 3600) << 11) | (((Secs / 60) % 60) << 5) | ((Secs % 60) / 2);
        ULONG Dt = ((Y - 1980) << 9) | ((M + 1) << 5) | D;
        Time[0] = (UCHAR)T; Time[1] = (UCHAR)(T >> 8);
        Date[0] = (UCHAR)Dt; Date[1] = (UCHAR)(Dt >> 8);
    }
}


static void
PutEntry(PUCHAR E, const UCHAR *Name, UCHAR Attr, UCHAR Case, ULONG Ino, ULONG Mtime, ULONG Cluster, ULONG Size)
{
    ShFill(E, 0, 32);
    ExtCopy(E, Name, 11);
    E[11] = Attr;
    E[12] = Case;
    E[14] = (UCHAR)Ino; E[15] = (UCHAR)(Ino >> 8); E[16] = (UCHAR)(Ino >> 16); E[17] = (UCHAR)(Ino >> 24);
    DosTime(Mtime, E + 22, E + 24);
    E[18] = E[24]; E[19] = E[25];
    E[26] = (UCHAR)Cluster; E[27] = (UCHAR)(Cluster >> 8);
    E[28] = (UCHAR)Size; E[29] = (UCHAR)(Size >> 8); E[30] = (UCHAR)(Size >> 16); E[31] = (UCHAR)(Size >> 24);
}

/* The entries of directory d (with "." and ".." for subdirectories) */
static PUCHAR
DirImage(SHIM *S, ULONG d, PULONG Bytes)
{
    SDIR *D = &S->D[d];
    ULONG n = D->Count + (d ? 2 : 0), Size, k, Pos = 0;
    PUCHAR Img;
    static const UCHAR Dot[11] = { '.', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ' };
    static const UCHAR DotDot[11] = { '.', '.', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ' };
    Size = d ? ((n * 32 + S->Bs - 1) / S->Bs) * S->Bs : 256 * 32;
    Img = (PUCHAR)ShAlloc(Size);
    if (!Img) return NULL;
    if (d) {
        PutEntry(Img, Dot, 0x10, 0, D->Ino, 0, D->First, 0);
        PutEntry(Img + 32, DotDot, 0x10, 0, S->D[D->Parent].Ino, 0, S->D[D->Parent].First, 0);
        Pos = 64;
    }
    for (k = 0; k < D->Count; k++, Pos += 32) {
        SFILE *F = &D->F[k];
        PutEntry(Img + Pos, F->Name, F->Attr, F->Case, F->Ino, F->Mtime,
                 F->Dir >= 0 ? S->D[F->Dir].First : F->First, F->Size);
    }
    *Bytes = Size;
    return Img;
}

static int
WriteBootSector(SHIM *S, const UCHAR *Code)
{
    UCHAR B[1024];
    ULONG Reserved = (ULONG)(S->A * S->Spb);
    ULONG Spf = S->K * S->Spb - 16;          /* the root directory: 16 sectors */
    ULONG Total = (ULONG)(S->FatEnd * S->Spb);
    ULONG Spt = S->H->SectorsPerTrack ? S->H->SectorsPerTrack : 63;
    ULONG Heads = S->H->Heads ? S->H->Heads : 255;
    ULONG Hidden = S->H->PartitionStart;
    ULONG Serial = S->Vol->SerialNumber;
    ExtCopy(B, Code, 1024);
    ExtCopy(B + 3, "EXTNTBT1", 8);
    B[0x0B] = 0x00; B[0x0C] = 0x02;          /* 512 bytes a sector */
    B[0x0D] = (UCHAR)S->Spb;
    B[0x0E] = (UCHAR)Reserved; B[0x0F] = (UCHAR)(Reserved >> 8);
    B[0x10] = 1;                             /* one FAT */
    B[0x11] = 0x00; B[0x12] = 0x01;          /* 256 root entries */
    B[0x13] = 0; B[0x14] = 0;
    B[0x15] = 0xF8;
    B[0x16] = (UCHAR)Spf; B[0x17] = (UCHAR)(Spf >> 8);
    B[0x18] = (UCHAR)Spt; B[0x19] = 0;
    B[0x1A] = (UCHAR)Heads; B[0x1B] = (UCHAR)(Heads >> 8);
    B[0x1C] = (UCHAR)Hidden; B[0x1D] = (UCHAR)(Hidden >> 8);
    B[0x1E] = (UCHAR)(Hidden >> 16); B[0x1F] = (UCHAR)(Hidden >> 24);
    B[0x20] = (UCHAR)Total; B[0x21] = (UCHAR)(Total >> 8); B[0x22] = (UCHAR)(Total >> 16); B[0x23] = (UCHAR)(Total >> 24);
    B[0x24] = 0x80; B[0x25] = 0; B[0x26] = 0x29;
    B[0x27] = (UCHAR)Serial; B[0x28] = (UCHAR)(Serial >> 8); B[0x29] = (UCHAR)(Serial >> 16); B[0x2A] = (UCHAR)(Serial >> 24);
    ExtCopy(B + 0x2B, "EXTNT BOOT ", 11);
    ExtCopy(B + 0x36, "FAT16   ", 8);
    ShFill(B + EXT_SHIM_WINDIR, 0, 16);
    ExtCopy(B + EXT_SHIM_WINDIR, S->R->WinDir, ShLen(S->R->WinDir));
    return ShWrite(S, 0, 2, B);
}

/* ------------------------------------------------------------------ */
/* Building the view                                                   */
/* ------------------------------------------------------------------ */

static VOID
CopyName(char *To, const char *From, ULONG Max)
{
    ULONG n = 0;
    if (*From == '\\' || *From == '/') From++;
    while (From[n] && n + 1 < Max) { To[n] = From[n]; n++; }
    To[n] = 0;
}

/* The Windows directory: given, recorded by extboot, BOOT.INI's default entry, or found */
static VOID
FindWinDir(SHIM *S, const char *Given)
{
    char *Root = S->R->WinDir;
    UCHAR Sec[1024];
    ULONG Ino, k;

    Root[0] = 0;
    if (Given && *Given) { CopyName(Root, Given, sizeof(S->R->WinDir)); return; }
    if (ShRead(S, 0, 2, Sec) == 0 && ExtShimPresent(Sec) && Sec[EXT_SHIM_WINDIR] > ' ') {
        ExtCopy(Root, Sec + EXT_SHIM_WINDIR, 15);
        Root[15] = 0;
        return;
    }
    Ino = Find(S, EXT_ROOT_INO, "BOOT.INI", FALSE);
    if (Ino) {
        EXT_INODE I;
        char *Txt = (char *)ShAlloc(4096);
        ULONG Got = 0;
        if (Txt && NT_SUCCESS(ExtReadInode(S->Vol, Ino, &I)) &&
            NT_SUCCESS(ExtReadFile(S->Vol, Ino, &I, 0, 4095, Txt, &Got))) {
            char *P;
            Txt[Got] = 0;
            for (P = Txt; *P; P++) {
                if ((P == Txt || P[-1] == '\n') && (P[0] | 0x20) == 'd' && ExtMemEq(P + 1, "efault", 6)) {
                    const char *Q = ShChr(P, '\\');
                    if (Q) {
                        ULONG n = 0;
                        Q++;
                        while (*Q && *Q != '\r' && *Q != '\n' && *Q != ' ' && *Q != '\t' && n < 15) Root[n++] = *Q++;
                        Root[n] = 0;
                    }
                    break;
                }
            }
        }
        ShFree(Txt);
        if (Root[0]) return;
    }
    {
        static const char *const Guess[] = { "WINNT", "WINDOWS", "WINNT40", "WINNT35", "WINNT4", "WINNT351", "WINNT31", 0 };
        for (k = 0; Guess[k]; k++) {
            ULONG W = Find(S, EXT_ROOT_INO, Guess[k], TRUE);
            if (W && Find(S, W, "SYSTEM32", TRUE)) { CopyName(Root, Guess[k], 16); return; }
        }
    }
}

/* The boot area: inode 5's first contiguous run */
static int
FindBootArea(SHIM *S)
{
    EXT_INODE Bi;
    ULONGLONG Disk, Len, B = 0;
    ULONG Run;
    if (!NT_SUCCESS(ExtReadInode(S->Vol, 5, &Bi)) || Bi.LinksCount == 0 ||
        !NT_SUCCESS(ExtMapFileBlock(S->Vol, 5, &Bi, 0, &Disk, &Run)) || Disk == 0)
        return Fail(S, "the volume has no boot area: format it with extfmt /BOOT");
    S->A = Disk;
    Len = ExtInodeSize(S->Vol, &Bi) / S->Bs;
    while (B < Len && B < 0x100000) {
        ULONGLONG D2;
        ULONG R = 0;
        if (!NT_SUCCESS(ExtMapFileBlock(S->Vol, 5, &Bi, B, &D2, &R)) || D2 != S->A + B || R == 0) break;
        B += R;
    }
    if (B > Len) B = Len;
    S->M = (ULONG)B;
    S->K = (EXT_BOOT_FAT_SECTORS + S->Spb - 1) / S->Spb;
    if (S->Spb > 128 || S->A * S->Spb > 65535 || S->M < S->K + 2)
        return Fail(S, "the boot area is not usable: format the volume with extfmt /BOOT");
    S->Base = S->A + S->K;
    /*
     * The FAT claims 65524 clusters, the most FAT16 has: Windows' own FAT
     * driver will not mount a FAT16 that large, so on any system the view
     * is only ever read by NTLDR (and EXTNT gets the volume), while NTLDR
     * reads it as it reads any FAT16. Files stay in the first 65518: cluster
     * numbers from FFF0h up are taken for markers by some loaders.
     */
    S->FatEnd = S->Base + 65524;
    if (S->FatEnd > S->Vol->BlocksCount) S->FatEnd = S->Vol->BlocksCount;
    S->WinEnd = S->Base + 65518;
    if (S->WinEnd > S->Vol->BlocksCount) S->WinEnd = S->Vol->BlocksCount;
    S->Clusters = (ULONG)(S->FatEnd - S->Base);
    if (S->Clusters < 4085) return Fail(S, "the volume is too small for the boot view");
    return 0;
}

static int
BuildTree(SHIM *S)
{
    static const char *const RootFiles[] = { "NTLDR", "NTDETECT.COM", "BOOT.INI", "BOOTSECT.DOS", "NTBOOTDD.SYS",
                                             "BOOTFONT.BIN", 0 };
    static const char *const Sys32Files[] = { "NT*.EXE", "HAL*.DLL", "KD*.DLL", "BOOTVID.DLL", "*.NLS", "*.FON",
                                              "C_*.*", "L_*.*", 0 };
    static const char *const ConfigFiles[] = { "SYSTEM", "SYSTEM.*", 0 };
    static const char *const FontFiles[] = { "*.FON", 0 };
    static const char *const SysRootFiles[] = { "BOOTSTAT.DAT", 0 };
    static const char *const AppPatchFiles[] = { "DRVMAIN.SDB", 0 };
    static const char *const InfFiles[] = { "BIOSINFO.INF", 0 };
    ULONG SysRoot, Sys32, Ino;
    int Wd, S32, Sr;

    if (!S->R->WinDir[0]) return Fail(S, "cannot tell the Windows directory: give it with /S");
    if (ShChr(S->R->WinDir, '\\')) return Fail(S, "the Windows directory must be in the root of the volume");
    SysRoot = Find(S, EXT_ROOT_INO, S->R->WinDir, TRUE);
    Sys32 = SysRoot ? Find(S, SysRoot, "SYSTEM32", TRUE) : 0;
    if (!Sys32) return Fail(S, "the Windows directory (with SYSTEM32) was not found");

    S->NDirs = 0;
    NewDir(S, -1, "", EXT_ROOT_INO);
    if (AddFiles(S, 0, EXT_ROOT_INO, RootFiles) < 0) return Fail(S, "out of memory");
    if (!Find(S, EXT_ROOT_INO, "NTLDR", FALSE)) return Fail(S, "NTLDR is not in the root of the volume");
    Wd = NewDir(S, 0, S->R->WinDir, SysRoot);
    if (Wd < 0) return Fail(S, "the Windows directory has no 8.3 name");
    if (AddFiles(S, Wd, SysRoot, SysRootFiles) < 0) return Fail(S, "out of memory");
    S32 = NewDir(S, Wd, "SYSTEM32", Sys32);
    if (S32 < 0 || AddFiles(S, S32, Sys32, Sys32Files) < 0) return Fail(S, "out of memory");
    if ((Ino = Find(S, Sys32, "DRIVERS", TRUE)) != 0) {
        Sr = NewDir(S, S32, "DRIVERS", Ino);
        if (Sr < 0 || AddFiles(S, Sr, Ino, 0) < 0) return Fail(S, "out of memory");
    }
    if ((Ino = Find(S, Sys32, "CONFIG", TRUE)) != 0) {
        Sr = NewDir(S, S32, "CONFIG", Ino);
        if (Sr < 0 || AddFiles(S, Sr, Ino, ConfigFiles) < 0) return Fail(S, "out of memory");
    }
    if ((Ino = Find(S, SysRoot, "FONTS", TRUE)) != 0) {
        Sr = NewDir(S, Wd, "FONTS", Ino);
        if (Sr < 0 || AddFiles(S, Sr, Ino, FontFiles) < 0) return Fail(S, "out of memory");
    }
    if ((Ino = Find(S, SysRoot, "SYSTEM", TRUE)) != 0) {
        Sr = NewDir(S, Wd, "SYSTEM", Ino);
        if (Sr < 0 || AddFiles(S, Sr, Ino, FontFiles) < 0) return Fail(S, "out of memory");
    }
    if ((Ino = Find(S, SysRoot, "INF", TRUE)) != 0) {
        Sr = NewDir(S, Wd, "INF", Ino);
        if (Sr < 0 || AddFiles(S, Sr, Ino, InfFiles) < 0) return Fail(S, "out of memory");
    }
    if ((Ino = Find(S, SysRoot, "APPPATCH", TRUE)) != 0) {
        Sr = NewDir(S, Wd, "APPPATCH", Ino);
        if (Sr < 0 || AddFiles(S, Sr, Ino, AppPatchFiles) < 0) return Fail(S, "out of memory");
    }
    return 0;
}

/* Every file of the view inside the window (other files moved out of the way first) */
static int
PlaceFiles(SHIM *S, BOOLEAN Initial)
{
    ULONGLONG Need = 0;
    ULONG d, k;
    for (d = 0; d < S->NDirs; d++)
        for (k = 0; k < S->D[d].Count; k++)
            if (S->D[d].F[k].Dir < 0 && S->D[d].F[k].Size) Need += BlocksToMove(S, S->D[d].F[k].Ino);
    if (Need) {
        Need += 64 + Need / 32;
        if (Initial) {
            /* room for the files to grow (the registry), so that later updates need not move others */
            ULONGLONG Extra = (4ULL << 20) / S->Bs;
            if (Extra > S->Clusters / 16) Extra = S->Clusters / 16;
            Need += Extra;
        }
        if (MakeRoom(S, Need) != 0) return Fail(S, "not enough free space near the start of the volume");
    }
    for (d = 0; d < S->NDirs; d++) {
        for (k = 0; k < S->D[d].Count; k++) {
            SFILE *F = &S->D[d].F[k];
            LONGLONG M;
            if (F->Dir >= 0 || F->Size == 0 || InWindow(S, F->Ino)) continue;
            M = MoveBlocks(S, F->Ino, MOVE_IN);
            ShCommit(S);
            if (M < 0 || !InWindow(S, F->Ino)) {
                FailName(S, "no room to move a file near the start of the volume", F->Name);
                return -1;
            }
            S->R->MovedIn++;
            /* its size as moved */
            {
                EXT_INODE I;
                if (NT_SUCCESS(ExtReadInode(S->Vol, F->Ino, &I))) F->Size = (ULONG)ExtInodeSize(S->Vol, &I);
            }
        }
    }
    return 0;
}

/* The FAT, the root directory and the subdirectories */
static int
WriteView(SHIM *S)
{
    ULONG d, k, FatBytes, Bytes;
    PUCHAR FatImg, Dir;

    S->Fat = (USHORT *)ShAlloc((S->Clusters + 2) * sizeof(USHORT));
    if (!S->Fat) return Fail(S, "out of memory");
    for (k = 0; k < S->Clusters + 2; k++) S->Fat[k] = 0xFFF7;        /* bad: nothing may go there */
    S->Fat[0] = 0xFFF8;
    S->Fat[1] = 0xFFFF;

    /* subdirectory clusters: the boot area blocks after the root directory */
    S->NextDirBlock = 0;
    for (d = 1; d < S->NDirs; d++) {
        ULONG Need = ((S->D[d].Count + 2) * 32 + S->Bs - 1) / S->Bs, c;
        if (S->K + S->NextDirBlock + Need > S->M) return Fail(S, "the boot area is too small for the directories");
        S->D[d].First = S->NextDirBlock + 2;
        S->D[d].Clusters = Need;
        for (c = 0; c < Need; c++) {
            ULONG Cl = S->D[d].First + c;
            S->Fat[Cl] = (USHORT)(c + 1 < Need ? Cl + 1 : 0xFFFF);
        }
        S->NextDirBlock += Need;
    }
    for (d = 0; d < S->NDirs; d++) {
        for (k = 0; k < S->D[d].Count; k++) {
            SFILE *F = &S->D[d].F[k];
            if (F->Dir >= 0 || F->Size == 0) continue;
            F->First = Chain(S, F->Ino);
            if (!F->First) { FailName(S, "a file of the view has a hole", F->Name); return -1; }
        }
    }

    FatBytes = (S->K * S->Spb - 16) * 512;
    FatImg = (PUCHAR)ShAlloc(FatBytes);
    if (!FatImg) return Fail(S, "out of memory");
    for (k = 0; k < S->Clusters + 2 && k * 2 + 1 < FatBytes; k++) {
        FatImg[k * 2] = (UCHAR)S->Fat[k];
        FatImg[k * 2 + 1] = (UCHAR)(S->Fat[k] >> 8);
    }
    k = ShWrite(S, S->A * S->Spb, FatBytes / 512, FatImg);
    ShFree(FatImg);
    if (k != 0) return Fail(S, "write error (FAT)");
    for (d = 0; d < S->NDirs; d++) {
        if (d == 0 && S->D[0].Count > 256) return Fail(S, "too many files in the root");
        Dir = DirImage(S, d, &Bytes);
        if (!Dir) return Fail(S, "out of memory");
        if (d == 0) k = ShWrite(S, S->A * S->Spb + S->K * S->Spb - 16, 16, Dir);
        else k = ShWrite(S, (S->Base + S->D[d].First - 2) * S->Spb, Bytes / 512, Dir);
        ShFree(Dir);
        if (k != 0) return Fail(S, "write error (directory)");
    }
    return 0;
}

EXT_STATUS
ExtShimUpdate(PEXT_SHIM_HOST Host, const char *WinDir, const UCHAR *Code, PEXT_SHIM_RESULT Result)
{
    SHIM *S;
    ULONG d, k;
    int Rc;

    ExtZero(Result, sizeof(*Result));
    if (Host->Vol->ReadOnly) { Result->Error = "the volume is read-only"; return EXT_READ_ONLY; }
    S = (SHIM *)ShAlloc(sizeof(SHIM));
    if (!S) { Result->Error = "out of memory"; return EXT_NO_MEMORY; }
    S->H = Host;
    S->Vol = Host->Vol;
    S->R = Result;
    S->Bs = S->Vol->BlockSize;
    S->Spb = S->Vol->SectorsPerBlock;

    Rc = FindBootArea(S);
    if (Rc == 0) { FindWinDir(S, WinDir); Rc = BuildTree(S); }
    if (Rc == 0) Rc = PlaceFiles(S, (BOOLEAN)(Code != 0));
    if (Rc == 0) Rc = WriteView(S);
    if (Rc == 0 && Code && WriteBootSector(S, Code) != 0) Rc = Fail(S, "write error (boot sector)");
    ShCommit(S);

    for (d = 0; d < S->NDirs; d++) {
        for (k = 0; k < S->D[d].Count; k++) if (S->D[d].F[k].Dir < 0) Result->Files++;
        ShFree(S->D[d].F);
    }
    Result->Dirs = S->NDirs;
    ShFree(S->Fat);
    ShFree(S);
    return Rc == 0 ? EXT_OK : EXT_CORRUPT;
}
