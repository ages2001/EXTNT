/*
 * EXTNT - IRP dispatch, exception handling and volume verification
 *
 * Every request is handled synchronously in the caller's thread. Handlers
 * return a status and leave completion to ExnFsdDispatch unless they set
 * EXN_CTX_NO_COMPLETE.
 */

#include "ext.h"

static NTSTATUS
ExnCommonDispatch (
    PEXN_IRP_CONTEXT Ctx
    )
{
    UCHAR Major = Ctx->IrpSp->MajorFunction;


    /* The file system device itself */
    if (Ctx->Vcb == NULL) {

        switch (Major) {

        case IRP_MJ_CREATE:
            Ctx->IrpSp->FileObject->FsContext = NULL;
            Ctx->IrpSp->FileObject->FsContext2 = NULL;
            Ctx->Irp->IoStatus.Information = FILE_OPENED;
            return STATUS_SUCCESS;

        case IRP_MJ_CLEANUP:
        case IRP_MJ_CLOSE:
            return STATUS_SUCCESS;

        case IRP_MJ_FILE_SYSTEM_CONTROL:
            return ExnCommonFileSystemControl(Ctx);

        case IRP_MJ_SHUTDOWN:
            return ExnCommonShutdown(Ctx);

        default:
            return STATUS_INVALID_DEVICE_REQUEST;
        }
    }

    switch (Major) {

    case IRP_MJ_CREATE:
        return ExnCommonCreate(Ctx);

    case IRP_MJ_CLOSE:
        return ExnCommonClose(Ctx);

    case IRP_MJ_CLEANUP:
        return ExnCommonCleanup(Ctx);

    case IRP_MJ_READ:
        return ExnCommonRead(Ctx);

    case IRP_MJ_WRITE:
        return ExnCommonWrite(Ctx);

    case IRP_MJ_QUERY_INFORMATION:
        return ExnCommonQueryInformation(Ctx);

    case IRP_MJ_SET_INFORMATION:
        return ExnCommonSetInformation(Ctx);

    case IRP_MJ_QUERY_VOLUME_INFORMATION:
        return ExnCommonQueryVolumeInformation(Ctx);

    case IRP_MJ_SET_VOLUME_INFORMATION:
        return ExnCommonSetVolumeInformation(Ctx);

    case IRP_MJ_DIRECTORY_CONTROL:
        return ExnCommonDirectoryControl(Ctx);

    case IRP_MJ_FILE_SYSTEM_CONTROL:
        return ExnCommonFileSystemControl(Ctx);

    case IRP_MJ_DEVICE_CONTROL:
        return ExnCommonDeviceControl(Ctx);

    case IRP_MJ_LOCK_CONTROL:
        return ExnCommonLockControl(Ctx);

    case IRP_MJ_FLUSH_BUFFERS:
        return ExnCommonFlushBuffers(Ctx);

    case IRP_MJ_QUERY_EA:
        return ExnCommonQueryEa(Ctx);

    case IRP_MJ_SET_EA:
        return ExnCommonSetEa(Ctx);

#ifndef EXN_NT4
    case IRP_MJ_PNP:
        return ExnCommonPnp(Ctx);
#endif

    default:
        return STATUS_INVALID_DEVICE_REQUEST;
    }
}

static VOID
ExnDispatchBody (
    PVOID Context
    )
{
    PEXN_IRP_CONTEXT Ctx = (PEXN_IRP_CONTEXT)Context;
    Ctx->DispatchStatus = ExnCommonDispatch(Ctx);
}

static NTSTATUS
ExnGuardedDispatch (
    PEXN_IRP_CONTEXT Ctx
    )
{
    NTSTATUS Code;

    /*
     * The handlers turn the Cache Manager's and Memory Manager's exceptions
     * into status codes where they call them; this catches the rest of the
     * expected ones (I/O errors raised somewhere below us). Anything else is
     * a bug and stops the system where it happened.
     */
    Ctx->DispatchStatus = STATUS_UNSUCCESSFUL;
    Code = ExnTryCall(ExnDispatchBody, Ctx, TRUE);
    return NT_SUCCESS(Code) ? Ctx->DispatchStatus : ExnExceptionStatus(Code);
}

static BOOLEAN
ExnCanRetry (
    PEXN_IRP_CONTEXT Ctx
    )
{
    if ((Ctx->Flags & EXN_CTX_NO_COMPLETE) || (Ctx->Irp->Flags & IRP_PAGING_IO)) {
        return FALSE;
    }

    /* Every handler below checks the volume before changing anything */
    switch (Ctx->IrpSp->MajorFunction) {

    case IRP_MJ_CREATE:
    case IRP_MJ_READ:
    case IRP_MJ_WRITE:
    case IRP_MJ_QUERY_INFORMATION:
    case IRP_MJ_SET_INFORMATION:
    case IRP_MJ_QUERY_VOLUME_INFORMATION:
    case IRP_MJ_SET_VOLUME_INFORMATION:
    case IRP_MJ_DIRECTORY_CONTROL:
    case IRP_MJ_FLUSH_BUFFERS:
    case IRP_MJ_QUERY_EA:
    case IRP_MJ_SET_EA:
        return TRUE;

    case IRP_MJ_FILE_SYSTEM_CONTROL:
        return (BOOLEAN)(Ctx->IrpSp->MinorFunction == IRP_MN_USER_FS_REQUEST);
    }

    return FALSE;
}

/*
 * The device reported a media change. Let the I/O manager run a verify
 * (IRP_MN_VERIFY_VOLUME comes back to us) so the request can be retried.
 */
static NTSTATUS
ExnPerformVerify (
    PEXN_IRP_CONTEXT Ctx
    )
{
    PDEVICE_OBJECT Device;
    PFILE_OBJECT FileObject;
    NTSTATUS Status;

    Device = IoGetDeviceToVerify(PsGetCurrentThread());
    IoSetDeviceToVerify(PsGetCurrentThread(), NULL);

    if (Device == NULL) {

        if (Ctx->Vcb == NULL) {
            return STATUS_VERIFY_REQUIRED;
        }

        Device = Ctx->Vcb->Vpb->RealDevice;
    }

    Status = IoVerifyVolume(Device, FALSE);

    /* A different volume is in the drive: have the create reparsed */
    FileObject = Ctx->IrpSp->FileObject;

    if (Ctx->IrpSp->MajorFunction == IRP_MJ_CREATE &&
        FileObject != NULL &&
        FileObject->RelatedFileObject == NULL &&
        (Status == STATUS_WRONG_VOLUME ||
         (Ctx->Vcb != NULL && (Ctx->Vcb->VcbState & VCB_STATE_DISMOUNTED)))) {

        Ctx->Irp->IoStatus.Information = IO_REMOUNT;
        return STATUS_REPARSE;
    }

    return Status;
}

NTSTATUS
NTAPI
ExnFsdDispatch (
    PDEVICE_OBJECT DeviceObject,
    PIRP Irp
    )
{
    EXN_IRP_CONTEXT Ctx;
    NTSTATUS Status;
    NTSTATUS VerifyStatus;
    BOOLEAN TopLevel = FALSE;
    ULONG Attempts = 0;


    FsRtlEnterFileSystem();

    if (IoGetTopLevelIrp() == NULL) {
        IoSetTopLevelIrp(Irp);
        TopLevel = TRUE;
    }

    RtlZeroMemory(&Ctx, sizeof(Ctx));
    Ctx.Irp = Irp;
    Ctx.IrpSp = IoGetCurrentIrpStackLocation(Irp);
    Ctx.DeviceObject = DeviceObject;

    if (DeviceObject != ExnData.FileSystemDeviceObject) {
        Ctx.Vcb = (PEXN_VCB)DeviceObject->DeviceExtension;
    }

    if (TopLevel) {
        Ctx.Flags |= EXN_CTX_TOP_LEVEL;
    }

#ifdef EXN_DBGPORT
    DBGP("[F t="); DBGX(KeGetCurrentThread()); DBGP(" mj="); DBGX(Ctx.IrpSp->MajorFunction); DBGP(" mn="); DBGX(Ctx.IrpSp->MinorFunction);
    if (Ctx.IrpSp->MajorFunction == IRP_MJ_CREATE && Ctx.IrpSp->FileObject) { DBGP(" "); DBGU(Ctx.IrpSp->FileObject->FileName); }
    if (Ctx.IrpSp->MajorFunction == IRP_MJ_READ || Ctx.IrpSp->MajorFunction == IRP_MJ_WRITE) {
        DBGP(" off="); DBGX(Ctx.IrpSp->Parameters.Read.ByteOffset.LowPart); DBGP(" len="); DBGX(Ctx.IrpSp->Parameters.Read.Length);
        DBGP(" fl="); DBGX(Irp->Flags); }
    DBGP("]\n");
#endif
    for (;;) {

        Status = ExnGuardedDispatch(&Ctx);

        if (Status != STATUS_VERIFY_REQUIRED || !TopLevel ||
            Attempts >= 2 || !ExnCanRetry(&Ctx)) {

            break;
        }

        Attempts++;

        VerifyStatus = ExnPerformVerify(&Ctx);

        if (VerifyStatus == STATUS_REPARSE || !NT_SUCCESS(VerifyStatus)) {
            Status = VerifyStatus;
            break;
        }

        Irp->IoStatus.Information = 0;
    }

#ifdef EXN_DBGPORT
    DBGP("[F< t="); DBGX(KeGetCurrentThread()); DBGP(" mj="); DBGX(Ctx.IrpSp->MajorFunction); DBGP(" st="); DBGX(Status); DBGP("]\n");
#endif
    if (!(Ctx.Flags & EXN_CTX_NO_COMPLETE)) {

        if (Status == STATUS_VERIFY_REQUIRED && Ctx.Vcb != NULL &&
            IoGetDeviceToVerify(PsGetCurrentThread()) == NULL) {

            IoSetHardErrorOrVerifyDevice(Irp, Ctx.Vcb->Vpb->RealDevice);
        }

        Irp->IoStatus.Status = Status;
        IoCompleteRequest(Irp, (CCHAR)(NT_SUCCESS(Status) ? IO_DISK_INCREMENT : IO_NO_INCREMENT));
    }

    if (TopLevel) {
        IoSetTopLevelIrp(NULL);
    }

    FsRtlExitFileSystem();

    return Status;
}

/*
 * Fails requests on a dismounted volume and asks for a verify when the
 * device has flagged a media change.
 */
NTSTATUS
ExnVerifyVcb (
    PEXN_IRP_CONTEXT Ctx,
    PEXN_VCB Vcb
    )
{
    PDEVICE_OBJECT RealDevice = Vcb->Vpb->RealDevice;
    PFILE_OBJECT FileObject = Ctx->IrpSp->FileObject;

    if (Vcb->VcbState & VCB_STATE_DISMOUNTED) {

        /*
         * Files were still open at the dismount, so the VPB still leads
         * here. A new open asks for a verify: it fails, the I/O manager
         * gives the device a fresh VPB and the open is reparsed onto a
         * new mount.
         */
        if (Ctx->IrpSp->MajorFunction == IRP_MJ_CREATE &&
            FileObject != NULL && FileObject->RelatedFileObject == NULL &&
            RealDevice->Vpb == Vcb->Vpb) {

            IoSetHardErrorOrVerifyDevice(Ctx->Irp, RealDevice);
            return STATUS_VERIFY_REQUIRED;
        }

        return EXN_STATUS_DISMOUNTED;
    }

    if ((RealDevice->Flags & DO_VERIFY_VOLUME) &&
        !(Ctx->IrpSp->Flags & SL_OVERRIDE_VERIFY_VOLUME) &&
        Vcb->VerifyThread != KeGetCurrentThread()) {

        IoSetHardErrorOrVerifyDevice(Ctx->Irp, RealDevice);
        return STATUS_VERIFY_REQUIRED;
    }

    return STATUS_SUCCESS;
}

/* ExnVerifyVcb, and the volume must accept changes */
NTSTATUS
ExnVerifyWritable (
    PEXN_IRP_CONTEXT Ctx,
    PEXN_VCB Vcb
    )
{
    NTSTATUS Status;

    Status = ExnVerifyVcb(Ctx, Vcb);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    if (Vcb->VcbState & VCB_STATE_READ_ONLY) {
        return STATUS_MEDIA_WRITE_PROTECTED;
    }

    /* back in use after a lock let go (not after shutdown: what is
       changed then goes straight to the disk) */
    if (!Vcb->Writing && !(Vcb->VcbState & (VCB_STATE_SHUTDOWN | VCB_STATE_LOCKED))) {
        (VOID)ExnStartWriting(Vcb);
        if (Vcb->VcbState & VCB_STATE_READ_ONLY) {
            return STATUS_MEDIA_WRITE_PROTECTED;
        }
    }

    return STATUS_SUCCESS;
}
