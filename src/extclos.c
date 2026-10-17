/*
 * EXTNT - IRP_MJ_CLEANUP, IRP_MJ_CLOSE, dismount and volume teardown
 *
 * Cleanup of the last handle is where a file is deleted, extra
 * allocation is given back and the entry set gets the final sizes and
 * times. A dismounted volume stays in memory until its last file object
 * is closed. Then the VPB is detached (so the next access mounts afresh)
 * and the volume device object is deleted.
 */

#include "ext.h"

VOID
ExnUnlockVcb (
    PEXN_VCB Vcb
    )
{
    KIRQL Irql;

    Vcb->VcbState &= ~VCB_STATE_LOCKED;
    Vcb->LockFileObject = NULL;

    IoAcquireVpbSpinLock(&Irql);
    Vcb->Vpb->Flags &= ~VPB_LOCKED;
    IoReleaseVpbSpinLock(Irql);
}

/*
 * Drops cached data of files nobody has open, so that file objects held
 * only by the cache manager or memory manager get closed. Callers that
 * want the data kept flush first. The list is rescanned after each purge
 * because closes can arrive while it runs.
 */
VOID
ExnPurgeCachedFiles (
    PEXN_VCB Vcb
    )
{
    PLIST_ENTRY Entry;
    PEXN_FCB Fcb;
    BOOLEAN Restart;

    do {

        Restart = FALSE;

        for (Entry = Vcb->FcbList.Flink; Entry != &Vcb->FcbList; Entry = Entry->Flink) {

            Fcb = CONTAINING_RECORD(Entry, EXN_FCB, FcbLinks);

            if (Fcb->FcbState & FCB_STATE_VISITED) {
                continue;
            }

            Fcb->FcbState |= FCB_STATE_VISITED;

            if (!ExnIsFcb(Fcb) ||
                Fcb->UncleanCount != 0 ||
                (Fcb->SectionObjectPointers.DataSectionObject == NULL &&
                 Fcb->SectionObjectPointers.ImageSectionObject == NULL)) {

                continue;
            }

            Fcb->RefCount++;

            (VOID)CcPurgeCacheSection(&Fcb->SectionObjectPointers, NULL, 0, FALSE);
            /* an image still mapped (a running program) is left alone: forcing
               it closed takes the pages from under the process (NT 3.1 faults) */
            if (Fcb->SectionObjectPointers.ImageSectionObject == NULL ||
                MmFlushImageSection(&Fcb->SectionObjectPointers, MmFlushForWrite)) {
                (VOID)MmForceSectionClosed(&Fcb->SectionObjectPointers, TRUE);
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
}

/*
 * Called with Vcb->Resource held exclusive. Callers that can still reach
 * the disk flush the volume first. Returns TRUE when the VCB can be
 * deleted right away; the caller then releases the resource and calls
 * ExnDeleteVcb.
 */
BOOLEAN
ExnDismountVcb (
    PEXN_VCB Vcb
    )
{
    PLIST_ENTRY Entry;
    PEXN_FCB Fcb;

    if (Vcb->VcbState & VCB_STATE_DISMOUNTED) {
        return FALSE;
    }

    EXN_DBG((EXN_PFX "Dismounting volume %08lX\n", Vcb->SerialNumber));

    Vcb->VcbState |= VCB_STATE_DISMOUNTED | VCB_STATE_IN_DISMOUNT;
    Vcb->VcbState &= ~VCB_STATE_MOUNTED;

    /* Send cached I/O on open files back through the IRP path */
    for (Entry = Vcb->FcbList.Flink; Entry != &Vcb->FcbList; Entry = Entry->Flink) {
        Fcb = CONTAINING_RECORD(Entry, EXN_FCB, FcbLinks);
        Fcb->Header.IsFastIoPossible = FastIoIsNotPossible;
    }

    ExnPurgeCachedFiles(Vcb);

    /* Metadata: write what is pending, then forget it (re-read on next use) */
    ExnCommitTimerStop(Vcb);
    (VOID)ExnStopWriting(Vcb);
    ExnEngineEnter(Vcb);
    ExnMetaPurge(Vcb);
    ExnRelease(&Vcb->EngineResource);

    Vcb->VcbState &= ~VCB_STATE_IN_DISMOUNT;

    if (ExnTryTeardown(Vcb, 0)) {
        return TRUE;
    }

    /*
     * The volume still has open references (user files - e.g. the running
     * system volume while autochk runs), so it cannot
     * be torn down and handed back to the I/O manager for a fresh mount: on NT
     * 3.1 that remount path cannot complete while the old VPB is still mounted,
     * and the verify that a later open triggers would hang. The caches have
     * already been purged above, so instead of leaving the volume
     * dismounted-but-alive we keep it mounted and re-read the on-disk
     * structures, which is exactly the fresh state a remount would produce.
     *
     * Only while it is still the same ext volume, though. Formatted over -
     * another file system, or a new ext volume - it stays dismounted: its
     * files fail, and the next open is verified, so the I/O manager mounts
     * what is on the disk now. Reading on as if nothing happened would write
     * into the new file system.
     */
    if (!(Vcb->VcbState & VCB_STATE_REMOVED) && !ExnSameVolume(Vcb)) {
        EXN_DBG((EXN_PFX "volume %08lX was formatted over: stays dismounted\n", Vcb->SerialNumber));
        return FALSE;
    }

    Vcb->VcbState &= ~VCB_STATE_DISMOUNTED;
    Vcb->VcbState |= VCB_STATE_MOUNTED;
    Vcb->VcbState &= ~VCB_STATE_DASD_WRITTEN;

    /* Re-read the volume's structures as a fresh mount would */
    if (!(Vcb->VcbState & VCB_STATE_REMOVED)) {
        if (NT_SUCCESS(ExnLoadVolume(Vcb))) {
            ExnLoadLabel(Vcb);
        } else {
            Vcb->VcbState |= VCB_STATE_DISMOUNTED;
            Vcb->VcbState &= ~VCB_STATE_MOUNTED;
            return FALSE;
        }
    }
    /* (a locked volume is the locker's to write: writing resumes at the unlock) */
    if (!(Vcb->VcbState & (VCB_STATE_READ_ONLY | VCB_STATE_LOCKED))) {
        (VOID)ExnStartWriting(Vcb);
    }

    return FALSE;
}

/*
 * Called with Vcb->Resource held exclusive. InFlightReferences is the
 * VPB reference the current request itself holds (1 for a create, 0 for
 * a close).
 */
BOOLEAN
ExnTryTeardown (
    PEXN_VCB Vcb,
    ULONG InFlightReferences
    )
{
    PVPB Vpb = Vcb->Vpb;
    KIRQL Irql;
    BOOLEAN FreeVpb = FALSE;

    if (!(Vcb->VcbState & VCB_STATE_DISMOUNTED) ||
        (Vcb->VcbState & (VCB_STATE_IN_DISMOUNT | VCB_STATE_DELETE_PENDING)) ||
        Vcb->OpenCount != 0) {

        return FALSE;
    }

    IoAcquireVpbSpinLock(&Irql);

    if (Vpb->ReferenceCount != InFlightReferences) {
        IoReleaseVpbSpinLock(Irql);
        return FALSE;
    }

    if (Vpb->RealDevice->Vpb == Vpb) {

        Vpb->DeviceObject = NULL;
        Vpb->Flags &= ~(VPB_MOUNTED | VPB_LOCKED);

    } else if (InFlightReferences == 0) {

        /* The I/O manager replaced this VPB during a verify */
        FreeVpb = TRUE;
    }

    IoReleaseVpbSpinLock(Irql);

    Vcb->VcbState |= VCB_STATE_DELETE_PENDING;

    if (FreeVpb) {
        Vcb->VcbState |= VCB_STATE_FREE_VPB;
    }

    return TRUE;
}

/* Called without any locks held */
VOID
ExnDeleteVcb (
    PEXN_VCB Vcb
    )
{
    PEXN_FCB Fcb;
    PVPB FreeVpb = NULL;

    EXN_DBG((EXN_PFX "Deleting volume %08lX\n", Vcb->SerialNumber));

    (VOID)ExAcquireResourceExclusiveLite(&ExnData.Resource, TRUE);
    RemoveEntryList(&Vcb->VcbLinks);
    ExnRelease(&ExnData.Resource);

    while (!IsListEmpty(&Vcb->FcbList)) {
        Fcb = CONTAINING_RECORD(Vcb->FcbList.Flink, EXN_FCB, FcbLinks);
        ExnDeleteFcb(Fcb);
    }

    if (Vcb->VolumeFcb != NULL) {
        ExnDeleteFcb(Vcb->VolumeFcb);
    }

    ExnCommitTimerStop(Vcb);
    ExnMetaPurge(Vcb);
    ExtUnmountFree(&Vcb->Vol);

    FsRtlNotifyUninitializeSync(&Vcb->NotifySync);

    if (Vcb->VcbState & VCB_STATE_FREE_VPB) {
        FreeVpb = Vcb->Vpb;
    }

    ExDeleteResourceLite(&Vcb->EngineResource);
    ExDeleteResourceLite(&Vcb->Resource);

    ObDereferenceObject(Vcb->TargetDeviceObject);
    IoDeleteDevice(Vcb->VolumeDeviceObject);

    if (FreeVpb != NULL) {
        ExFreePool(FreeVpb);
    }
}

/* ------------------------------------------------------------------ */
/* Cleanup                                                             */
/* ------------------------------------------------------------------ */

/* Last handle of a delete-pending file or directory: remove its name */
static VOID
ExnDeleteOnCleanup (
    PEXN_VCB Vcb,
    PEXN_FCB Fcb
    )
{
    BOOLEAN Empty = TRUE;
    BOOLEAN LastName;
    NTSTATUS Status;

    if (ExnIsDcb(Fcb)) {

        /* Entries may have come back since the delete was requested */
        ExnEngineEnter(Vcb);
        Status = ExnEngineLeave(Vcb, ExtDirIsEmpty(&Vcb->Vol, Fcb->Ino, &Fcb->Inode, &Empty));
        if (!NT_SUCCESS(Status)) {
            Empty = FALSE;
        }

        if (!Empty) {
            Fcb->FcbState &= ~FCB_STATE_DELETE_PENDING;
            return;
        }
        LastName = TRUE;

    } else {

        /* with other hard links left, the file and its cache live on */
        LastName = (BOOLEAN)(Fcb->Inode.LinksCount <= 1 || (Fcb->FcbState & FCB_STATE_SYMLINK));
        if (LastName) {
            /* Nothing cached may reach the blocks about to be freed */
            (VOID)ExAcquireResourceExclusiveLite(&Fcb->PagingIoResource, TRUE);
            Fcb->Header.FileSize.QuadPart = 0;
            Fcb->Header.ValidDataLength.QuadPart = 0;
            ExnRelease(&Fcb->PagingIoResource);

            (VOID)CcPurgeCacheSection(&Fcb->SectionObjectPointers, NULL, 0, FALSE);
        }
    }

    Status = ExnUnlinkName(Vcb, Fcb);

    if (!NT_SUCCESS(Status)) {
        EXN_DBG((EXN_PFX "Delete of %wZ failed %lX\n", &Fcb->Name, Status));
    }
    Fcb->FcbState &= ~FCB_STATE_DELETE_PENDING;

    ExnNotifyChange(Vcb, Fcb,
                    ExnIsDcb(Fcb) ? FILE_NOTIFY_CHANGE_DIR_NAME : FILE_NOTIFY_CHANGE_FILE_NAME,
                    FILE_ACTION_REMOVED);
}

NTSTATUS
ExnCommonCleanup (
    PEXN_IRP_CONTEXT Ctx
    )
{
    PFILE_OBJECT FileObject = Ctx->IrpSp->FileObject;
    PEXN_FCB Fcb = (PEXN_FCB)FileObject->FsContext;
    PEXN_CCB Ccb = (PEXN_CCB)FileObject->FsContext2;
    PEXN_VCB Vcb = Ctx->Vcb;
    LARGE_INTEGER Now;
    LARGE_INTEGER Zero;
    PLARGE_INTEGER TruncateSize = NULL;
    ULONG Filter = 0;
    BOOLEAN FcbAcquired = FALSE;
    BOOLEAN Writable;
    BOOLEAN Modified;

    /* IoCreateStreamFileObject cleans up before FsContext is set */
    if (Fcb == NULL || Ccb == NULL) {
        return STATUS_SUCCESS;
    }

    Zero.QuadPart = 0;

    (VOID)ExAcquireResourceExclusiveLite(&Vcb->Resource, TRUE);

    __try {

        Writable = (BOOLEAN)!(Vcb->VcbState & (VCB_STATE_DISMOUNTED | VCB_STATE_READ_ONLY |
                                               VCB_STATE_SHUTDOWN));

        if (ExnIsVfcb(Fcb)) {

            if (Vcb->LockFileObject == FileObject) {

                ExnUnlockVcb(Vcb);

                /* Metadata may have been rewritten under us */
                if (Vcb->VcbState & VCB_STATE_DASD_WRITTEN) {
                    (VOID)ExnDismountVcb(Vcb);
                }
            }

        } else {

            (VOID)ExAcquireResourceExclusiveLite(&Fcb->Resource, TRUE);
            FcbAcquired = TRUE;

            if (ExnIsDcb(Fcb)) {
#ifdef EXN_NT31
                ExnNotifyCleanupList(Vcb, Ccb);
#else
                FsRtlNotifyCleanup(Vcb->NotifySync, &Vcb->DirNotifyList, Ccb);
#endif
            } else {
                FsRtlFastUnlockAll(&Fcb->FileLock, FileObject,
                                   IoGetRequestorProcess(Ctx->Irp), NULL);
            }

            if (Ccb->Flags & CCB_FLAG_DELETE_ON_CLOSE) {
                Fcb->FcbState |= FCB_STATE_DELETE_PENDING;
                ExnSetDeleteName(Fcb, Ccb);
            }

            Modified = (BOOLEAN)((FileObject->Flags & FO_FILE_MODIFIED) != 0);

            if (Writable && !(Fcb->FcbState & FCB_STATE_DELETED)) {

                if (Modified) {

                    KeQuerySystemTime(&Now);

                    if (!(Ccb->Flags & CCB_FLAG_USER_SET_WRITE)) {
                        Fcb->LastWriteTime = Now;
                        Filter |= FILE_NOTIFY_CHANGE_LAST_WRITE;
                    }

                    if (!(Ccb->Flags & CCB_FLAG_USER_SET_ACCESS)) {
                        Fcb->LastAccessTime = Now;
                    }

                    Fcb->ChangeTime = Now;      /* data changed: ctime too, as Linux */
                    Fcb->FcbState |= FCB_STATE_INODE_DIRTY;
                }

                if (FileObject->Flags & FO_FILE_SIZE_CHANGED) {
                    Fcb->FcbState |= FCB_STATE_INODE_DIRTY;
                    Filter |= FILE_NOTIFY_CHANGE_SIZE;
                }

                if (Fcb->UncleanCount == 1 && (Fcb->FcbState & FCB_STATE_DELETE_PENDING)) {

                    ExnDeleteOnCleanup(Vcb, Fcb);

                    if (Fcb->FcbState & FCB_STATE_DELETED) {
                        TruncateSize = &Zero;
                        Filter = 0;
                    }

                } else if (Fcb->UncleanCount == 1 &&
                           (Fcb->FcbState & FCB_STATE_TRUNCATE_ON_CLOSE)) {

                    (VOID)ExnSetAllocation(Vcb, Fcb, (ULONGLONG)Fcb->Header.FileSize.QuadPart);
            if (Fcb->SectionObjectPointers.SharedCacheMap != NULL) {
                CcSetFileSizes(FileObject, (PCC_FILE_SIZES)&Fcb->Header.AllocationSize);
            }
                    Fcb->FcbState &= ~FCB_STATE_TRUNCATE_ON_CLOSE;
                }

                if (Fcb->FcbState & FCB_STATE_INODE_DIRTY) {
                    (VOID)ExnUpdateInode(Vcb, Fcb);
                }

                if (Filter != 0) {
                    ExnNotifyChange(Vcb, Fcb, Filter, FILE_ACTION_MODIFIED);
                }

                /* Removable media: this file is on the disk once closed */
                if ((Vcb->VcbState & VCB_STATE_FLUSH_ON_CLOSE) && ExnIsFcb(Fcb) &&
                    (Modified || (FileObject->Flags & FO_FILE_SIZE_CHANGED)) &&
                    !(Fcb->FcbState & FCB_STATE_DELETED)) {

                    (VOID)ExnFlushFile(Vcb, Fcb);
                }
            }

            if (ExnIsFcb(Fcb)) {

                CcUninitializeCacheMap(FileObject, TruncateSize, NULL);

                /* On a dismounted volume let the last handle take the cache
                   with it, so the file objects close and the VCB can go */
                if ((Vcb->VcbState & VCB_STATE_DISMOUNTED) && Fcb->UncleanCount == 1) {
                    (VOID)CcPurgeCacheSection(&Fcb->SectionObjectPointers, NULL, 0, FALSE);
                    (VOID)MmForceSectionClosed(&Fcb->SectionObjectPointers, TRUE);
                }
            }

            if (FileObject->Flags & FO_NO_INTERMEDIATE_BUFFERING) {
                Fcb->NonCachedUncleanCount--;
            }
        }

        IoRemoveShareAccess(FileObject, &Fcb->ShareAccess);

        Fcb->UncleanCount--;
        Vcb->UncleanCount--;

        if (Ccb->Flags & CCB_FLAG_WRITE_HANDLE) {

            Ccb->Flags &= ~CCB_FLAG_WRITE_HANDLE;
            Vcb->WriteCount--;

            /* Removable media is left clean once nothing writes to it */
            if (Vcb->WriteCount == 0 && Writable && (Vcb->VcbState & VCB_STATE_FLUSH_ON_CLOSE)) {
                (VOID)ExnFlushVolume(Vcb, TRUE);
            }
        }

        if (ExnIsFcb(Fcb)) {
            Fcb->Header.IsFastIoPossible = ExnIsFastIoPossible(Fcb);
        }

        FileObject->Flags |= FO_CLEANUP_COMPLETE;

    } __finally {

        if (FcbAcquired) {
            ExnRelease(&Fcb->Resource);
        }

        ExnRelease(&Vcb->Resource);
    }

    return STATUS_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Close                                                               */
/* ------------------------------------------------------------------ */

/* Called with Vcb->Resource exclusive; TRUE: the VCB can be deleted */
static BOOLEAN
ExnCloseFcb (
    PEXN_VCB Vcb,
    PEXN_FCB Fcb,
    PEXN_CCB Ccb
    )
{
    if (Ccb == NULL) {

        /* no internal streams in EXTNT */
        UNREFERENCED_PARAMETER(Fcb);

    } else {

        ExnDeleteCcb(Ccb);

        /* Mapped writes after the last cleanup can still move VDL */
        if (Fcb->RefCount == 1 && (Fcb->FcbState & FCB_STATE_INODE_DIRTY) &&
            !(Vcb->VcbState & (VCB_STATE_DISMOUNTED | VCB_STATE_READ_ONLY))) {

            (VOID)ExnUpdateInode(Vcb, Fcb);
        }

        ExnDereferenceFcb(Vcb, Fcb);
        Vcb->OpenCount--;
    }

    return ExnTryTeardown(Vcb, 0);
}

static VOID
NTAPI
ExnCloseWorker (
    PVOID Context
    )
{
    PEXN_CLOSE_ITEM Item = (PEXN_CLOSE_ITEM)Context;
    PEXN_VCB Vcb = Item->Vcb;
    BOOLEAN Delete;

    FsRtlEnterFileSystem();

    (VOID)ExAcquireResourceExclusiveLite(&Vcb->Resource, TRUE);

    __try {

        Delete = ExnCloseFcb(Vcb, Item->Fcb, Item->Ccb);

    } __finally {

        ExnRelease(&Vcb->Resource);
    }

    if (Delete) {
        ExnDeleteVcb(Vcb);
    }

    FsRtlExitFileSystem();

    ExFreePool(Item);
}

/*
 * A close can arrive from inside the cache or memory manager while this
 * thread already holds the volume shared; waiting for it exclusive would
 * then deadlock. Such closes are finished by a worker thread.
 */
NTSTATUS
ExnCommonClose (
    PEXN_IRP_CONTEXT Ctx
    )
{
    PFILE_OBJECT FileObject = Ctx->IrpSp->FileObject;
    PEXN_FCB Fcb = (PEXN_FCB)FileObject->FsContext;
    PEXN_CCB Ccb = (PEXN_CCB)FileObject->FsContext2;
    PEXN_VCB Vcb = Ctx->Vcb;
    PEXN_CLOSE_ITEM Item;
    BOOLEAN Delete;

    if (Fcb == NULL) {
        return STATUS_SUCCESS;
    }

    FileObject->FsContext = NULL;
    FileObject->FsContext2 = NULL;

    if (!ExAcquireResourceExclusiveLite(&Vcb->Resource, FALSE)) {

        Item = NULL;

        if (IoGetTopLevelIrp() != Ctx->Irp) {
            Item = (PEXN_CLOSE_ITEM)ExAllocatePoolWithTag(NonPagedPool, sizeof(EXN_CLOSE_ITEM),
                                                          EXN_TAG_WORK);
        }

        if (Item != NULL) {

            Item->Vcb = Vcb;
            Item->Fcb = Fcb;
            Item->Ccb = Ccb;

            /* Deprecated in the WDK, but the only work queue NT 3.51/4.0 have */
#if defined(_MSC_VER) && _MSC_VER >= 1300
#pragma warning(push)
#pragma warning(disable: 4995 4996)
#endif
            ExInitializeWorkItem(&Item->Item, ExnCloseWorker, Item);
            ExQueueWorkItem(&Item->Item, DelayedWorkQueue);
#if defined(_MSC_VER) && _MSC_VER >= 1300
#pragma warning(pop)
#endif

            return STATUS_SUCCESS;
        }

        (VOID)ExAcquireResourceExclusiveLite(&Vcb->Resource, TRUE);
    }

    __try {

        Delete = ExnCloseFcb(Vcb, Fcb, Ccb);

    } __finally {

        ExnRelease(&Vcb->Resource);
    }

    if (Delete) {
        ExnDeleteVcb(Vcb);
    }

    return STATUS_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Deleting                                                            */
/* ------------------------------------------------------------------ */

/* The in-memory inode of directory Ino: its DCB's copy when it has one
   (that copy is the one every change goes through), else Temp read now */
PEXT_INODE
ExnDirInode (
    PEXN_VCB Vcb,
    ULONG Ino,
    PEXT_INODE Temp
    )
{
    PEXN_FCB Dcb = ExnFindFcbByIno(Vcb, Ino, FALSE);
    if (Dcb != NULL && ExnIsDcb(Dcb)) {
        return &Dcb->Inode;
    }
    if (!NT_SUCCESS(ExtReadInode(&Vcb->Vol, Ino, Temp))) {
        return NULL;
    }
    return Temp;
}

/* Remembers which name a delete through this handle removes */
VOID
ExnSetDeleteName (
    PEXN_FCB Fcb,
    PEXN_CCB Ccb
    )
{
    /* the handle's own name matters only for a file with several links: a
       single name is always the FCB's (renames keep that one current) */
    if (Ccb != NULL && (Ccb->Flags & CCB_FLAG_HAVE_NAME) &&
        (Fcb->ParentDcb == NULL || (ExnIsFcb(Fcb) && Fcb->Inode.LinksCount > 1))) {
        Fcb->DeleteParentIno = Ccb->ParentIno;
        Fcb->DeleteNameLength = Ccb->NameLength;
        RtlCopyMemory(Fcb->DeleteName, Ccb->Name, Ccb->NameLength + 1);
    } else if (Fcb->ParentDcb != NULL) {
        Fcb->DeleteParentIno = Fcb->ParentDcb->Ino;
        Fcb->DeleteNameLength = Fcb->ExtNameLength;
        RtlCopyMemory(Fcb->DeleteName, Fcb->ExtName, Fcb->ExtNameLength + 1);
    }
}

/*
 * Removes the name a pending delete names. When it was the last link the
 * file or (empty) directory goes from the disk with its data and xattrs;
 * the FCB stays (marked deleted) for Cc and Mm. Vcb->Resource exclusive.
 */
NTSTATUS
ExnUnlinkName (
    PEXN_VCB Vcb,
    PEXN_FCB Fcb
    )
{
    EXT_INODE Temp;
    PEXT_INODE Dir;
    NTSTATUS Status;
    PEXN_FCB Parent;

    if (Fcb->FcbState & FCB_STATE_DELETED) {
        return STATUS_SUCCESS;
    }
    if (Fcb->DeleteNameLength == 0) {
        ExnSetDeleteName(Fcb, NULL);
    }

    /* Nothing may reach the blocks about to be freed */
    (VOID)ExAcquireResourceExclusiveLite(&Fcb->PagingIoResource, TRUE);

    ExnEngineEnter(Vcb);
    Dir = ExnDirInode(Vcb, Fcb->DeleteParentIno, &Temp);
    if (Dir == NULL) {
        Status = STATUS_FILE_CORRUPT_ERROR;
    } else {
        Status = ExtUnlink(&Vcb->Vol, Fcb->DeleteParentIno, Dir, (const char *)Fcb->DeleteName,
                           Fcb->DeleteNameLength);
    }
    if (NT_SUCCESS(Status)) {
        /* the engine changed the inode on the disk: our copy follows */
        if (!NT_SUCCESS(ExtReadInode(&Vcb->Vol, Fcb->Ino, &Fcb->Inode)) ||
            Fcb->Inode.LinksCount == 0 || ExnIsDcb(Fcb) || (Fcb->FcbState & FCB_STATE_SYMLINK)) {
            Fcb->FcbState |= FCB_STATE_DELETED;
        }
        ExnForgetMapping(Fcb);
    }
    Status = ExnEngineLeave(Vcb, Status);

    if (NT_SUCCESS(Status)) {
        Parent = ExnFindFcbByIno(Vcb, Fcb->DeleteParentIno, FALSE);
        if (Parent != NULL && ExnIsDcb(Parent)) {
            ExnIndexRemove(Parent, Fcb->DeleteName, Fcb->DeleteNameLength);
        }
        Fcb->DeleteNameLength = 0;
        if (Fcb->FcbState & FCB_STATE_DELETED) {
            Fcb->FcbState &= ~FCB_STATE_INODE_DIRTY;
            if (Fcb->Runs != NULL) {
                ExFreePool(Fcb->Runs);
                Fcb->Runs = NULL;
            }
            Fcb->RunCount = 0;
            Fcb->Header.AllocationSize.QuadPart = 0;
            Fcb->Header.FileSize.QuadPart = 0;
            Fcb->Header.ValidDataLength.QuadPart = 0;
        }
    }

    ExnRelease(&Fcb->PagingIoResource);
    return Status;
}
