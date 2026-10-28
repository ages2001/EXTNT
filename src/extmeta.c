/*
 * EXTNT - the engine's host in the driver: the metadata cache and journal
 * commits.
 *
 * Metadata is cached here block by block, exactly as the engine maps it (a
 * block is the run of sectors one ExtHostMap call names: a bitmap, a
 * directory block, an extent node, the sectors holding an inode ...):
 *
 *   - A block mapped again finds its cached copy.
 *   - A new block that overlaps cached ones writes and drops them first.
 *   - Freed blocks are dropped from the cache (ExtHostFreed), so a block that
 *     turns into file data is never written over it later.
 *   - Clean blocks stay cached up to EXN_META_MAX_BYTES, least recently used
 *     first out.
 *
 * Changed blocks are written:
 *   - without a journal (ext2): when the engine call that changed them
 *     finishes (ExnEngineLeave);
 *   - with a journal: by a commit (ExtTxnCommit logs every dirty block, then
 *     writes them home). Commits happen every EXN_COMMIT_SECONDS, when the
 *     dirty set grows past what one transaction should hold, on a flush
 *     request, and when the volume goes away - like Linux's jbd2.
 *
 * Everything here runs with Vcb->EngineResource held exclusive, except the
 * timer DPC (which only queues the commit work item).
 */

#include "ext.h"

#define VCB_OF(Vol)     ((PEXN_VCB)(Vol)->HostContext)

static ULONG
MetaHash (
    ULONGLONG Sector
    )
{
    ULONG S = (ULONG)Sector ^ (ULONG)(Sector >> 32);
    return (S ^ (S >> 10)) & (EXN_META_HASH - 1);
}

VOID
ExnMetaInitialize (
    PEXN_VCB Vcb
    )
{
    ULONG i;

    InitializeListHead(&Vcb->MetaLru);
    for (i = 0; i < EXN_META_HASH; i++) {
        Vcb->MetaHash[i] = NULL;
    }
    Vcb->MetaBytes = 0;
    Vcb->MetaDirty = 0;
    Vcb->MetaDirtyBytes = 0;
    Vcb->MetaDirtyLimit = 4 * 1024 * 1024;
    Vcb->MetaWriteStatus = STATUS_SUCCESS;
}

static VOID
MetaUnhash (
    PEXN_VCB Vcb,
    PEXN_META M
    )
{
    PEXN_META *P = &Vcb->MetaHash[MetaHash(M->Sector)];

    while (*P != NULL) {
        if (*P == M) {
            *P = M->HashNext;
            break;
        }
        P = &(*P)->HashNext;
    }
}

static VOID
MetaSetClean (
    PEXN_VCB Vcb,
    PEXN_META M
    )
{
    if (M->Dirty) {
        M->Dirty = FALSE;
        Vcb->MetaDirty--;
        Vcb->MetaDirtyBytes -= M->Count * EXN_SECTOR_SIZE;
    }
}

static VOID
MetaFree (
    PEXN_VCB Vcb,
    PEXN_META M
    )
{
    MetaUnhash(Vcb, M);
    RemoveEntryList(&M->Links);
    Vcb->MetaBytes -= M->Count * EXN_SECTOR_SIZE;
    MetaSetClean(Vcb, M);
    ExFreePool(M->Data);
    ExFreePool(M);
}

static NTSTATUS
MetaWrite (
    PEXN_VCB Vcb,
    PEXN_META M
    )
{
    NTSTATUS Status;

    Status = ExnWriteDisk(Vcb, M->Sector, M->Count, M->Data);
    if (NT_SUCCESS(Status)) {
        MetaSetClean(Vcb, M);
    } else if (NT_SUCCESS(Vcb->MetaWriteStatus)) {
        Vcb->MetaWriteStatus = Status;
    }
    return Status;
}

/* Writes every changed block home and waits (the engine's ExtHostFlush) */
static NTSTATUS
MetaFlush (
    PEXN_VCB Vcb
    )
{
    PLIST_ENTRY Entry;
    PEXN_META M;
    NTSTATUS Status = STATUS_SUCCESS;
    NTSTATUS Result;

    if (Vcb->MetaDirty != 0) {
        /* oldest first: closer to the order the changes were made */
        for (Entry = Vcb->MetaLru.Blink; Entry != &Vcb->MetaLru; Entry = Entry->Blink) {
            M = CONTAINING_RECORD(Entry, EXN_META, Links);
            if (M->Dirty) {
                Result = MetaWrite(Vcb, M);
                if (!NT_SUCCESS(Result) && NT_SUCCESS(Status)) {
                    Status = Result;
                }
            }
        }
    }
    if (NT_SUCCESS(Status)) {
        Status = Vcb->MetaWriteStatus;
    }
    Vcb->MetaWriteStatus = STATUS_SUCCESS;
    return Status;
}

/* Drops everything (the volume is going away or was changed under us) */
VOID
ExnMetaPurge (
    PEXN_VCB Vcb
    )
{
    PEXN_META M;

    while (!IsListEmpty(&Vcb->MetaLru)) {
        M = CONTAINING_RECORD(Vcb->MetaLru.Flink, EXN_META, Links);
        MetaFree(Vcb, M);
    }
    Vcb->MetaDirty = 0;
    Vcb->MetaDirtyBytes = 0;
}

/* Keeps the clean cache within its budget */
static VOID
MetaTrim (
    PEXN_VCB Vcb
    )
{
    PLIST_ENTRY Entry, Prev;
    PEXN_META M;

    for (Entry = Vcb->MetaLru.Blink;
         Entry != &Vcb->MetaLru && Vcb->MetaBytes > EXN_META_MAX_BYTES + Vcb->MetaDirtyBytes;
         Entry = Prev) {

        Prev = Entry->Blink;
        M = CONTAINING_RECORD(Entry, EXN_META, Links);
        if (M->Refs == 0 && !M->Dirty) {
            MetaFree(Vcb, M);
        }
    }
}

/*
 * Drops cached blocks overlapping [Sector, Sector+Count). Unless Discard,
 * changed ones are written first. Fails if one is in use (and not Discard).
 */
static NTSTATUS
MetaDropOverlaps (
    PEXN_VCB Vcb,
    ULONGLONG Sector,
    ULONG Count,
    BOOLEAN Discard
    )
{
    PLIST_ENTRY Entry, Next;
    PEXN_META M;
    NTSTATUS Status;

    for (Entry = Vcb->MetaLru.Flink; Entry != &Vcb->MetaLru; Entry = Next) {

        Next = Entry->Flink;
        M = CONTAINING_RECORD(Entry, EXN_META, Links);

        if (M->Sector >= Sector + Count || Sector >= M->Sector + M->Count) {
            continue;
        }

        if (M->Refs != 0) {
            if (Discard) {
                continue;           /* still mapped by a caller: leave it */
            }
            EXN_DBG((EXN_PFX "overlapping metadata maps %I64X+%lu / %I64X+%lu\n",
                     Sector, Count, M->Sector, M->Count));
            return STATUS_FILE_CORRUPT_ERROR;
        }

        if (M->Dirty && !Discard) {
            Status = MetaWrite(Vcb, M);
            if (!NT_SUCCESS(Status)) {
                return Status;
            }
        }
        MetaFree(Vcb, M);
    }

    return STATUS_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* The engine's host interface                                         */
/* ------------------------------------------------------------------ */

EXT_STATUS
ExtHostMap (
    PEXT_VOL Vol,
    ULONGLONG Sector,
    ULONG Count,
    ULONG Mode,
    PEXT_BUF Buf
    )
{
    PEXN_VCB Vcb = VCB_OF(Vol);
    PEXN_META M;
    NTSTATUS Status;
    ULONGLONG Total = (ULONGLONG)(Vcb->PartitionBytes >> EXN_SECTOR_SHIFT);

    Buf->Host = NULL;
    Buf->Data = NULL;

    if (Count == 0 || Count > 256 || Sector >= Total || Count > Total - Sector) {
        return EXT_CORRUPT;
    }

    for (M = Vcb->MetaHash[MetaHash(Sector)]; M != NULL; M = M->HashNext) {
        if (M->Sector == Sector && M->Count == Count) {
            break;
        }
    }

    if (M == NULL) {

        /* Overlapping blocks of other shapes go to the disk and out first */
        Status = MetaDropOverlaps(Vcb, Sector, Count, FALSE);
        if (!NT_SUCCESS(Status)) {
            return Status;
        }

        M = (PEXN_META)ExAllocatePoolWithTag(NonPagedPool, sizeof(EXN_META), EXN_TAG_META);
        if (M == NULL) {
            return EXT_NO_MEMORY;
        }
        M->Data = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, Count * EXN_SECTOR_SIZE, EXN_TAG_META);
        if (M->Data == NULL) {
            ExFreePool(M);
            return EXT_NO_MEMORY;
        }
        M->Sector = Sector;
        M->Count = Count;
        M->Refs = 0;
        M->Dirty = FALSE;
        M->IsData = (BOOLEAN)((Mode & EXT_MAP_DATA) != 0);

        if (EXT_MAP_MODE(Mode) == EXT_MAP_NEW) {
            RtlZeroMemory(M->Data, Count * EXN_SECTOR_SIZE);
        } else {
            Status = ExnReadDisk(Vcb, Sector, Count, M->Data);
            if (!NT_SUCCESS(Status)) {
                ExFreePool(M->Data);
                ExFreePool(M);
                return Status;
            }
        }

        M->HashNext = Vcb->MetaHash[MetaHash(Sector)];
        Vcb->MetaHash[MetaHash(Sector)] = M;
        InsertHeadList(&Vcb->MetaLru, &M->Links);
        Vcb->MetaBytes += Count * EXN_SECTOR_SIZE;

    } else {

        RemoveEntryList(&M->Links);
        InsertHeadList(&Vcb->MetaLru, &M->Links);
        if (EXT_MAP_MODE(Mode) == EXT_MAP_NEW) {
            RtlZeroMemory(M->Data, Count * EXN_SECTOR_SIZE);
        }
    }

    M->Refs++;
    Buf->Data = M->Data;
    Buf->Sector = Sector;
    Buf->Count = Count;
    Buf->Host = M;
    Buf->Dirty = FALSE;
    return EXT_OK;
}

VOID
ExtHostSetDirty (
    PEXT_VOL Vol,
    PEXT_BUF Buf
    )
{
    PEXN_VCB Vcb = VCB_OF(Vol);
    PEXN_META M = (PEXN_META)Buf->Host;

    if (M != NULL && !M->Dirty) {
        M->Dirty = TRUE;
        Vcb->MetaDirty++;
        Vcb->MetaDirtyBytes += M->Count * EXN_SECTOR_SIZE;
    }
    Buf->Dirty = TRUE;
}

VOID
ExtHostUnmap (
    PEXT_VOL Vol,
    PEXT_BUF Buf
    )
{
    PEXN_VCB Vcb = VCB_OF(Vol);
    PEXN_META M = (PEXN_META)Buf->Host;

    if (M == NULL) {
        return;
    }
    Buf->Host = NULL;
    Buf->Data = NULL;
    M->Refs--;
    if (M->Refs == 0) {
        /* file data the engine moved (symlink targets, ea_inode values) is
           not journaled: it goes out at once */
        if (M->IsData && M->Dirty) {
            (VOID)MetaWrite(Vcb, M);
        }
        if (Vcb->MetaBytes > EXN_META_MAX_BYTES + Vcb->MetaDirtyBytes) {
            MetaTrim(Vcb);
        }
    }
}

VOID
ExtHostFreed (
    PEXT_VOL Vol,
    ULONGLONG Sector,
    ULONG Count
    )
{
    (VOID)MetaDropOverlaps(VCB_OF(Vol), Sector, Count, TRUE);
}

VOID
ExtHostEnumDirty (
    PEXT_VOL Vol,
    EXT_DIRTY_CB Cb,
    PVOID Ctx
    )
{
    PEXN_VCB Vcb = VCB_OF(Vol);
    PLIST_ENTRY Entry;
    PEXN_META M;

    for (Entry = Vcb->MetaLru.Blink; Entry != &Vcb->MetaLru; Entry = Entry->Blink) {
        M = CONTAINING_RECORD(Entry, EXN_META, Links);
        if (M->Dirty && !M->IsData) {
            Cb(Ctx, M->Sector, M->Count, M->Data);
        }
    }
}

EXT_STATUS
ExtHostFlush (
    PEXT_VOL Vol
    )
{
    PEXN_VCB Vcb = VCB_OF(Vol);
    NTSTATUS Status = MetaFlush(Vcb);

    /* the disk's own cache too: the journal's ordering rests on it */
    if (NT_SUCCESS(Status)) {
        (VOID)ExnFlushDevice(Vcb);
    }
    return Status;
}

EXT_STATUS
ExtHostReadDirect (
    PEXT_VOL Vol,
    ULONGLONG Sector,
    ULONG Count,
    PVOID Buffer
    )
{
    /* the engine's buffers are paged: the transfer goes through nonpaged memory */
    PVOID Bounce = ExAllocatePoolWithTag(NonPagedPool, Count * EXN_SECTOR_SIZE, EXN_TAG_BUFFER);
    NTSTATUS Status;

    if (Bounce == NULL) {
        return EXT_NO_MEMORY;
    }
    Status = ExnReadDisk(VCB_OF(Vol), Sector, Count, Bounce);
    if (NT_SUCCESS(Status)) {
        RtlCopyMemory(Buffer, Bounce, Count * EXN_SECTOR_SIZE);
    }
    ExFreePool(Bounce);
    return Status;
}

EXT_STATUS
ExtHostWriteDirect (
    PEXT_VOL Vol,
    ULONGLONG Sector,
    ULONG Count,
    const VOID *Buffer
    )
{
    PEXN_VCB Vcb = VCB_OF(Vol);
    PLIST_ENTRY Entry;
    PEXN_META M;
    ULONGLONG From, To;

    /* a cached copy of what is written here (journal replay writes metadata
       home this way) takes the new contents */
    for (Entry = Vcb->MetaLru.Flink; Entry != &Vcb->MetaLru; Entry = Entry->Flink) {
        M = CONTAINING_RECORD(Entry, EXN_META, Links);
        if (M->Sector >= Sector + Count || Sector >= M->Sector + M->Count) {
            continue;
        }
        From = (M->Sector > Sector) ? M->Sector : Sector;
        To = (M->Sector + M->Count < Sector + Count) ? M->Sector + M->Count : Sector + Count;
        RtlCopyMemory(M->Data + (ULONG)(From - M->Sector) * EXN_SECTOR_SIZE,
                      (const UCHAR *)Buffer + (ULONG)(From - Sector) * EXN_SECTOR_SIZE,
                      (ULONG)(To - From) * EXN_SECTOR_SIZE);
    }
    {
        PVOID Bounce = ExAllocatePoolWithTag(NonPagedPool, Count * EXN_SECTOR_SIZE, EXN_TAG_BUFFER);
        NTSTATUS Status;

        if (Bounce == NULL) {
            return EXT_NO_MEMORY;
        }
        RtlCopyMemory(Bounce, Buffer, Count * EXN_SECTOR_SIZE);
        Status = ExnWriteDisk(Vcb, Sector, Count, Bounce);
        ExFreePool(Bounce);
        return Status;
    }
}

PVOID
ExtHostAlloc (
    ULONG Bytes
    )
{
    return ExAllocatePoolWithTag(PagedPool, Bytes ? Bytes : 1, EXN_TAG_ENGINE);
}

VOID
ExtHostFree (
    PVOID P
    )
{
    ExFreePool(P);
}

ULONG
ExtHostNow (
    VOID
    )
{
    LARGE_INTEGER Now;
    ULONG Seconds;

    KeQuerySystemTime(&Now);
    if (!RtlTimeToSecondsSince1970(&Now, &Seconds)) {
        Seconds = 0;
    }
    return Seconds;
}

/* ------------------------------------------------------------------ */
/* Engine calls and commits                                            */
/* ------------------------------------------------------------------ */

VOID
ExnEngineEnter (
    PEXN_VCB Vcb
    )
{
#ifdef EXN_TRACEIO
    DBGP("[eng+ "); DBGX(KeGetCurrentThread()); DBGP("]\n");
#endif
    (VOID)ExAcquireResourceExclusiveLite(&Vcb->EngineResource, TRUE);
#ifdef EXN_TRACEIO
    DBGP("[eng= "); DBGX(KeGetCurrentThread()); DBGP("]\n");
#endif
}

/* A journal commit, or a plain flush without a journal (engine held) */
NTSTATUS
ExnCommit (
    PEXN_VCB Vcb
    )
{
    NTSTATUS Status;

    if (Vcb->Vol.ReadOnly || !Vcb->Writing) {
        return MetaFlush(Vcb);
    }
    Status = ExtTxnCommit(&Vcb->Vol);
    if (NT_SUCCESS(Status)) {
        Vcb->Commits++;
    }
    return Status;
}

NTSTATUS
ExnCommitNow (
    PEXN_VCB Vcb
    )
{
    ExnEngineEnter(Vcb);
    return ExnEngineLeave(Vcb, ExnCommit(Vcb));
}

/*
 * Ends an engine call. Without a journal the changes go to the disk now;
 * with one they wait for the next commit unless the dirty set is getting
 * large. Returns Status, or the write error if the call itself succeeded.
 */
NTSTATUS
ExnEngineLeave (
    PEXN_VCB Vcb,
    NTSTATUS Status
    )
{
    NTSTATUS Flush = STATUS_SUCCESS;

    if (Vcb->MetaDirty != 0) {
        if (Vcb->Vol.Journal == NULL || Vcb->MetaDirtyBytes > Vcb->MetaDirtyLimit ||
            (Vcb->VcbState & VCB_STATE_SHUTDOWN)) {
            Flush = ExnCommit(Vcb);
        }
    }

    ExnRelease(&Vcb->EngineResource);

    if (NT_SUCCESS(Status) && !NT_SUCCESS(Flush)) {
        Status = Flush;
    }
    return ExnStatus(Status);
}

/* ------------------------------------------------------------------ */
/* The periodic commit                                                 */
/* ------------------------------------------------------------------ */

static VOID
NTAPI
ExnCommitWorker (
    PVOID Context
    )
{
    PEXN_VCB Vcb = (PEXN_VCB)Context;

    if (!(Vcb->VcbState & (VCB_STATE_DISMOUNTED | VCB_STATE_IN_DISMOUNT)) && Vcb->Writing) {
        ExnEngineEnter(Vcb);
        if (Vcb->MetaDirty != 0 || Vcb->Vol.PendingCount != 0) {
            (VOID)ExnCommit(Vcb);
        }
        ExnRelease(&Vcb->EngineResource);
    }
    InterlockedExchange(&Vcb->CommitQueued, 0);
    /* one-shot timer (NT 3.1 has no periodic KeSetTimerEx): arm the next */
    if (Vcb->TimerSet) {
        LARGE_INTEGER Due;
        Due.QuadPart = -(LONGLONG)EXN_COMMIT_SECONDS * 10000000;
        KeSetTimer(&Vcb->CommitTimer, Due, &Vcb->CommitDpc);
    }
    KeSetEvent(&Vcb->CommitIdle, IO_NO_INCREMENT, FALSE);
}

static VOID
NTAPI
ExnCommitDpc (
    PKDPC Dpc,
    PVOID Context,
    PVOID Arg1,
    PVOID Arg2
    )
{
    PEXN_VCB Vcb = (PEXN_VCB)Context;

    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(Arg1);
    UNREFERENCED_PARAMETER(Arg2);

    if (!Vcb->TimerSet) {
        return;                 /* stopping: the volume may be going away */
    }
    if (InterlockedExchange(&Vcb->CommitQueued, 1) == 0) {
        (VOID)KeResetEvent(&Vcb->CommitIdle);
        ExQueueWorkItem(&Vcb->CommitItem, DelayedWorkQueue);
    }
}

VOID
ExnCommitTimerStart (
    PEXN_VCB Vcb
    )
{
    LARGE_INTEGER Due;

    if (Vcb->TimerSet || Vcb->Vol.ReadOnly) {
        return;
    }
    KeInitializeEvent(&Vcb->CommitIdle, NotificationEvent, TRUE);
    ExInitializeWorkItem(&Vcb->CommitItem, ExnCommitWorker, Vcb);
    KeInitializeDpc(&Vcb->CommitDpc, ExnCommitDpc, Vcb);
    KeInitializeTimer(&Vcb->CommitTimer);
    Due.QuadPart = -(LONGLONG)EXN_COMMIT_SECONDS * 10000000;
    Vcb->TimerSet = TRUE;
    KeSetTimer(&Vcb->CommitTimer, Due, &Vcb->CommitDpc);
}

/* Stops the timer and waits for a queued commit to finish (engine not held) */
VOID
ExnCommitTimerStop (
    PEXN_VCB Vcb
    )
{
    if (!Vcb->TimerSet) {
        return;
    }
    Vcb->TimerSet = FALSE;
    KeCancelTimer(&Vcb->CommitTimer);
    KeWaitForSingleObject(&Vcb->CommitIdle, Executive, KernelMode, FALSE, NULL);
    KeCancelTimer(&Vcb->CommitTimer);   /* a worker that ran meanwhile may have re-armed it */
}

/* ------------------------------------------------------------------ */
/* Status mapping                                                      */
/* ------------------------------------------------------------------ */

NTSTATUS
ExnStatus (
    EXT_STATUS S
    )
{
    switch (S) {
    case EXT_NO_SPACE:          return STATUS_EA_TOO_LARGE;
    case EXT_TOO_LARGE:         return STATUS_DISK_FULL;
    case EXT_NOT_A_DIRECTORY:   return STATUS_NOT_A_DIRECTORY;
    case EXT_NO_SUCH_EA:        return STATUS_NO_EAS_ON_FILE;
    case EXT_EA_CORRUPT:        return STATUS_EA_CORRUPT_ERROR;
    default:                    return (NTSTATUS)S;
    }
}
