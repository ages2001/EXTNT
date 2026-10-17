/*
 * EXTNT - IRP_MJ_WRITE
 *
 * ext files are sparse: a block that was never written is a hole and reads
 * as zeros. ValidDataLength (VDL) is kept in memory only, as the size the
 * disk is known to hold data for; writing past it zeroes the gap, but only
 * where the gap has blocks (the old last page through the cache, allocated
 * blocks after it on the disk) - holes stay holes.
 *
 * Cached writes give the range its blocks before the data enters the cache,
 * so a full disk is reported to the writer and not to the lazy writer. The
 * inode gets the new size when the handle is cleaned up or the file flushed.
 *
 * Only paging writes that the lazy writer did not issue may move VDL
 * forward (mapped files, flushes we start ourselves); whoever issues them
 * holds the file exclusive.
 */

#include "ext.h"

/* Cache Manager calls, run through ExnTryCall (the Cache Manager raises) */
typedef struct _EXN_CC_WRITE {
    PFILE_OBJECT FileObject;
    PEXN_FCB   Fcb;
    PIRP        Irp;
    BOOLEAN     InitOnly;           /* only set up caching */
    BOOLEAN     Mdl;
    LARGE_INTEGER Start;
    ULONG       Count;
    PVOID       Buffer;
    NTSTATUS    Status;
} EXN_CC_WRITE;

static VOID
ExnCcWriteBody (
    PVOID Context
    )
{
    EXN_CC_WRITE *C = (EXN_CC_WRITE *)Context;

    if (C->Fcb != NULL && C->FileObject->PrivateCacheMap == NULL) {
        CcInitializeCacheMap(C->FileObject, (PCC_FILE_SIZES)&C->Fcb->Header.AllocationSize, FALSE,
                             &ExnData.CacheManagerCallbacks, C->Fcb);
    }
    if (C->InitOnly) {
        return;
    }
    if (C->Mdl) {
        CcPrepareMdlWrite(C->FileObject, &C->Start, C->Count, &C->Irp->MdlAddress, &C->Irp->IoStatus);
        C->Status = C->Irp->IoStatus.Status;
    } else if (!CcCopyWrite(C->FileObject, &C->Start, C->Count, TRUE, C->Buffer)) {
        C->Status = STATUS_UNSUCCESSFUL;
    }
}

typedef struct _EXN_CC_ZERO {
    PFILE_OBJECT FileObject;
    LARGE_INTEGER From;
    LARGE_INTEGER To;
} EXN_CC_ZERO;

static VOID
ExnCcZeroBody (
    PVOID Context
    )
{
    EXN_CC_ZERO *C = (EXN_CC_ZERO *)Context;
    (VOID)CcZeroData(C->FileObject, &C->From, &C->To, TRUE);
}

#ifndef FSRTL_FLAG_USER_MAPPED_FILE
#define FSRTL_FLAG_USER_MAPPED_FILE     0x20
#endif

/*
 * Zeroes [Start, End) of a file whose data up to Start is valid. Through
 * the cache when this file object caches the file, else on the disk.
 */
/* Writes Length zero bytes at Offset through the cache */
static NTSTATUS
ExnCopyZeros (
    PFILE_OBJECT FileObject,
    LONGLONG Offset,
    ULONG Length
    )
{
    LARGE_INTEGER From;
    PVOID Zero;

    Zero = ExAllocatePoolWithTag(PagedPool, Length, EXN_TAG_BUFFER);
    if (Zero == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlZeroMemory(Zero, Length);
    From.QuadPart = Offset;

    {
        EXN_CC_WRITE Cc;
        NTSTATUS Code;

        RtlZeroMemory(&Cc, sizeof(Cc));
        Cc.FileObject = FileObject;
        Cc.Start = From;
        Cc.Count = Length;
        Cc.Buffer = Zero;
        Code = ExnTryCall(ExnCcWriteBody, &Cc, FALSE);

        ExFreePool(Zero);
        return NT_SUCCESS(Code) ? STATUS_SUCCESS : ExnExceptionStatus(Code);
    }
}

/*
 * Zeroes [Start, End) of a file whose data up to Start is valid; End is
 * within FileSize. The page holding Start goes through the cache when this
 * file object caches the file; the rest is zeroed on the disk where it has
 * blocks (holes are zero already), with any cached copy of it dropped.
 */
NTSTATUS
ExnZeroFileRange (
    PEXN_FCB Fcb,
    PFILE_OBJECT FileObject,
    LONGLONG Start,
    LONGLONG End
    )
{
    PEXN_VCB Vcb = Fcb->Vcb;
    LONGLONG PageEnd;
    LARGE_INTEGER From;
    NTSTATUS Status;

    if (Start >= End) {
        return STATUS_SUCCESS;
    }

    if (FileObject == NULL || FileObject->PrivateCacheMap == NULL) {
        return ExnZeroDisk(Vcb, Fcb, Start, End);
    }

    PageEnd = ExnRoundUp(Start, EXN_PAGE_SIZE);
    if (PageEnd > End) {
        PageEnd = End;
    }
    if (Start < PageEnd) {
        Status = ExnCopyZeros(FileObject, Start, (ULONG)(PageEnd - Start));
        if (!NT_SUCCESS(Status)) {
            return Status;
        }
    }
    if (PageEnd < End) {
        From.QuadPart = PageEnd;
        (VOID)CcPurgeCacheSection(&Fcb->SectionObjectPointers, &From, (ULONG)
                                  ((End - PageEnd) > 0x7FFFF000 ? 0x7FFFF000 : (End - PageEnd)), FALSE);
        return ExnZeroDisk(Vcb, Fcb, PageEnd, End);
    }
    return STATUS_SUCCESS;
}

/* DASD writes, only through a handle that locked or dismounted the volume */
static NTSTATUS
ExnWriteVolume (
    PEXN_IRP_CONTEXT Ctx,
    PEXN_FCB Vfcb,
    PEXN_CCB Ccb,
    LONGLONG StartingByte,
    ULONG ByteCount
    )
{
    PIRP Irp = Ctx->Irp;
    PFILE_OBJECT FileObject = Ctx->IrpSp->FileObject;
    PEXN_VCB Vcb = Vfcb->Vcb;
    NTSTATUS Status = STATUS_SUCCESS;

    if (Ccb == NULL || (Irp->Flags & IRP_PAGING_IO)) {
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    (VOID)ExAcquireResourceExclusiveLite(&Vcb->Resource, TRUE);

    __try {

        if (Vcb->LockFileObject != FileObject && !(Ccb->Flags & CCB_FLAG_DISMOUNTED_VOLUME)) {
            Status = STATUS_ACCESS_DENIED;
            __leave;
        }

        if (((ULONG)StartingByte & (EXN_SECTOR_SIZE - 1)) != 0 ||
            (ByteCount & (EXN_SECTOR_SIZE - 1)) != 0) {

            Status = STATUS_INVALID_PARAMETER;
            __leave;
        }

        if (StartingByte >= Vcb->PartitionBytes) {
            Status = STATUS_END_OF_FILE;
            __leave;
        }

        if (StartingByte + ByteCount > Vcb->PartitionBytes) {
            ByteCount = (ULONG)(Vcb->PartitionBytes - StartingByte);
        }

        Vcb->VcbState |= VCB_STATE_DASD_WRITTEN;

        /* The engine's cached view of the disk is about to go stale */
        ExnEngineEnter(Vcb);
        (VOID)ExnCommit(Vcb);
        ExnMetaPurge(Vcb);
        ExnRelease(&Vcb->EngineResource);

        Status = ExnLockUserBuffer(Irp, IoReadAccess, ByteCount);
        if (!NT_SUCCESS(Status)) {
            __leave;
        }

        Status = ExnNonCachedIo(Ctx, Vfcb, IRP_MJ_WRITE, StartingByte, ByteCount, Vcb->PartitionBytes);

        if (NT_SUCCESS(Status)) {

            Irp->IoStatus.Information = ByteCount;

            if (FileObject->Flags & FO_SYNCHRONOUS_IO) {
                FileObject->CurrentByteOffset.QuadPart = StartingByte + ByteCount;
            }
        }

    } __finally {

        ExnRelease(&Vcb->Resource);
    }

    return Status;
}

/* Paging writes of a file: never past FileSize */
static NTSTATUS
ExnPagingWrite (
    PEXN_IRP_CONTEXT Ctx,
    PEXN_FCB Fcb,
    LONGLONG StartingVbo,
    ULONG ByteCount
    )
{
    PEXN_VCB Vcb = Fcb->Vcb;
    PIRP Irp = Ctx->Irp;
    LONGLONG FileSize;
    LONGLONG ValidData;
    LONGLONG End;
    BOOLEAN ExtendValidData = FALSE;
    NTSTATUS Status = STATUS_SUCCESS;

    (VOID)ExAcquireResourceSharedLite(&Fcb->PagingIoResource, TRUE);

    __try {

        FileSize = Fcb->Header.FileSize.QuadPart;
        ValidData = Fcb->Header.ValidDataLength.QuadPart;

        if (StartingVbo >= FileSize) {
            __leave;
        }

        if (StartingVbo + ByteCount > FileSize) {
            ByteCount = (ULONG)(FileSize - StartingVbo);
        }

        End = StartingVbo + ByteCount;

        /* A paging file is managed entirely by the Memory Manager: never
         * zero-fill or extend valid data for it (doing so from the modified
         * page writer froze the system). Just write the already-allocated run. */
        if (Fcb->FcbState & FCB_STATE_PAGING_FILE) {

            Status = ExnNonCachedIo(Ctx, Fcb, IRP_MJ_WRITE, StartingVbo,
                                    (ULONG)ExnRoundUp(ByteCount, EXN_SECTOR_SIZE), 0);
            if (NT_SUCCESS(Status)) {
                Irp->IoStatus.Information = ByteCount;
            }
            __leave;
        }

        if (End > ValidData) {

            if (Fcb->LazyWriteThread == KeGetCurrentThread()) {

                /* A mapped page past VDL waits for the mapped page writer,
                   which may move VDL */
                if ((Fcb->Header.Flags & FSRTL_FLAG_USER_MAPPED_FILE) &&
                    End > ExnRoundUp(ValidData, EXN_PAGE_SIZE)) {

                    Status = STATUS_FILE_LOCK_CONFLICT;
                    __leave;
                }

            } else {

                ExtendValidData = TRUE;

                if (StartingVbo > ValidData) {
                    Status = ExnZeroDisk(Vcb, Fcb, ValidData, StartingVbo);
                    if (!NT_SUCCESS(Status)) {
                        __leave;
                    }
                }
            }
        }

        Status = ExnNonCachedIo(Ctx, Fcb, IRP_MJ_WRITE, StartingVbo,
                                (ULONG)ExnRoundUp(ByteCount, EXN_SECTOR_SIZE), 0);

        if (NT_SUCCESS(Status)) {

            Irp->IoStatus.Information = ByteCount;

            if (ExtendValidData && End > Fcb->Header.ValidDataLength.QuadPart) {
                Fcb->Header.ValidDataLength.QuadPart = End;
                Fcb->FcbState |= FCB_STATE_INODE_DIRTY;
            }
        }

    } __finally {

        ExnRelease(&Fcb->PagingIoResource);
    }

    return Status;
}

static NTSTATUS
ExnWriteFile (
    PEXN_IRP_CONTEXT Ctx,
    PEXN_FCB Fcb,
    LARGE_INTEGER StartingByte,
    ULONG ByteCount
    )
{
    PEXN_VCB Vcb = Fcb->Vcb;
    PIRP Irp = Ctx->Irp;
    PIO_STACK_LOCATION IrpSp = Ctx->IrpSp;
    PFILE_OBJECT FileObject = IrpSp->FileObject;
    BOOLEAN NonCached = (BOOLEAN)((Irp->Flags & IRP_NOCACHE) != 0);
    BOOLEAN WriteToEof;
    BOOLEAN VcbAcquired = FALSE;
    BOOLEAN FcbAcquired = FALSE;
    BOOLEAN PagingAcquired = FALSE;
    BOOLEAN Extended = FALSE;
    LONGLONG OldFileSize = 0;
    LONGLONG OldValidData = 0;
    LONGLONG End;
    IO_STATUS_BLOCK Iosb;
    PVOID Buffer;
    NTSTATUS Status;

    WriteToEof = (BOOLEAN)(StartingByte.LowPart == FILE_WRITE_TO_END_OF_FILE &&
                           StartingByte.HighPart == -1);

    Status = ExnVerifyWritable(Ctx, Vcb);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    if (NonCached &&
        ((!WriteToEof && ((ULONG)StartingByte.QuadPart & (EXN_SECTOR_SIZE - 1)) != 0) ||
         (ByteCount & (EXN_SECTOR_SIZE - 1)) != 0)) {

        return STATUS_INVALID_PARAMETER;
    }

    /* Throttle before taking anything the lazy writer may need */
    if (!NonCached) {
        (VOID)CcCanIWrite(FileObject, ByteCount, TRUE, FALSE);
    }

    __try {

        (VOID)ExAcquireResourceSharedLite(&Vcb->Resource, TRUE);
        VcbAcquired = TRUE;

        (VOID)ExAcquireResourceExclusiveLite(&Fcb->Resource, TRUE);
        FcbAcquired = TRUE;

        Status = ExnVerifyWritable(Ctx, Vcb);
        if (!NT_SUCCESS(Status)) {
            __leave;
        }

        if (Fcb->FcbState & FCB_STATE_DELETED) {
            Status = STATUS_FILE_DELETED;
            __leave;
        }

        if (WriteToEof) {
            StartingByte = Fcb->Header.FileSize;
        }

        if (NonCached && ((ULONG)StartingByte.QuadPart & (EXN_SECTOR_SIZE - 1)) != 0) {
            Status = STATUS_INVALID_PARAMETER;
            __leave;
        }

        /* Keep the cache coherent with what goes straight to the disk */
        if (NonCached && Fcb->SectionObjectPointers.DataSectionObject != NULL) {

            (VOID)ExAcquireResourceExclusiveLite(&Fcb->PagingIoResource, TRUE);
            PagingAcquired = TRUE;

            CcFlushCache(&Fcb->SectionObjectPointers, &StartingByte, ByteCount, &Iosb);

            if (!NT_SUCCESS(Iosb.Status)) {
                Status = Iosb.Status;
                __leave;
            }

            (VOID)CcPurgeCacheSection(&Fcb->SectionObjectPointers, &StartingByte, ByteCount, FALSE);
        }

        if (!FsRtlCheckLockForWriteAccess(&Fcb->FileLock, Irp)) {
            Status = STATUS_FILE_LOCK_CONFLICT;
            __leave;
        }

        End = StartingByte.QuadPart + ByteCount;

        if (StartingByte.QuadPart < 0 || End < StartingByte.QuadPart) {
            Status = STATUS_INVALID_PARAMETER;
            __leave;
        }

        OldFileSize = Fcb->Header.FileSize.QuadPart;
        OldValidData = Fcb->Header.ValidDataLength.QuadPart;

        if (End > (LONGLONG)ExnMaxFileSize(Vcb)) {
            Status = STATUS_DISK_FULL;
            __leave;
        }

        /* the blocks first: a full disk fails this write, not a later flush */
        if (!NonCached) {
            Status = ExnAllocateRange(Vcb, Fcb, StartingByte.QuadPart, End);
            if (!NT_SUCCESS(Status)) {
                __leave;
            }
        }

        if (End > OldFileSize) {

            if (End > Fcb->Header.AllocationSize.QuadPart) {
                Fcb->Header.AllocationSize.QuadPart = ExnRoundUp(End, Vcb->Vol.BlockSize);
            }

            Fcb->Header.FileSize.QuadPart = End;
            Fcb->FcbState |= FCB_STATE_INODE_DIRTY;
            Extended = TRUE;

            if (Fcb->SectionObjectPointers.SharedCacheMap != NULL) {
                CcSetFileSizes(FileObject, (PCC_FILE_SIZES)&Fcb->Header.AllocationSize);
            }
        }

        if (!NonCached) {

            EXN_CC_WRITE Cc;
            NTSTATUS Code;

            Buffer = NULL;
            if (!(IrpSp->MinorFunction & IRP_MN_MDL)) {
                Buffer = ExnMapUserBuffer(Irp);
                if (Buffer == NULL) {
                    Status = STATUS_INSUFFICIENT_RESOURCES;
                    __leave;
                }
            }

            RtlZeroMemory(&Cc, sizeof(Cc));
            Cc.FileObject = FileObject;
            Cc.Fcb = Fcb;
            Cc.Irp = Irp;
            Cc.InitOnly = TRUE;
            Code = ExnTryCall(ExnCcWriteBody, &Cc, FALSE);
            if (!NT_SUCCESS(Code)) {
                Status = ExnExceptionStatus(Code);
            }

            if (!NT_SUCCESS(Status)) {
                __leave;
            }

            if (StartingByte.QuadPart > OldValidData) {
                Status = ExnZeroFileRange(Fcb, FileObject, OldValidData, StartingByte.QuadPart);
                if (!NT_SUCCESS(Status)) {
                    __leave;
                }
            }

            Cc.InitOnly = FALSE;
            Cc.Mdl = (BOOLEAN)((IrpSp->MinorFunction & IRP_MN_MDL) != 0);
            Cc.Start = StartingByte;
            Cc.Count = ByteCount;
            Cc.Buffer = Buffer;
            Cc.Status = Status;
            Code = ExnTryCall(ExnCcWriteBody, &Cc, FALSE);
            Status = NT_SUCCESS(Code) ? Cc.Status : ExnExceptionStatus(Code);

            if (!NT_SUCCESS(Status)) {
                __leave;
            }

        } else {

            if (StartingByte.QuadPart > OldValidData) {
                Status = ExnZeroDisk(Vcb, Fcb, OldValidData, StartingByte.QuadPart);
                if (!NT_SUCCESS(Status)) {
                    __leave;
                }
            }

            Status = ExnLockUserBuffer(Irp, IoReadAccess, ByteCount);
            if (!NT_SUCCESS(Status)) {
                __leave;
            }

            Status = ExnNonCachedIo(Ctx, Fcb, IRP_MJ_WRITE, StartingByte.QuadPart, ByteCount, 0);
        }

        if (!NT_SUCCESS(Status)) {
            __leave;
        }

        if (End > Fcb->Header.ValidDataLength.QuadPart) {

            Fcb->Header.ValidDataLength.QuadPart = End;
            Fcb->FcbState |= FCB_STATE_INODE_DIRTY;

            /* Tell the cache the disk now holds valid data up to here */
            if (NonCached && Fcb->SectionObjectPointers.SharedCacheMap != NULL) {
                CcSetFileSizes(FileObject, (PCC_FILE_SIZES)&Fcb->Header.AllocationSize);
            }
        }

        Irp->IoStatus.Information = ByteCount;

        if (FileObject->Flags & FO_SYNCHRONOUS_IO) {
            FileObject->CurrentByteOffset.QuadPart = End;
        }

        FileObject->Flags |= FO_FILE_MODIFIED;
        Fcb->FcbState |= FCB_STATE_INODE_DIRTY;     /* mtime/ctime at cleanup */

        if (Extended) {
            FileObject->Flags |= FO_FILE_SIZE_CHANGED;
        }

    } __finally {

        if (Extended && (AbnormalTermination() || !NT_SUCCESS(Status))) {

            /* Pull the sizes back; the allocation goes at cleanup */
            if (!PagingAcquired) {
                (VOID)ExAcquireResourceExclusiveLite(&Fcb->PagingIoResource, TRUE);
                PagingAcquired = TRUE;
            }

            Fcb->Header.FileSize.QuadPart = OldFileSize;
            Fcb->Header.ValidDataLength.QuadPart = OldValidData;
            Fcb->FcbState |= FCB_STATE_TRUNCATE_ON_CLOSE;

            if (Fcb->SectionObjectPointers.SharedCacheMap != NULL) {
                CcSetFileSizes(FileObject, (PCC_FILE_SIZES)&Fcb->Header.AllocationSize);
            }
        }

        if (PagingAcquired) {
            ExnRelease(&Fcb->PagingIoResource);
        }

        if (FcbAcquired) {
            ExnRelease(&Fcb->Resource);
        }

        if (VcbAcquired) {
            ExnRelease(&Vcb->Resource);
        }
    }

    return Status;
}

NTSTATUS
ExnCommonWrite (
    PEXN_IRP_CONTEXT Ctx
    )
{
    PIRP Irp = Ctx->Irp;
    PIO_STACK_LOCATION IrpSp = Ctx->IrpSp;
    PFILE_OBJECT FileObject = IrpSp->FileObject;
    PEXN_FCB Fcb = (PEXN_FCB)FileObject->FsContext;
    PEXN_CCB Ccb = (PEXN_CCB)FileObject->FsContext2;
    ULONG ByteCount = IrpSp->Parameters.Write.Length;

    if (Fcb == NULL) {
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    if (IrpSp->MinorFunction & IRP_MN_COMPLETE) {
        CcMdlWriteComplete(FileObject, &IrpSp->Parameters.Write.ByteOffset, Irp->MdlAddress);
        Irp->MdlAddress = NULL;
        return STATUS_SUCCESS;
    }

    Irp->IoStatus.Information = 0;

    if (ByteCount == 0) {
        return STATUS_SUCCESS;
    }

    if (ExnIsVfcb(Fcb)) {
        return ExnWriteVolume(Ctx, Fcb, Ccb, IrpSp->Parameters.Write.ByteOffset.QuadPart, ByteCount);
    }

    if (!ExnIsFcb(Fcb) || (Fcb->FcbState & FCB_STATE_SYMLINK)) {
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    if (Irp->Flags & IRP_PAGING_IO) {
        return ExnPagingWrite(Ctx, Fcb, IrpSp->Parameters.Write.ByteOffset.QuadPart, ByteCount);
    }

    return ExnWriteFile(Ctx, Fcb, IrpSp->Parameters.Write.ByteOffset, ByteCount);
}
