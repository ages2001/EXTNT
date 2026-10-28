/*
 * EXTNT - the ext3/ext4 journal (jbd2).
 *
 * Recovery: a volume with the RECOVER bit is replayed with the standard three
 * passes (scan for the last complete transaction, collect revoke records,
 * copy every logged block that was not revoked to its home). A transaction
 * counts only once its commit block is present and, with checksums, valid.
 *
 * Writing: EXTNT logs metadata the way jbd2 does, with a synchronous
 * checkpoint. ExtTxnCommit takes every dirty metadata block from the host
 * cache, writes descriptor + data blocks + a commit block to the log, points
 * the journal superblock at that transaction (the commit point), writes the
 * blocks home, and marks the journal empty again. A crash at any moment
 * leaves either the old or the new metadata after replay - by Linux, e2fsck
 * or EXTNT itself. File data is not journaled (as in writeback mode).
 *
 * The log is read and written with the host's direct I/O, never through the
 * metadata cache. Everything in the journal is big-endian.
 */

#ifdef EXT_KERNEL
#include "ext.h"
#else
#include "extusr.h"
#include "extsup.h"
#endif
#include "extjbd.h"

static ULONG
Be32(ULONG v)
{
    return ((v & 0xFF) << 24) | ((v & 0xFF00) << 8) | ((v >> 8) & 0xFF00) | ((v >> 24) & 0xFF);
}
static USHORT
Be16(USHORT v)
{
    return (USHORT)(((v & 0xFF) << 8) | ((v >> 8) & 0xFF));
}
static BOOLEAN
SeqAfter(ULONG a, ULONG b)                  /* a strictly after b, with wrap */
{
    return (BOOLEAN)((LONG)(a - b) > 0);
}

#define JBD_TAIL_SIZE   4                   /* descriptor/revoke block csum tail */
#define JBD_DEFAULT_FC  256                 /* fast-commit area when s_num_fc_blks == 0 */

typedef struct _EXT_JOURNAL {
    EXT_INODE   Inode;
    ULONG       JInum;
    ULONG       BlockSize;
    ULONG       First;                      /* first log block */
    ULONG       Last;                       /* one past the last log block (before any fc area) */
    ULONG       Sequence;                   /* next transaction ID */
    ULONG       Start;                      /* s_start as on disk (0 = empty) */
    ULONG       Incompat;
    BOOLEAN     Has64, CsumV2, CsumV3;
    UCHAR       Uuid[16];
    ULONG       CsumSeed;                   /* crc32c(~0, journal uuid) */
    ULONG       TagSize, TagsPerDesc;

    /* the open transaction: home block numbers and full block images */
    BOOLEAN     InTxn;
    ULONG       Count, Cap;
    ULONGLONG  *Home;
    PUCHAR      Data;
} EXT_JOURNAL, *PEXT_JOURNAL;

/* ------------------------------------------------------------------ */
/* Log block I/O                                                       */
/* ------------------------------------------------------------------ */

static EXT_STATUS
JDisk(PEXT_VOL Vol, PEXT_JOURNAL J, ULONG LogBlk, PULONGLONG Disk)
{
    ULONG Run;
    EXT_STATUS S = ExtMapFileBlock(Vol, J->JInum, &J->Inode, LogBlk, Disk, &Run);
    if (NT_SUCCESS(S) && *Disk == 0) S = EXT_FILE_CORRUPT;     /* the log has no holes */
    return S;
}

static EXT_STATUS
JRead(PEXT_VOL Vol, PEXT_JOURNAL J, ULONG LogBlk, PUCHAR Out)
{
    ULONGLONG Disk;
    EXT_STATUS S = JDisk(Vol, J, LogBlk, &Disk);
    if (!NT_SUCCESS(S)) return S;
    return ExtHostReadDirect(Vol, ExtBlockSector(Vol, Disk), Vol->SectorsPerBlock, Out);
}

static EXT_STATUS
JWrite(PEXT_VOL Vol, PEXT_JOURNAL J, ULONG LogBlk, const UCHAR *In)
{
    ULONGLONG Disk;
    EXT_STATUS S = JDisk(Vol, J, LogBlk, &Disk);
    if (!NT_SUCCESS(S)) return S;
    return ExtHostWriteDirect(Vol, ExtBlockSector(Vol, Disk), Vol->SectorsPerBlock, In);
}

static ULONG
JNext(PEXT_JOURNAL J, ULONG Blk)
{
    Blk++;
    if (Blk >= J->Last) Blk = J->First;
    return Blk;
}

/* journal_tag_bytes() */
static ULONG
JTagBytes(PEXT_JOURNAL J)
{
    ULONG Sz;
    if (J->CsumV3) return (ULONG)sizeof(JBD_BLOCK_TAG3);
    Sz = (ULONG)sizeof(JBD_BLOCK_TAG);          /* 12 */
    if (J->CsumV2) Sz += 2;
    return J->Has64 ? Sz : Sz - 4;
}

/* ------------------------------------------------------------------ */
/* Journal superblock                                                  */
/* ------------------------------------------------------------------ */

static EXT_STATUS
JLoad(PEXT_VOL Vol, PEXT_JOURNAL J)
{
    PUCHAR B;
    PJBD_SUPERBLOCK Sb;
    ULONG MaxLen, Fc, Type;
    EXT_STATUS S;

    J->JInum = Vol->JournalInum ? Vol->JournalInum : 8;
    S = ExtReadInode(Vol, J->JInum, &J->Inode);
    if (!NT_SUCCESS(S)) return S;
    J->BlockSize = Vol->BlockSize;
    B = (PUCHAR)ExtHostAlloc(Vol->BlockSize);
    if (!B) return EXT_NO_MEMORY;
    S = JRead(Vol, J, 0, B);
    if (!NT_SUCCESS(S)) { ExtHostFree(B); return S; }
    Sb = (PJBD_SUPERBLOCK)B;
    Type = Be32(Sb->Header.BlockType);
    if (Be32(Sb->Header.Magic) != JBD_MAGIC || (Type != JBD_BT_SUPERBLOCK_V1 && Type != JBD_BT_SUPERBLOCK_V2)) {
        ExtHostFree(B);
        return EXT_CORRUPT;
    }
    if (Be32(Sb->BlockSize) != Vol->BlockSize) { ExtHostFree(B); return EXT_NOT_SUPPORTED; }
    MaxLen = Be32(Sb->MaxLen);
    J->First = Be32(Sb->First);
    J->Sequence = Be32(Sb->Sequence);
    J->Start = Be32(Sb->Start);
    J->Incompat = (Type == JBD_BT_SUPERBLOCK_V2) ? Be32(Sb->FeatureIncompat) : 0;
    J->Has64 = (BOOLEAN)((J->Incompat & JBD_FI_64BIT) != 0);
    J->CsumV2 = (BOOLEAN)((J->Incompat & JBD_FI_CSUM_V2) != 0);
    J->CsumV3 = (BOOLEAN)((J->Incompat & JBD_FI_CSUM_V3) != 0);
    ExtCopy(J->Uuid, Sb->Uuid, 16);
    J->CsumSeed = ExtCrc32c(0xFFFFFFFF, Sb->Uuid, 16);
    Fc = 0;
    if (J->Incompat & JBD_FI_FAST_COMMIT) Fc = Be32(Sb->NumFcBlks) ? Be32(Sb->NumFcBlks) : JBD_DEFAULT_FC;
    ExtHostFree(B);

    if (J->Incompat & ~(ULONG)(JBD_FI_SUPPORTED | JBD_FI_FAST_COMMIT)) return EXT_NOT_SUPPORTED;
    if (J->First == 0 || MaxLen <= Fc || J->First >= MaxLen - Fc ||
        ((ULONGLONG)MaxLen << Vol->BlockShift) > ExtInodeSize(Vol, &J->Inode) + Vol->BlockSize)
        return EXT_CORRUPT;
    J->Last = MaxLen - Fc;
    J->TagSize = JTagBytes(J);
    J->TagsPerDesc = (Vol->BlockSize - (ULONG)sizeof(JBD_HEADER) - ((J->CsumV2 || J->CsumV3) ? JBD_TAIL_SIZE : 0)
                      - 16 /* uuid after the first tag */) / J->TagSize;
    return EXT_OK;
}

/* Rewrites s_start/s_sequence (and the csum) of the journal superblock */
static EXT_STATUS
JSetState(PEXT_VOL Vol, PEXT_JOURNAL J, ULONG Start, ULONG Sequence)
{
    PUCHAR B = (PUCHAR)ExtHostAlloc(Vol->BlockSize);
    PJBD_SUPERBLOCK Sb;
    EXT_STATUS S;
    if (!B) return EXT_NO_MEMORY;
    S = JRead(Vol, J, 0, B);
    if (NT_SUCCESS(S)) {
        Sb = (PJBD_SUPERBLOCK)B;
        Sb->Start = Be32(Start);
        Sb->Sequence = Be32(Sequence);
        Sb->ErrNo = 0;
        if (J->CsumV2 || J->CsumV3) {
            Sb->Checksum = 0;
            Sb->Checksum = Be32(ExtCrc32c(0xFFFFFFFF, B, 1024));
        }
        S = JWrite(Vol, J, 0, B);
    }
    ExtHostFree(B);
    if (NT_SUCCESS(S)) { J->Start = Start; J->Sequence = Sequence; }
    return S;
}

/* Checks the crc32c of a log block whose csum (big-endian) sits at CsumOffset */
static BOOLEAN
JBlockCsumOk(PEXT_JOURNAL J, PUCHAR Block, ULONG CsumOffset)
{
    ULONG Saved = *(PULONG)(Block + CsumOffset), Crc;
    if (!(J->CsumV2 || J->CsumV3)) return TRUE;
    *(PULONG)(Block + CsumOffset) = 0;
    Crc = ExtCrc32c(J->CsumSeed, Block, J->BlockSize);
    *(PULONG)(Block + CsumOffset) = Saved;
    return (BOOLEAN)(Be32(Saved) == Crc);
}

/* ------------------------------------------------------------------ */
/* Recovery                                                            */
/* ------------------------------------------------------------------ */

#define JPASS_SCAN   0
#define JPASS_REVOKE 1
#define JPASS_REPLAY 2

typedef struct { ULONGLONG Block; ULONG Seq; } JREVOKE;
typedef struct { JREVOKE *R; ULONG Count, Cap; } JREVOKES;

static VOID
RevokeAdd(JREVOKES *T, ULONGLONG Block, ULONG Seq)
{
    ULONG i;
    for (i = 0; i < T->Count; i++)
        if (T->R[i].Block == Block) { if (SeqAfter(Seq, T->R[i].Seq)) T->R[i].Seq = Seq; return; }
    if (T->Count == T->Cap) {
        ULONG NewCap = T->Cap ? T->Cap * 2 : 128;
        JREVOKE *N = (JREVOKE *)ExtHostAlloc(NewCap * sizeof(JREVOKE));
        if (!N) return;
        if (T->R) { ExtCopy(N, T->R, T->Count * sizeof(JREVOKE)); ExtHostFree(T->R); }
        T->R = N; T->Cap = NewCap;
    }
    T->R[T->Count].Block = Block;
    T->R[T->Count].Seq = Seq;
    T->Count++;
}

static BOOLEAN
Revoked(JREVOKES *T, ULONGLONG Block, ULONG Seq)
{
    ULONG i;
    for (i = 0; i < T->Count; i++)
        if (T->R[i].Block == Block) return (BOOLEAN)!SeqAfter(Seq, T->R[i].Seq);
    return FALSE;
}

static VOID
TagRead(PEXT_JOURNAL J, PUCHAR Tag, PULONGLONG Block, PULONG Flags)
{
    if (J->CsumV3) {
        PJBD_BLOCK_TAG3 T = (PJBD_BLOCK_TAG3)Tag;
        *Block = Be32(T->BlockNrLo) | (J->Has64 ? (ULONGLONG)Be32(T->BlockNrHi) << 32 : 0);
        *Flags = Be32(T->Flags);
    } else {
        PJBD_BLOCK_TAG T = (PJBD_BLOCK_TAG)Tag;
        *Block = Be32(T->BlockNr) | (J->Has64 ? (ULONGLONG)Be32(T->BlockNrHi) << 32 : 0);
        *Flags = Be16(T->Flags);
    }
}

/* One pass over the log from s_start. SCAN sets *EndSeq to the ID after the
   last complete transaction; the other passes stop there. */
static EXT_STATUS
JPass(PEXT_VOL Vol, PEXT_JOURNAL J, int Pass, JREVOKES *Revokes, PULONG EndSeq)
{
    PUCHAR B = (PUCHAR)ExtHostAlloc(J->BlockSize), D = (PUCHAR)ExtHostAlloc(J->BlockSize);
    ULONG Log = J->Start, Seq = J->Sequence, Guard = 0;
    ULONG TailBytes = (J->CsumV2 || J->CsumV3) ? JBD_TAIL_SIZE : 0;
    EXT_STATUS S = EXT_OK;

    if (!B || !D) { S = EXT_NO_MEMORY; goto out; }
    for (;;) {
        PJBD_HEADER H;
        ULONG Type;
        if (Pass != JPASS_SCAN && !SeqAfter(*EndSeq, Seq)) break;
        if (++Guard > J->Last) break;                       /* never loop forever */
        S = JRead(Vol, J, Log, B);
        if (!NT_SUCCESS(S)) break;
        H = (PJBD_HEADER)B;
        if (Be32(H->Magic) != JBD_MAGIC || Be32(H->Sequence) != Seq) break;
        Type = Be32(H->BlockType);

        if (Type == JBD_BT_DESCRIPTOR) {
            ULONG Off = (ULONG)sizeof(JBD_HEADER), DataLog = JNext(J, Log);
            if (TailBytes && !JBlockCsumOk(J, B, J->BlockSize - JBD_TAIL_SIZE)) break;
            for (;;) {
                ULONGLONG Home; ULONG Flags;
                if (Off + J->TagSize > J->BlockSize - TailBytes) break;
                TagRead(J, B + Off, &Home, &Flags);
                Off += J->TagSize;
                if (!(Flags & JBD_FLAG_SAME_UUID)) Off += 16;
                if (Pass == JPASS_REPLAY && !Revoked(Revokes, Home, Seq)) {
                    EXT_BUF Buf;
                    if (Home >= Vol->BlocksCount) { S = EXT_CORRUPT; goto out; }
                    S = JRead(Vol, J, DataLog, D);
                    if (!NT_SUCCESS(S)) goto out;
                    if (Flags & JBD_FLAG_ESCAPE) *(PULONG)D = Be32(JBD_MAGIC);
                    S = ExtMapBlocks(Vol, Home, 1, EXT_MAP_NEW, &Buf);
                    if (!NT_SUCCESS(S)) goto out;
                    ExtCopy(Buf.Data, D, J->BlockSize);
                    ExtSetDirty(Vol, &Buf);
                    ExtHostUnmap(Vol, &Buf);
                }
                DataLog = JNext(J, DataLog);
                if (Flags & JBD_FLAG_LAST_TAG) break;
            }
            Log = DataLog;
        } else if (Type == JBD_BT_COMMIT) {
            if (Pass == JPASS_SCAN && TailBytes &&
                !JBlockCsumOk(J, B, (ULONG)((PUCHAR)&((PJBD_COMMIT_BLOCK)B)->Checksum - B)))
                break;                                      /* torn commit: not complete */
            Log = JNext(J, Log);
            Seq++;
        } else if (Type == JBD_BT_REVOKE) {
            if (TailBytes && !JBlockCsumOk(J, B, J->BlockSize - JBD_TAIL_SIZE)) break;
            if (Pass == JPASS_REVOKE) {
                ULONG Used = Be32(((PJBD_REVOKE_HEADER)B)->Count), O = (ULONG)sizeof(JBD_REVOKE_HEADER);
                ULONG Rec = J->Has64 ? 8 : 4;
                if (Used > J->BlockSize - TailBytes) Used = J->BlockSize - TailBytes;
                for (; O + Rec <= Used; O += Rec) {
                    ULONGLONG Blk = Be32(*(PULONG)(B + O));
                    if (Rec == 8) Blk = (Blk << 32) | Be32(*(PULONG)(B + O + 4));
                    RevokeAdd(Revokes, Blk, Seq);
                }
            }
            Log = JNext(J, Log);
        } else {
            break;
        }
    }
    if (Pass == JPASS_SCAN) { *EndSeq = Seq; S = EXT_OK; }

out:
    if (B) ExtHostFree(B);
    if (D) ExtHostFree(D);
    return S;
}

EXT_STATUS
ExtJournalRecover(PEXT_VOL Vol)
{
    EXT_JOURNAL J;
    JREVOKES Rev;
    ULONG EndSeq = 0;
    EXT_STATUS S;

    if (!Vol->HasJournal || !Vol->NeedsRecovery) return EXT_OK;
    if (Vol->ReadOnly) return EXT_READ_ONLY;
    ExtZero(&J, sizeof(J));
    ExtZero(&Rev, sizeof(Rev));
    S = JLoad(Vol, &J);
    if (!NT_SUCCESS(S)) return S;

    if (J.Start != 0) {
        if (J.Start < J.First || J.Start >= J.Last) return EXT_CORRUPT;
        S = JPass(Vol, &J, JPASS_SCAN, &Rev, &EndSeq);
        if (NT_SUCCESS(S)) S = JPass(Vol, &J, JPASS_REVOKE, &Rev, &EndSeq);
        if (NT_SUCCESS(S)) S = JPass(Vol, &J, JPASS_REPLAY, &Rev, &EndSeq);
        if (Rev.R) ExtHostFree(Rev.R);
        if (!NT_SUCCESS(S)) return S;
        /* homes first, then the journal may say it is empty */
        S = ExtHostFlush(Vol);
        if (!NT_SUCCESS(S)) return S;
        S = JSetState(Vol, &J, 0, EndSeq);
        if (!NT_SUCCESS(S)) return S;
    }

    /* replayed blocks may include the superblock and group descriptors:
       reload them before clearing the flag */
    {
        PVOID Host = Vol->HostContext;
        ExtUnmountFree(Vol);
        ExtZero(Vol, sizeof(*Vol));
        Vol->HostContext = Host;
        S = ExtMount(Vol, FALSE);
        if (!NT_SUCCESS(S)) return S;
    }
    Vol->FeatureIncompat &= ~(ULONG)EXT_FI_RECOVER;
    Vol->Super->FeatureIncompat &= ~(ULONG)EXT_FI_RECOVER;
    Vol->NeedsRecovery = FALSE;
    S = ExtFlushSuper(Vol);
    if (NT_SUCCESS(S)) S = ExtHostFlush(Vol);
    return S;
}

/* ------------------------------------------------------------------ */
/* Writing                                                             */
/* ------------------------------------------------------------------ */

#define JBD_MAX_TXN     2048

/* Number of log blocks a transaction of Count blocks takes */
static ULONG
JTxnBlocks(PEXT_JOURNAL J, ULONG Count)
{
    return Count + (Count + J->TagsPerDesc - 1) / J->TagsPerDesc + 1;
}

EXT_STATUS
ExtJournalOpen(PEXT_VOL Vol)
{
    PEXT_JOURNAL J;
    EXT_STATUS S;

    if (!Vol->HasJournal || Vol->ReadOnly || Vol->Journal) return EXT_OK;
    J = (PEXT_JOURNAL)ExtHostAlloc(sizeof(EXT_JOURNAL));
    if (!J) return EXT_NO_MEMORY;
    ExtZero(J, sizeof(*J));
    S = JLoad(Vol, J);
    if (!NT_SUCCESS(S)) { ExtHostFree(J); return S; }
    if (J->Start != 0) { ExtHostFree(J); return EXT_CORRUPT; }    /* must have been recovered */
    if (J->Sequence == 0) J->Sequence = 1;

    /* the largest transaction that fits the ring */
    J->Cap = JBD_MAX_TXN;
    while (J->Cap > 0 && JTxnBlocks(J, J->Cap) > J->Last - J->First) J->Cap--;
    if (J->Cap < 8) { ExtHostFree(J); return EXT_NOT_SUPPORTED; }
    J->Home = (ULONGLONG *)ExtHostAlloc(J->Cap * sizeof(ULONGLONG));
    J->Data = (PUCHAR)ExtHostAlloc(J->Cap * Vol->BlockSize);
    if (!J->Home || !J->Data) {
        if (J->Home) ExtHostFree(J->Home);
        if (J->Data) ExtHostFree(J->Data);
        ExtHostFree(J);
        return EXT_NO_MEMORY;
    }
    Vol->Journal = J;
    return EXT_OK;
}

VOID
ExtJournalClose(PEXT_VOL Vol)
{
    PEXT_JOURNAL J = (PEXT_JOURNAL)Vol->Journal;
    if (!J) return;
    if (J->Home) ExtHostFree(J->Home);
    if (J->Data) ExtHostFree(J->Data);
    ExtHostFree(J);
    Vol->Journal = NULL;
}

BOOLEAN
ExtJournalInTxn(PEXT_VOL Vol)
{
    PEXT_JOURNAL J = (PEXT_JOURNAL)Vol->Journal;
    return (BOOLEAN)(J && J->InTxn);
}

EXT_STATUS
ExtTxnBegin(PEXT_VOL Vol)
{
    PEXT_JOURNAL J = (PEXT_JOURNAL)Vol->Journal;
    if (J && !J->InTxn) { J->InTxn = TRUE; J->Count = 0; }
    return EXT_OK;
}

VOID
ExtTxnAbort(PEXT_VOL Vol)
{
    PEXT_JOURNAL J = (PEXT_JOURNAL)Vol->Journal;
    if (J) { J->InTxn = FALSE; J->Count = 0; }
}

/* Slot for HomeBlock in the transaction, loading the block's current disk
   contents when it is new to the transaction and Load is set */
static PUCHAR
JSlot(PEXT_VOL Vol, PEXT_JOURNAL J, ULONGLONG HomeBlock, BOOLEAN Load)
{
    ULONG i;
    PUCHAR P;
    for (i = 0; i < J->Count; i++)
        if (J->Home[i] == HomeBlock) return J->Data + (ULONGLONG)i * Vol->BlockSize;
    if (J->Count >= J->Cap) return NULL;
    P = J->Data + (ULONGLONG)J->Count * Vol->BlockSize;
    if (Load && !NT_SUCCESS(ExtHostReadDirect(Vol, ExtBlockSector(Vol, HomeBlock), Vol->SectorsPerBlock, P)))
        return NULL;
    J->Home[J->Count++] = HomeBlock;
    return P;
}

EXT_STATUS
ExtJournalLog(PEXT_VOL Vol, ULONGLONG HomeBlock, const VOID *Data)
{
    PEXT_JOURNAL J = (PEXT_JOURNAL)Vol->Journal;
    PUCHAR P;
    if (!J || !J->InTxn) return EXT_OK;
    P = JSlot(Vol, J, HomeBlock, FALSE);
    if (!P) return EXT_NO_SPACE;
    ExtCopy(P, Data, Vol->BlockSize);
    return EXT_OK;
}

typedef struct { PEXT_VOL Vol; PEXT_JOURNAL J; EXT_STATUS S; } JLOG_CTX;

/* A dirty cached run: patch it into the image of every block it touches */
static VOID
JLogRun(PVOID Ctx, ULONGLONG Sector, ULONG Count, const UCHAR *Data)
{
    JLOG_CTX *C = (JLOG_CTX *)Ctx;
    PEXT_VOL Vol = C->Vol;
    ULONGLONG End = Sector + Count;
    while (Sector < End && NT_SUCCESS(C->S)) {
        ULONGLONG Blk = Sector / Vol->SectorsPerBlock;
        ULONG InBlk = (ULONG)(Sector % Vol->SectorsPerBlock);
        ULONG N = (ULONG)MIN_UL((ULONGLONG)(Vol->SectorsPerBlock - InBlk), End - Sector);
        BOOLEAN Whole = (BOOLEAN)(InBlk == 0 && N == Vol->SectorsPerBlock);
        PUCHAR P = JSlot(Vol, C->J, Blk, (BOOLEAN)!Whole);
        if (!P) { C->S = EXT_NO_SPACE; return; }
        ExtCopy(P + InBlk * 512, Data, N * 512);
        Data += N * 512;
        Sector += N;
    }
}

static VOID
TagWrite(PEXT_JOURNAL J, PUCHAR Tag, ULONGLONG Home, ULONG Flags, ULONG Csum)
{
    if (J->CsumV3) {
        PJBD_BLOCK_TAG3 T = (PJBD_BLOCK_TAG3)Tag;
        T->BlockNrLo = Be32((ULONG)Home);
        T->Flags = Be32(Flags);
        T->BlockNrHi = Be32((ULONG)(Home >> 32));
        T->Checksum = Be32(Csum);
    } else {
        PJBD_BLOCK_TAG T = (PJBD_BLOCK_TAG)Tag;
        T->BlockNr = Be32((ULONG)Home);
        T->Checksum = Be16((USHORT)Csum);
        T->Flags = Be16((USHORT)Flags);
        if (J->Has64) T->BlockNrHi = Be32((ULONG)(Home >> 32));
    }
}

/* Writes the open transaction to the log starting at Start */
static EXT_STATUS
JWriteTxn(PEXT_VOL Vol, PEXT_JOURNAL J, ULONG Seq, ULONG Start)
{
    ULONG BS = Vol->BlockSize, Log = Start, i = 0, TailBytes = (J->CsumV2 || J->CsumV3) ? JBD_TAIL_SIZE : 0;
    PUCHAR Desc = (PUCHAR)ExtHostAlloc(BS), Tmp = (PUCHAR)ExtHostAlloc(BS);
    EXT_STATUS S = EXT_OK;

    if (!Desc || !Tmp) { S = EXT_NO_MEMORY; goto out; }
    while (i < J->Count) {
        ULONG n = MIN_UL(J->Count - i, J->TagsPerDesc), k, Off = (ULONG)sizeof(JBD_HEADER);
        ULONG DescLog = Log;
        ExtZero(Desc, BS);
        ((PJBD_HEADER)Desc)->Magic = Be32(JBD_MAGIC);
        ((PJBD_HEADER)Desc)->BlockType = Be32(JBD_BT_DESCRIPTOR);
        ((PJBD_HEADER)Desc)->Sequence = Be32(Seq);
        Log = JNext(J, Log);
        for (k = 0; k < n; k++, i++) {
            PUCHAR Blk = J->Data + (ULONGLONG)i * BS;
            ULONG Flags = (k ? JBD_FLAG_SAME_UUID : 0) | (k == n - 1 ? JBD_FLAG_LAST_TAG : 0);
            ULONG Csum = 0;
            ExtCopy(Tmp, Blk, BS);
            if (Be32(*(PULONG)Tmp) == JBD_MAGIC) { *(PULONG)Tmp = 0; Flags |= JBD_FLAG_ESCAPE; }
            if (TailBytes) {
                ULONG BeSeq = Be32(Seq);
                Csum = ExtCrc32c(J->CsumSeed, &BeSeq, 4);
                Csum = ExtCrc32c(Csum, Blk, BS);            /* over the real (unescaped) data */
            }
            TagWrite(J, Desc + Off, J->Home[i], Flags, Csum);
            Off += J->TagSize;
            if (k == 0) { ExtCopy(Desc + Off, J->Uuid, 16); Off += 16; }
            S = JWrite(Vol, J, Log, Tmp);
            if (!NT_SUCCESS(S)) goto out;
            Log = JNext(J, Log);
        }
        if (TailBytes) {
            *(PULONG)(Desc + BS - JBD_TAIL_SIZE) = 0;
            *(PULONG)(Desc + BS - JBD_TAIL_SIZE) = Be32(ExtCrc32c(J->CsumSeed, Desc, BS));
        }
        S = JWrite(Vol, J, DescLog, Desc);
        if (!NT_SUCCESS(S)) goto out;
    }

    /* commit block */
    ExtZero(Desc, BS);
    {
        PJBD_COMMIT_BLOCK C = (PJBD_COMMIT_BLOCK)Desc;
        C->Header.Magic = Be32(JBD_MAGIC);
        C->Header.BlockType = Be32(JBD_BT_COMMIT);
        C->Header.Sequence = Be32(Seq);
        C->CommitSec = Be32(ExtHostNow());
        if (TailBytes) C->Checksum = Be32(ExtCrc32c(J->CsumSeed, Desc, BS));
    }
    S = JWrite(Vol, J, Log, Desc);

out:
    if (Desc) ExtHostFree(Desc);
    if (Tmp) ExtHostFree(Tmp);
    return S;
}

EXT_STATUS
ExtTxnCommit(PEXT_VOL Vol)
{
    PEXT_JOURNAL J = (PEXT_JOURNAL)Vol->Journal;
    JLOG_CTX C;
    ULONG Seq;
    EXT_STATUS S;

    if (Vol->ReadOnly) return EXT_OK;
    if (!J) return ExtHostFlush(Vol);           /* no journal: straight home */

    /* frees of this transaction go into it (they were held back until now) */
    S = ExtApplyPendingFrees(Vol);
    if (!NT_SUCCESS(S)) return S;
    S = ExtFlushSuper(Vol);
    if (!NT_SUCCESS(S)) return S;
    if (!J->InTxn) { J->InTxn = TRUE; J->Count = 0; }
    C.Vol = Vol; C.J = J; C.S = EXT_OK;
    ExtHostEnumDirty(Vol, JLogRun, &C);
    if (C.S == EXT_NO_SPACE) {
        /* more than the journal can hold at once: write home unprotected */
        J->InTxn = FALSE; J->Count = 0;
        return ExtHostFlush(Vol);
    }
    if (!NT_SUCCESS(C.S)) { J->InTxn = FALSE; J->Count = 0; return C.S; }
    if (J->Count == 0) { J->InTxn = FALSE; return EXT_OK; }

    Seq = J->Sequence;
    S = JWriteTxn(Vol, J, Seq, J->First);       /* the log is empty: start at the front */
    if (NT_SUCCESS(S)) S = JSetState(Vol, J, J->First, Seq);       /* commit point */
    J->InTxn = FALSE;
    J->Count = 0;
    if (!NT_SUCCESS(S)) return S;
    if (Vol->CrashAfterCommit) return EXT_OK;   /* test hook: leave it for replay */
    S = ExtHostFlush(Vol);                      /* homes */
    if (NT_SUCCESS(S)) S = JSetState(Vol, J, 0, Seq + 1);          /* checkpointed */
    return S;
}

/* ------------------------------------------------------------------ */
/* Starting and stopping writes on a mounted volume                    */
/* ------------------------------------------------------------------ */

/* After mount (and recovery): open the journal, mark the volume in use */
EXT_STATUS
ExtStartWriting(PEXT_VOL Vol)
{
    EXT_STATUS S;
    if (Vol->ReadOnly) return EXT_OK;
    S = ExtJournalOpen(Vol);
    if (!NT_SUCCESS(S)) return S;
    Vol->MountState = Vol->Super->State;
    /* as Linux: a journalled volume says "in use" with needs_recovery, and
       keeps its clean state for after the replay; without a journal the
       state itself says "not cleanly unmounted" */
    if (Vol->Journal == NULL) {
        Vol->Super->State &= ~EXT_VALID_FS;
    }
    Vol->Super->MountCount++;
    Vol->Super->MountTime = ExtHostNow();
    if (Vol->Journal) {
        Vol->Super->FeatureIncompat |= EXT_FI_RECOVER;
        Vol->FeatureIncompat |= EXT_FI_RECOVER;
    }
    S = ExtFlushSuper(Vol);
    if (NT_SUCCESS(S)) S = ExtHostFlush(Vol);       /* RECOVER must be on disk before any commit */
    return S;
}

/* Clean unmount: last commit, journal empty, state restored */
EXT_STATUS
ExtStopWriting(PEXT_VOL Vol)
{
    EXT_STATUS S;
    if (Vol->ReadOnly) return EXT_OK;
    S = ExtFlushSuper(Vol);
    if (NT_SUCCESS(S)) S = ExtTxnCommit(Vol);
    if (!NT_SUCCESS(S)) return S;
    Vol->Super->FeatureIncompat &= ~(ULONG)EXT_FI_RECOVER;
    Vol->FeatureIncompat &= ~(ULONG)EXT_FI_RECOVER;
    Vol->Super->State = (USHORT)(Vol->MountState | (Vol->Super->State & EXT_ERROR_FS));
    Vol->Super->WriteTime = ExtHostNow();
    S = ExtFlushSuper(Vol);
    if (NT_SUCCESS(S)) S = ExtHostFlush(Vol);
    ExtJournalClose(Vol);
    return S;
}
