/*
 * EXTNT - fast I/O and cache manager callbacks
 */

#include "ext.h"

static BOOLEAN
NTAPI
ExnFastIoNoWrite (
    PFILE_OBJECT FileObject,
    PLARGE_INTEGER FileOffset,
    ULONG Length,
    BOOLEAN Wait,
    ULONG LockKey,
    PVOID Buffer,
    PIO_STATUS_BLOCK IoStatus,
    PDEVICE_OBJECT DeviceObject
    )
{
    UNREFERENCED_PARAMETER(FileObject);
    UNREFERENCED_PARAMETER(FileOffset);
    UNREFERENCED_PARAMETER(Length);
    UNREFERENCED_PARAMETER(Wait);
    UNREFERENCED_PARAMETER(LockKey);
    UNREFERENCED_PARAMETER(Buffer);
    UNREFERENCED_PARAMETER(IoStatus);
    UNREFERENCED_PARAMETER(DeviceObject);
    return FALSE;
}

UCHAR
ExnIsFastIoPossible (
    PEXN_FCB Fcb
    )
{
    if (Fcb->Vcb->VcbState & VCB_STATE_DISMOUNTED) {
        return FastIoIsNotPossible;
    }

    if (FsRtlAreThereCurrentFileLocks(&Fcb->FileLock)) {
        return FastIoIsQuestionable;
    }

    return FastIoIsPossible;
}

static BOOLEAN
ExnFastIoUsable (
    PEXN_FCB Fcb
    )
{
    if (Fcb == NULL || (!ExnIsFcb(Fcb) && !ExnIsDcb(Fcb))) {
        return FALSE;
    }

    if ((Fcb->Vcb->VcbState & VCB_STATE_DISMOUNTED) ||
        (Fcb->Vcb->Vpb->RealDevice->Flags & DO_VERIFY_VOLUME)) {

        return FALSE;
    }

    return TRUE;
}

static BOOLEAN
NTAPI
ExnFastIoCheckIfPossible (
    PFILE_OBJECT FileObject,
    PLARGE_INTEGER FileOffset,
    ULONG Length,
    BOOLEAN Wait,
    ULONG LockKey,
    BOOLEAN CheckForReadOperation,
    PIO_STATUS_BLOCK IoStatus,
    PDEVICE_OBJECT DeviceObject
    )
{
    PEXN_FCB Fcb = (PEXN_FCB)FileObject->FsContext;
    LARGE_INTEGER LargeLength;

    UNREFERENCED_PARAMETER(Wait);
    UNREFERENCED_PARAMETER(IoStatus);
    UNREFERENCED_PARAMETER(DeviceObject);

    if (!ExnFastIoUsable(Fcb) || !ExnIsFcb(Fcb)) {
        return FALSE;
    }

    LargeLength.QuadPart = Length;

    if (CheckForReadOperation) {
        return FsRtlFastCheckLockForRead(&Fcb->FileLock, FileOffset, &LargeLength, LockKey,
                                         FileObject, IoGetCurrentProcess());
    }

    if (Fcb->Vcb->VcbState & VCB_STATE_READ_ONLY) {
        return FALSE;
    }

    return FsRtlFastCheckLockForWrite(&Fcb->FileLock, FileOffset, &LargeLength, LockKey,
                                      FileObject, IoGetCurrentProcess());
}

static BOOLEAN
NTAPI
ExnFastQueryBasicInfo (
    PFILE_OBJECT FileObject,
    BOOLEAN Wait,
    PFILE_BASIC_INFORMATION Buffer,
    PIO_STATUS_BLOCK IoStatus,
    PDEVICE_OBJECT DeviceObject
    )
{
    PEXN_FCB Fcb = (PEXN_FCB)FileObject->FsContext;
    FILE_BASIC_INFORMATION Info;

    UNREFERENCED_PARAMETER(DeviceObject);

    if (!ExnFastIoUsable(Fcb)) {
        return FALSE;
    }

    FsRtlEnterFileSystem();

    if (!ExAcquireResourceSharedLite(&Fcb->Resource, Wait)) {
        FsRtlExitFileSystem();
        return FALSE;
    }

    ExnFillBasicInfo(Fcb, &Info);

    ExnRelease(&Fcb->Resource);
    FsRtlExitFileSystem();

    *Buffer = Info;
    IoStatus->Status = STATUS_SUCCESS;
    IoStatus->Information = sizeof(FILE_BASIC_INFORMATION);

    return TRUE;
}

static BOOLEAN
NTAPI
ExnFastQueryStandardInfo (
    PFILE_OBJECT FileObject,
    BOOLEAN Wait,
    PFILE_STANDARD_INFORMATION Buffer,
    PIO_STATUS_BLOCK IoStatus,
    PDEVICE_OBJECT DeviceObject
    )
{
    PEXN_FCB Fcb = (PEXN_FCB)FileObject->FsContext;
    FILE_STANDARD_INFORMATION Info;

    UNREFERENCED_PARAMETER(DeviceObject);

    if (!ExnFastIoUsable(Fcb)) {
        return FALSE;
    }

    FsRtlEnterFileSystem();

    if (!ExAcquireResourceSharedLite(&Fcb->Resource, Wait)) {
        FsRtlExitFileSystem();
        return FALSE;
    }

    ExnFillStandardInfo(Fcb, &Info);

    ExnRelease(&Fcb->Resource);
    FsRtlExitFileSystem();

    *Buffer = Info;
    IoStatus->Status = STATUS_SUCCESS;
    IoStatus->Information = sizeof(FILE_STANDARD_INFORMATION);

    return TRUE;
}

static BOOLEAN
NTAPI
ExnFastQueryNetworkOpenInfo (
    PFILE_OBJECT FileObject,
    BOOLEAN Wait,
    PFILE_NETWORK_OPEN_INFORMATION Buffer,
    PIO_STATUS_BLOCK IoStatus,
    PDEVICE_OBJECT DeviceObject
    )
{
    PEXN_FCB Fcb = (PEXN_FCB)FileObject->FsContext;
    FILE_NETWORK_OPEN_INFORMATION Info;

    UNREFERENCED_PARAMETER(DeviceObject);

    if (!ExnFastIoUsable(Fcb)) {
        return FALSE;
    }

    FsRtlEnterFileSystem();

    if (!ExAcquireResourceSharedLite(&Fcb->Resource, Wait)) {
        FsRtlExitFileSystem();
        return FALSE;
    }

    ExnFillNetworkOpenInfo(Fcb, &Info);

    ExnRelease(&Fcb->Resource);
    FsRtlExitFileSystem();

    *Buffer = Info;
    IoStatus->Status = STATUS_SUCCESS;
    IoStatus->Information = sizeof(FILE_NETWORK_OPEN_INFORMATION);

    return TRUE;
}

#ifdef EXN_NT31
/*
 * NT 3.1 calls the fast I/O routines without the DeviceObject parameter that
 * NT 3.5 added: these routines pop what it pushes (stdcall), or the caller's
 * stack - and the registers it restores from it - would be off by four bytes.
 */
static BOOLEAN NTAPI
ExnFastIoCheckIfPossible31 (PFILE_OBJECT FileObject, PLARGE_INTEGER FileOffset, ULONG Length, BOOLEAN Wait,
                            ULONG LockKey, BOOLEAN CheckForReadOperation, PIO_STATUS_BLOCK IoStatus)
{
    return ExnFastIoCheckIfPossible(FileObject, FileOffset, Length, Wait, LockKey, CheckForReadOperation, IoStatus, NULL);
}

static BOOLEAN NTAPI
ExnFastIoNoWrite31 (PFILE_OBJECT FileObject, PLARGE_INTEGER FileOffset, ULONG Length, BOOLEAN Wait,
                    ULONG LockKey, PVOID Buffer, PIO_STATUS_BLOCK IoStatus)
{
    return ExnFastIoNoWrite(FileObject, FileOffset, Length, Wait, LockKey, Buffer, IoStatus, NULL);
}

static BOOLEAN NTAPI
ExnFastQueryBasicInfo31 (PFILE_OBJECT FileObject, BOOLEAN Wait, PFILE_BASIC_INFORMATION Buffer, PIO_STATUS_BLOCK IoStatus)
{
    return ExnFastQueryBasicInfo(FileObject, Wait, Buffer, IoStatus, NULL);
}

static BOOLEAN NTAPI
ExnFastQueryStandardInfo31 (PFILE_OBJECT FileObject, BOOLEAN Wait, PFILE_STANDARD_INFORMATION Buffer, PIO_STATUS_BLOCK IoStatus)
{
    return ExnFastQueryStandardInfo(FileObject, Wait, Buffer, IoStatus, NULL);
}
#endif

VOID
ExnInitializeFastIo (
    PFAST_IO_DISPATCH FastIo
    )
{
    RtlZeroMemory(FastIo, sizeof(FAST_IO_DISPATCH));

    FastIo->SizeOfFastIoDispatch = sizeof(FAST_IO_DISPATCH);
    FastIo->FastIoCheckIfPossible = ExnFastIoCheckIfPossible;
    FastIo->FastIoRead = FsRtlCopyRead;
    /* Writes take the IRP path: blocks are found for the data before it
       enters the cache (holes, inline data), so a full disk is reported to
       the writer and not to the lazy writer */
    FastIo->FastIoWrite = ExnFastIoNoWrite;
    FastIo->FastIoQueryBasicInfo = ExnFastQueryBasicInfo;
    FastIo->FastIoQueryStandardInfo = ExnFastQueryStandardInfo;
    FastIo->FastIoQueryNetworkOpenInfo = ExnFastQueryNetworkOpenInfo;
#ifdef EXN_NT31
    if (ExnNt31) {
        /* FsRtlCopyRead is NT 3.1's own: it already takes what NT 3.1 passes */
        FastIo->FastIoCheckIfPossible = (PFAST_IO_CHECK_IF_POSSIBLE)ExnFastIoCheckIfPossible31;
        FastIo->FastIoWrite = (PFAST_IO_WRITE)ExnFastIoNoWrite31;
        FastIo->FastIoQueryBasicInfo = (PFAST_IO_QUERY_BASIC_INFO)ExnFastQueryBasicInfo31;
        FastIo->FastIoQueryStandardInfo = (PFAST_IO_QUERY_STANDARD_INFO)ExnFastQueryStandardInfo31;
        FastIo->FastIoQueryNetworkOpenInfo = NULL;
    }
#endif
}

/* ------------------------------------------------------------------ */
/* Cache manager callbacks                                             */
/* ------------------------------------------------------------------ */

static BOOLEAN
NTAPI
ExnAcquireForLazyWrite (
    PVOID Context,
    BOOLEAN Wait
    )
{
    PEXN_FCB Fcb = (PEXN_FCB)Context;

    FsRtlEnterFileSystem();

    if (!ExAcquireResourceSharedLite(&Fcb->PagingIoResource, Wait)) {
        FsRtlExitFileSystem();
        return FALSE;
    }

    /* Paging writes from this thread must not move VDL */
    Fcb->LazyWriteThread = KeGetCurrentThread();

    if (IoGetTopLevelIrp() == NULL) {
        IoSetTopLevelIrp((PIRP)FSRTL_CACHE_TOP_LEVEL_IRP);
    }

    return TRUE;
}

static VOID
NTAPI
ExnReleaseFromLazyWrite (
    PVOID Context
    )
{
    PEXN_FCB Fcb = (PEXN_FCB)Context;

    if (IoGetTopLevelIrp() == (PIRP)FSRTL_CACHE_TOP_LEVEL_IRP) {
        IoSetTopLevelIrp(NULL);
    }

    Fcb->LazyWriteThread = NULL;

    ExnRelease(&Fcb->PagingIoResource);
    FsRtlExitFileSystem();
}

static BOOLEAN
NTAPI
ExnAcquireForReadAhead (
    PVOID Context,
    BOOLEAN Wait
    )
{
    PEXN_FCB Fcb = (PEXN_FCB)Context;

    FsRtlEnterFileSystem();

    if (!ExAcquireResourceSharedLite(&Fcb->Resource, Wait)) {
        FsRtlExitFileSystem();
        return FALSE;
    }

    if (IoGetTopLevelIrp() == NULL) {
        IoSetTopLevelIrp((PIRP)FSRTL_CACHE_TOP_LEVEL_IRP);
    }

    return TRUE;
}

static VOID
NTAPI
ExnReleaseFromReadAhead (
    PVOID Context
    )
{
    PEXN_FCB Fcb = (PEXN_FCB)Context;

    if (IoGetTopLevelIrp() == (PIRP)FSRTL_CACHE_TOP_LEVEL_IRP) {
        IoSetTopLevelIrp(NULL);
    }

    ExnRelease(&Fcb->Resource);
    FsRtlExitFileSystem();
}

VOID
ExnInitializeCacheCallbacks (
    VOID
    )
{
    ExnData.CacheManagerCallbacks.AcquireForLazyWrite = ExnAcquireForLazyWrite;
    ExnData.CacheManagerCallbacks.ReleaseFromLazyWrite = ExnReleaseFromLazyWrite;
    ExnData.CacheManagerCallbacks.AcquireForReadAhead = ExnAcquireForReadAhead;
    ExnData.CacheManagerCallbacks.ReleaseFromReadAhead = ExnReleaseFromReadAhead;

}
