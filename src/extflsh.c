/*
 * EXTNT - IRP_MJ_FLUSH_BUFFERS, IRP_MJ_SHUTDOWN and the volume's in-use state
 *
 * While EXTNT may change a volume, the volume says so on the disk, as Linux
 * does: with a journal the needs_recovery flag is set (a crash leaves the
 * journal to replay), without one the clean state is cleared (fsck checks
 * it). ExtStartWriting does that at mount; ExtStopWriting - last commit,
 * journal empty, state restored - at lock, dismount and shutdown.
 * Flushing a file or the volume commits the journal.
 */

#include "ext.h"

/* ------------------------------------------------------------------ */
/* In use                                                              */
/* ------------------------------------------------------------------ */

NTSTATUS
ExnStartWriting (
    PEXN_VCB Vcb
    )
{
    NTSTATUS Status;

    if (Vcb->Writing || Vcb->Vol.ReadOnly ||
        (Vcb->VcbState & (VCB_STATE_READ_ONLY | VCB_STATE_SHUTDOWN | VCB_STATE_REMOVED))) {
        return STATUS_SUCCESS;
    }

    ExnEngineEnter(Vcb);
    Status = ExtStartWriting(&Vcb->Vol);
    if (NT_SUCCESS(Status)) {
        Vcb->Writing = TRUE;
    }
    Status = ExnEngineLeave(Vcb, Status);

    if (NT_SUCCESS(Status)) {
        ExnCommitTimerStart(Vcb);
    } else {
        EXN_DBG((EXN_PFX "cannot start writing (%lX): read-only\n", Status));
        Vcb->VcbState |= VCB_STATE_READ_ONLY;
        Vcb->Vol.ReadOnly = TRUE;
    }
    return Status;
}

/* Everything on the disk and the volume clean; the engine not held */
NTSTATUS
ExnStopWriting (
    PEXN_VCB Vcb
    )
{
    NTSTATUS Status;

    if (!Vcb->Writing) {
        return STATUS_SUCCESS;
    }

    ExnCommitTimerStop(Vcb);

    if (Vcb->BootView) {
        ExnUpdateBootView(Vcb);
    }

    ExnEngineEnter(Vcb);
    Status = ExtStopWriting(&Vcb->Vol);
    Vcb->Writing = FALSE;
    Status = ExnEngineLeave(Vcb, Status);
    EXN_DBG((EXN_PFX "%08lX clean (%lX), %lu commits\n", Vcb->SerialNumber, Status, Vcb->Commits));

    if (!NT_SUCCESS(Status)) {
        EXN_DBG((EXN_PFX "stop writing failed %lX\n", Status));
    }
    return Status;
}

/* ------------------------------------------------------------------ */
/* The boot view (see extshim.c)                                       */
/* ------------------------------------------------------------------ */

static EXT_STATUS
ExnShimRead (PVOID Ctx, ULONGLONG Sector, ULONG Count, PVOID Buf)
{
    return ExtHostReadDirect(&((PEXN_VCB)Ctx)->Vol, Sector, Count, Buf);
}

static EXT_STATUS
ExnShimWrite (PVOID Ctx, ULONGLONG Sector, ULONG Count, const VOID *Buf)
{
    return ExtHostWriteDirect(&((PEXN_VCB)Ctx)->Vol, Sector, Count, Buf);
}

static EXT_STATUS
ExnShimCommit (PVOID Ctx)
{
    return ExnCommit((PEXN_VCB)Ctx);
}

/* Files in use as paging files, or with changes not yet in their inode, stay where they are */
static BOOLEAN
ExnShimCanMove (PVOID Ctx, ULONG Ino)
{
    PEXN_VCB Vcb = (PEXN_VCB)Ctx;
    PLIST_ENTRY Entry;
    PEXN_FCB Fcb;
    for (Entry = Vcb->FcbList.Flink; Entry != &Vcb->FcbList; Entry = Entry->Flink) {
        Fcb = CONTAINING_RECORD(Entry, EXN_FCB, FcbLinks);
        if (Fcb->Ino == Ino &&
            (Fcb->FcbState & (FCB_STATE_PAGING_FILE | FCB_STATE_INODE_DIRTY) || Fcb->Runs != NULL)) {
            return FALSE;
        }
    }
    return TRUE;
}

/* A file's blocks moved: the FCBs take the new inode */
static VOID
ExnShimMoved (PVOID Ctx, ULONG Ino)
{
    PEXN_VCB Vcb = (PEXN_VCB)Ctx;
    PLIST_ENTRY Entry;
    PEXN_FCB Fcb;
    for (Entry = Vcb->FcbList.Flink; Entry != &Vcb->FcbList; Entry = Entry->Flink) {
        Fcb = CONTAINING_RECORD(Entry, EXN_FCB, FcbLinks);
        if (Fcb->Ino == Ino && !(Fcb->FcbState & FCB_STATE_DELETED)) {
            (VOID)ExtReadInode(&Vcb->Vol, Ino, &Fcb->Inode);
            Fcb->MapRun = 0;
        }
    }
}

/*
 * Brings the FAT view NTLDR reads up to date with the files it shows:
 * a boot file that changed, grew, or was replaced shows its current blocks.
 * Called while the volume stops being written (shutdown, lock, dismount),
 * everything flushed; Vcb->Resource exclusive.
 */
VOID
ExnUpdateBootView (
    PEXN_VCB Vcb
    )
{
    EXT_SHIM_HOST Host;
    EXT_SHIM_RESULT Result;
    NTSTATUS Status;

    if (!Vcb->Writing || Vcb->Vol.ReadOnly || (Vcb->VcbState & VCB_STATE_READ_ONLY)) {
        return;
    }
    RtlZeroMemory(&Host, sizeof(Host));
    Host.Vol = &Vcb->Vol;
    Host.Ctx = Vcb;
    Host.Read = ExnShimRead;
    Host.Write = ExnShimWrite;
    Host.Commit = ExnShimCommit;
    Host.CanMove = ExnShimCanMove;
    Host.Moved = ExnShimMoved;
    Host.CopyBlocks = Vcb->Vol.BlockSize >= 16384 ? 1 : 16384 / Vcb->Vol.BlockSize;

    ExnEngineEnter(Vcb);
    Status = ExnCommit(Vcb);
    if (NT_SUCCESS(Status)) {
        Status = ExtShimUpdate(&Host, NULL, NULL, &Result);
        EXN_DBG((EXN_PFX "boot view: %lX, %lu files, %lu moved in, %lu out%s%s\n", Status, Result.Files,
                 Result.MovedIn, Result.MovedOut, Result.Error ? ": " : "", Result.Error ? Result.Error : ""));
    }
    (VOID)ExnEngineLeave(Vcb, Status);
}

/* Back in use after a lock (or a refused one); Vcb->Resource exclusive */
VOID
ExnResumeWriting (
    PEXN_VCB Vcb
    )
{
    if (!(Vcb->VcbState & (VCB_STATE_DISMOUNTED | VCB_STATE_LOCKED | VCB_STATE_PNP_LOCKED))) {
        (VOID)ExnStartWriting(Vcb);
    }
}

/* ------------------------------------------------------------------ */
/* Flushing                                                            */
/* ------------------------------------------------------------------ */

/* Data and inode of one file; Vcb->Resource exclusive */
NTSTATUS
ExnFlushFile (
    PEXN_VCB Vcb,
    PEXN_FCB Fcb
    )
{
    IO_STATUS_BLOCK Iosb;
    NTSTATUS Status = STATUS_SUCCESS;

    (VOID)ExAcquireResourceExclusiveLite(&Fcb->Resource, TRUE);

    __try {

        if (Fcb->SectionObjectPointers.DataSectionObject != NULL) {

            CcFlushCache(&Fcb->SectionObjectPointers, NULL, 0, &Iosb);
            Status = Iosb.Status;

            /* Let a lazy write in progress finish */
            (VOID)ExAcquireResourceExclusiveLite(&Fcb->PagingIoResource, TRUE);
            ExnRelease(&Fcb->PagingIoResource);
        }

        /*
         * ext keeps no valid-data length on disk, so blocks allocated past
         * the last byte written must be zeroed before the larger size
         * reaches the inode, or stale data would show through on the next
         * mount (holes need nothing).
         */
        if (ExnIsFcb(Fcb) && !(Fcb->FcbState & FCB_STATE_DELETED) &&
            Fcb->Header.ValidDataLength.QuadPart < Fcb->Header.FileSize.QuadPart) {

            NTSTATUS Zero = ExnZeroDisk(Vcb, Fcb, Fcb->Header.ValidDataLength.QuadPart,
                                        Fcb->Header.FileSize.QuadPart);

            if (NT_SUCCESS(Zero)) {
                Fcb->Header.ValidDataLength = Fcb->Header.FileSize;
            } else if (NT_SUCCESS(Status)) {
                Status = Zero;
            }
        }

        if ((Fcb->FcbState & FCB_STATE_INODE_DIRTY) && !(Fcb->FcbState & FCB_STATE_DELETED)) {

            if (NT_SUCCESS(Status)) {
                Status = ExnUpdateInode(Vcb, Fcb);
            } else {
                (VOID)ExnUpdateInode(Vcb, Fcb);
            }
        }

    } __finally {

        ExnRelease(&Fcb->Resource);
    }

    return Status;
}

/* Metadata: a journal commit (or, without a journal, the cached blocks) */
static NTSTATUS
ExnFlushMetadata (
    PEXN_VCB Vcb
    )
{
    return ExnCommitNow(Vcb);
}

/*
 * Writes everything cached for the volume; Final also ends its use
 * (journal empty, clean state), for a lock, dismount or shutdown.
 * Vcb->Resource exclusive.
 */
NTSTATUS
ExnFlushVolume (
    PEXN_VCB Vcb,
    BOOLEAN Final
    )
{
    PLIST_ENTRY Entry;
    PEXN_FCB Fcb;
    BOOLEAN Restart;
    NTSTATUS Status = STATUS_SUCCESS;
    NTSTATUS Result;

    if (Vcb->VcbState & (VCB_STATE_READ_ONLY | VCB_STATE_DISMOUNTED)) {
        return STATUS_SUCCESS;
    }

    /* A flush can close file objects, so the list is walked with markers */
    do {

        Restart = FALSE;

        for (Entry = Vcb->FcbList.Flink; Entry != &Vcb->FcbList; Entry = Entry->Flink) {

            Fcb = CONTAINING_RECORD(Entry, EXN_FCB, FcbLinks);

            if ((Fcb->FcbState & FCB_STATE_VISITED) || !ExnIsFcb(Fcb)) {
                continue;
            }

            Fcb->FcbState |= FCB_STATE_VISITED;
            Fcb->RefCount++;

            Result = ExnFlushFile(Vcb, Fcb);
            if (NT_SUCCESS(Status)) {
                Status = Result;
            }

            ExnDereferenceFcb(Vcb, Fcb);

            Restart = TRUE;
            break;
        }

    } while (Restart);

    for (Entry = Vcb->FcbList.Flink; Entry != &Vcb->FcbList; Entry = Entry->Flink) {
        Fcb = CONTAINING_RECORD(Entry, EXN_FCB, FcbLinks);
        Fcb->FcbState &= ~FCB_STATE_VISITED;
    }

    Result = ExnFlushMetadata(Vcb);
    if (NT_SUCCESS(Status)) {
        Status = Result;
    }

    if (Final) {
        Result = ExnStopWriting(Vcb);
        if (NT_SUCCESS(Status)) {
            Status = Result;
        }
    }

    (VOID)ExnFlushDevice(Vcb);

    return Status;
}

/* ------------------------------------------------------------------ */
/* Requests                                                            */
/* ------------------------------------------------------------------ */

NTSTATUS
ExnCommonFlushBuffers (
    PEXN_IRP_CONTEXT Ctx
    )
{
    PFILE_OBJECT FileObject = Ctx->IrpSp->FileObject;
    PEXN_FCB Fcb = (PEXN_FCB)FileObject->FsContext;
    PEXN_VCB Vcb = Ctx->Vcb;
    NTSTATUS Status;

    if (Fcb == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    (VOID)ExAcquireResourceExclusiveLite(&Vcb->Resource, TRUE);

    __try {

        Status = ExnVerifyVcb(Ctx, Vcb);
        if (!NT_SUCCESS(Status) || (Vcb->VcbState & VCB_STATE_READ_ONLY)) {
            __leave;
        }

        if (ExnIsVfcb(Fcb)) {

            Status = ExnFlushVolume(Vcb, FALSE);

        } else {

            if (ExnIsFcb(Fcb)) {
                Status = ExnFlushFile(Vcb, Fcb);
            }

            if (NT_SUCCESS(Status)) {
                Status = ExnFlushMetadata(Vcb);
            } else {
                (VOID)ExnFlushMetadata(Vcb);
            }

            (VOID)ExnFlushDevice(Vcb);
        }

    } __finally {

        ExnRelease(&Vcb->Resource);
    }

    return Status;
}

/* Sent to the file system device: leave every volume clean */
NTSTATUS
ExnCommonShutdown (
    PEXN_IRP_CONTEXT Ctx
    )
{
    PLIST_ENTRY Entry;
    PEXN_VCB Vcb;

    UNREFERENCED_PARAMETER(Ctx);

    EXN_DBG((EXN_PFX "shutdown\n"));
    (VOID)ExAcquireResourceSharedLite(&ExnData.Resource, TRUE);

    __try {

        for (Entry = ExnData.VcbList.Flink; Entry != &ExnData.VcbList; Entry = Entry->Flink) {

            Vcb = CONTAINING_RECORD(Entry, EXN_VCB, VcbLinks);

            (VOID)ExAcquireResourceExclusiveLite(&Vcb->Resource, TRUE);

            if (Vcb->VcbState & VCB_STATE_MOUNTED) {
                /* From now on every change goes to the disk at once (ExnEngineLeave):
                   the journal is closed and the volume marked clean */
                Vcb->VcbState |= VCB_STATE_SHUTDOWN;
                (VOID)ExnFlushVolume(Vcb, TRUE);
            }

            ExnRelease(&Vcb->Resource);
        }

    } __finally {

        ExnRelease(&ExnData.Resource);
    }

    return STATUS_SUCCESS;
}
