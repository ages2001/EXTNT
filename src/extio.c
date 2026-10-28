/*
 * EXTNT - disk I/O
 *
 * Synchronous requests to the target device, sector I/O for the metadata
 * cache and the journal, caller buffers, and the non-cached transfer of
 * file data: one request per contiguous run of the file's blocks; holes
 * read as zeros and get blocks when written.
 */

#include "ext.h"

/* ------------------------------------------------------------------ */
/* Synchronous requests to the target device                           */
/* ------------------------------------------------------------------ */

/*
 * Completion of our own requests: the waiter is woken and frees the IRP
 * itself. No APC to the thread is involved, so this works where the
 * caller runs with kernel APCs blocked (a paging read from a page fault on
 * NT 3.1 waits for metadata this way).
 */
static NTSTATUS
NTAPI
ExnIoDone (
    PDEVICE_OBJECT DeviceObject,
    PIRP Irp,
    PVOID Context
    )
{
    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Irp);

    KeSetEvent((PKEVENT)Context, IO_NO_INCREMENT, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

/* Reads or writes Length bytes of a nonpaged Buffer at byte Offset of Device */
NTSTATUS
ExnSyncIo (
    PDEVICE_OBJECT Device,
    UCHAR MajorFunction,
    LONGLONG Offset,
    ULONG Length,
    PVOID Buffer,
    BOOLEAN OverrideVerify
    )
{
    KEVENT Event;
    IO_STATUS_BLOCK Iosb;
    LARGE_INTEGER ByteOffset;
    PIRP Irp;
    PIO_STACK_LOCATION IrpSp;
    PMDL Mdl = NULL;
    NTSTATUS Status;

    KeInitializeEvent(&Event, NotificationEvent, FALSE);
    ByteOffset.QuadPart = Offset;

    Irp = IoAllocateIrp(Device->StackSize, FALSE);
    if (Irp == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    if (Length != 0) {
        Mdl = IoAllocateMdl(Buffer, Length, FALSE, FALSE, NULL);
        if (Mdl == NULL) {
            IoFreeIrp(Irp);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        MmBuildMdlForNonPagedPool(Mdl);
    }

    Irp->MdlAddress = Mdl;
    Irp->Flags = IRP_NOCACHE;
    Irp->RequestorMode = KernelMode;
    Irp->Tail.Overlay.Thread = PsGetCurrentThread();
    Irp->UserIosb = &Iosb;
    Irp->UserBuffer = Buffer;

    IrpSp = IoGetNextIrpStackLocation(Irp);
    IrpSp->MajorFunction = MajorFunction;
    IrpSp->Parameters.Read.Length = Length;
    IrpSp->Parameters.Read.ByteOffset = ByteOffset;
    if (OverrideVerify) {
        IrpSp->Flags |= SL_OVERRIDE_VERIFY_VOLUME;
    }

    IoSetCompletionRoutine(Irp, ExnIoDone, &Event, TRUE, TRUE, TRUE);

    Status = IoCallDriver(Device, Irp);
    if (Status == STATUS_PENDING) {
        KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
    }
    Status = Irp->IoStatus.Status;
    if (NT_SUCCESS(Status) && Length != 0 && Irp->IoStatus.Information != Length) {
        Status = STATUS_UNEXPECTED_IO_ERROR;
    }

    if (Mdl != NULL) {
        IoFreeMdl(Mdl);
    }
    Irp->MdlAddress = NULL;
    IoFreeIrp(Irp);
    return Status;
}

NTSTATUS
ExnDeviceIoctl (
    PDEVICE_OBJECT Device,
    ULONG IoControlCode,
    PVOID InputBuffer,
    ULONG InputLength,
    PVOID OutputBuffer,
    ULONG OutputLength,
    BOOLEAN OverrideVerify,
    PULONG Information
    )
{
    KEVENT Event;
    IO_STATUS_BLOCK Iosb;
    PIRP Irp;
    NTSTATUS Status;

    KeInitializeEvent(&Event, NotificationEvent, FALSE);

    Irp = IoBuildDeviceIoControlRequest(IoControlCode, Device, InputBuffer, InputLength,
                                        OutputBuffer, OutputLength, FALSE, &Event, &Iosb);
    if (Irp == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    if (OverrideVerify) {
        IoGetNextIrpStackLocation(Irp)->Flags |= SL_OVERRIDE_VERIFY_VOLUME;
    }

    Status = IoCallDriver(Device, Irp);

    if (Status == STATUS_PENDING) {
        KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
        Status = Iosb.Status;
    }
    if (Information != NULL) {
        *Information = (ULONG)Iosb.Information;
    }
    return Status;
}

NTSTATUS
ExnFlushDevice (
    PEXN_VCB Vcb
    )
{
    NTSTATUS Status = ExnSyncIo(Vcb->TargetDeviceObject, IRP_MJ_FLUSH_BUFFERS, 0, 0, NULL, TRUE);

    /* NT 3.x disk drivers may not know the request: nothing to wait for then */
    if (Status == STATUS_INVALID_DEVICE_REQUEST || Status == STATUS_NOT_SUPPORTED) {
        Status = STATUS_SUCCESS;
    }
    return Status;
}

/* ------------------------------------------------------------------ */
/* Sector I/O                                                          */
/* ------------------------------------------------------------------ */

#define EXN_MAX_SYNC_IO     0x10000     /* bytes per request */

static NTSTATUS
ExnSectorIo (
    PEXN_VCB Vcb,
    UCHAR Major,
    ULONGLONG Sector,
    ULONG Count,
    PUCHAR Buffer
    )
{
    NTSTATUS Status;
    BOOLEAN Override = (BOOLEAN)(Vcb->VerifyThread == KeGetCurrentThread());

#ifdef EXN_TRACEIO
    DBGP("[sio "); DBGX(Major); DBGP(" "); DBGX((ULONG)Sector); DBGP(" "); DBGX(Count); DBGP("]\n");
#endif
    while (Count != 0) {
        ULONG Run = Count > EXN_MAX_SYNC_IO / EXN_SECTOR_SIZE ? EXN_MAX_SYNC_IO / EXN_SECTOR_SIZE : Count;
        Status = ExnSyncIo(Vcb->TargetDeviceObject, Major, (LONGLONG)(Sector << EXN_SECTOR_SHIFT),
                           Run << EXN_SECTOR_SHIFT, Buffer, Override);
        if (!NT_SUCCESS(Status)) {
            return Status;
        }
        Sector += Run;
        Count -= Run;
        Buffer += Run << EXN_SECTOR_SHIFT;
    }
    return STATUS_SUCCESS;
}

NTSTATUS
ExnReadDisk (
    PEXN_VCB Vcb,
    ULONGLONG Sector,
    ULONG Count,
    PVOID Buffer
    )
{
    return ExnSectorIo(Vcb, IRP_MJ_READ, Sector, Count, (PUCHAR)Buffer);
}

NTSTATUS
ExnWriteDisk (
    PEXN_VCB Vcb,
    ULONGLONG Sector,
    ULONG Count,
    const VOID *Buffer
    )
{
    if (Vcb->VcbState & VCB_STATE_READ_ONLY) {
        return STATUS_MEDIA_WRITE_PROTECTED;
    }
    return ExnSectorIo(Vcb, IRP_MJ_WRITE, Sector, Count, (PUCHAR)Buffer);
}

/* ------------------------------------------------------------------ */
/* Caller buffers                                                      */
/* ------------------------------------------------------------------ */

typedef struct _EXN_PROBE {
    PMDL        Mdl;
    KPROCESSOR_MODE Mode;
    LOCK_OPERATION Operation;
} EXN_PROBE;

static VOID
ExnProbeBody (
    PVOID Context
    )
{
    EXN_PROBE *P = (EXN_PROBE *)Context;
    MmProbeAndLockPages(P->Mdl, P->Mode, P->Operation);
}

NTSTATUS
ExnLockUserBuffer (
    PIRP Irp,
    LOCK_OPERATION Operation,
    ULONG Length
    )
{
    PMDL Mdl;
    EXN_PROBE Probe;
    NTSTATUS Status = STATUS_SUCCESS;

    if (Irp->MdlAddress != NULL) {
        return STATUS_SUCCESS;
    }

    Mdl = IoAllocateMdl(Irp->UserBuffer, Length, FALSE, FALSE, Irp);
    if (Mdl == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Probe.Mdl = Mdl;
    Probe.Mode = Irp->RequestorMode;
    Probe.Operation = Operation;
    if (!NT_SUCCESS(ExnTryCall(ExnProbeBody, &Probe, FALSE))) {
        Status = STATUS_INVALID_USER_BUFFER;
    }

    if (!NT_SUCCESS(Status)) {
        IoFreeMdl(Mdl);
        Irp->MdlAddress = NULL;
    }
    return Status;
}

PVOID
ExnMapUserBuffer (
    PIRP Irp
    )
{
    if (Irp->MdlAddress == NULL) {
        return Irp->UserBuffer;
    }
    return ExnMdlAddress(Irp->MdlAddress);
}

/* ------------------------------------------------------------------ */
/* File offsets to disk offsets                                        */
/* ------------------------------------------------------------------ */

#define EXN_MAX_CONTIG      0x40000000

/* A paging file: binary search of its nonpaged run list */
static NTSTATUS
ExnRunsLookup (
    PEXN_VCB Vcb,
    PEXN_FCB Fcb,
    ULONGLONG Blk,
    PULONGLONG Disk,
    PULONGLONG Run
    )
{
    ULONG Lo = 0, Hi = Fcb->RunCount, Mid;
    PEXN_RUN R;

    UNREFERENCED_PARAMETER(Vcb);
    if (Hi == 0) {
        return STATUS_FILE_CORRUPT_ERROR;
    }
    while (Lo + 1 < Hi) {
        Mid = (Lo + Hi) / 2;
        if (Fcb->Runs[Mid].FileBlock <= Blk) Lo = Mid; else Hi = Mid;
    }
    R = &Fcb->Runs[Lo];
    if (Blk < R->FileBlock || Blk - R->FileBlock >= R->Length) {
        return STATUS_FILE_CORRUPT_ERROR;
    }
    *Disk = R->DiskBlock + (Blk - R->FileBlock);
    *Run = R->Length - (Blk - R->FileBlock);
    return STATUS_SUCCESS;
}

/*
 * Maps byte Vbo of a file to its volume byte offset and the bytes that
 * follow in the same state. A hole gives *Lbo = -1 (reads zeros), unless
 * Allocate, which gives it a block. The caller holds the file's paging
 * resource (or more).
 */
NTSTATUS
ExnVboToLbo (
    PEXN_VCB Vcb,
    PEXN_FCB Fcb,
    LONGLONG Vbo,
    BOOLEAN Allocate,
    PLONGLONG Lbo,
    PULONG Contiguous
    )
{
    ULONG Shift = Vcb->Vol.BlockShift;
    ULONGLONG Blk, Disk = 0, Run = 0;
    ULONG Within;
    LONGLONG Left;
    NTSTATUS Status;

    if (Vbo < 0) {
        return STATUS_INVALID_PARAMETER;
    }

    if (ExnIsVfcb(Fcb)) {
        Left = Fcb->Header.AllocationSize.QuadPart - Vbo;
        if (Left <= 0) {
            return STATUS_END_OF_FILE;
        }
        *Lbo = Vbo;
        *Contiguous = (Left > EXN_MAX_CONTIG) ? EXN_MAX_CONTIG : (ULONG)Left;
        return STATUS_SUCCESS;
    }

    Blk = (ULONGLONG)Vbo >> Shift;
    Within = (ULONG)(Vbo & (Vcb->Vol.BlockSize - 1));
#ifdef EXN_TRACEIO
    DBGP("[v2l "); DBGX(Fcb->Ino); DBGP(" "); DBGX((ULONG)Blk); DBGP("]\n");
#endif

    if (Fcb->Runs != NULL) {
        Status = ExnRunsLookup(Vcb, Fcb, Blk, &Disk, &Run);
        if (!NT_SUCCESS(Status)) {
            return Status;
        }
    } else {
        /* the one-entry map cache lives under the engine lock, like the tree */
        ExnEngineEnter(Vcb);
        if (Fcb->MapRun != 0 && Blk >= Fcb->MapFileBlock && Blk - Fcb->MapFileBlock < Fcb->MapRun &&
            !(Allocate && Fcb->MapHole)) {
            Run = Fcb->MapRun - (Blk - Fcb->MapFileBlock);
            Disk = Fcb->MapHole ? 0 : Fcb->MapDiskBlock + (Blk - Fcb->MapFileBlock);
            Status = STATUS_SUCCESS;
        } else {
            ULONG R32 = 0;
            Status = ExtMapFileBlock(&Vcb->Vol, Fcb->Ino, &Fcb->Inode, Blk, &Disk, &R32);
            if (NT_SUCCESS(Status) && Disk == 0 && Allocate) {
                BOOLEAN Fresh;
                Status = ExtGetFileBlock(&Vcb->Vol, Fcb->Ino, &Fcb->Inode, Blk, &Disk, &Fresh);
                if (NT_SUCCESS(Status)) {
                    Status = ExtWriteInode(&Vcb->Vol, Fcb->Ino, &Fcb->Inode);
                }
                R32 = 1;
            }
            if (R32 == 0) R32 = 1;
            Run = R32;
            if (NT_SUCCESS(Status)) {
                Fcb->MapFileBlock = Blk;
                Fcb->MapDiskBlock = Disk;
                Fcb->MapRun = R32;
                Fcb->MapHole = (BOOLEAN)(Disk == 0);
            } else {
                Fcb->MapRun = 0;
            }
        }
        Status = ExnEngineLeave(Vcb, Status);
        if (!NT_SUCCESS(Status)) {
            return Status;
        }
    }

    Left = (LONGLONG)(Run << Shift) - Within;
    *Contiguous = (Left > EXN_MAX_CONTIG) ? EXN_MAX_CONTIG : (ULONG)Left;
    *Lbo = (Disk == 0) ? -1 : (LONGLONG)((Disk << Shift) + Within);
    return STATUS_SUCCESS;
}

/*
 * Gives every hole in [Start, End) of a file its blocks (user writes do
 * this before the data reaches the cache, so "disk full" goes back to the
 * writer instead of the lazy writer). New blocks bigger than a page are
 * zeroed on the disk, since the cache writes them only page by page.
 */
NTSTATUS
ExnAllocateRange (
    PEXN_VCB Vcb,
    PEXN_FCB Fcb,
    LONGLONG Start,
    LONGLONG End
    )
{
    ULONG Shift = Vcb->Vol.BlockShift;
    ULONGLONG Blk, Last;
    BOOLEAN Changed = FALSE;
    NTSTATUS Status = STATUS_SUCCESS;

    if (Fcb->Runs != NULL || End <= Start || ExnIsVfcb(Fcb)) {
        return STATUS_SUCCESS;
    }
    Blk = (ULONGLONG)Start >> Shift;
    Last = ((ULONGLONG)End + Vcb->Vol.BlockSize - 1) >> Shift;

    ExnEngineEnter(Vcb);
    if (Fcb->Inode.Flags & EXT_IF_INLINE_DATA) {
        Status = ExtUninline(&Vcb->Vol, Fcb->Ino, &Fcb->Inode);   /* written to: out of the inode */
        Changed = TRUE;
    }
    while (Blk < Last && NT_SUCCESS(Status)) {
        ULONGLONG Disk;
        ULONG Run;
        Status = ExtMapFileBlock(&Vcb->Vol, Fcb->Ino, &Fcb->Inode, Blk, &Disk, &Run);
        if (!NT_SUCCESS(Status)) break;
        if (Disk != 0) {
            Blk += Run ? Run : 1;
            continue;
        }
        if (Run == 0) Run = 1;
        while (Run-- && Blk < Last) {
            BOOLEAN Fresh;
            Status = ExtGetFileBlock(&Vcb->Vol, Fcb->Ino, &Fcb->Inode, Blk, &Disk, &Fresh);
            if (!NT_SUCCESS(Status)) break;
            Changed = TRUE;
            if (Fresh && Vcb->Vol.BlockSize > EXN_PAGE_SIZE) {
                PVOID Zero = ExAllocatePoolWithTag(NonPagedPool, Vcb->Vol.BlockSize, EXN_TAG_BUFFER);
                if (Zero == NULL) { Status = STATUS_INSUFFICIENT_RESOURCES; break; }
                RtlZeroMemory(Zero, Vcb->Vol.BlockSize);
                Status = ExnWriteDisk(Vcb, Disk * Vcb->Vol.SectorsPerBlock, Vcb->Vol.SectorsPerBlock, Zero);
                ExFreePool(Zero);
                if (!NT_SUCCESS(Status)) break;
            }
            Blk++;
        }
    }
    if (Changed) {
        NTSTATUS W = ExtWriteInode(&Vcb->Vol, Fcb->Ino, &Fcb->Inode);
        if (NT_SUCCESS(Status)) Status = W;
    }
    if (Changed) {
        ExnForgetMapping(Fcb);          /* under the engine lock, like its readers */
    }
    return ExnEngineLeave(Vcb, Status);
}

/* ------------------------------------------------------------------ */
/* Non-cached transfers                                                */
/* ------------------------------------------------------------------ */

static NTSTATUS
NTAPI
ExnSyncCompletion (
    PDEVICE_OBJECT DeviceObject,
    PIRP Irp,
    PVOID Context
    )
{
    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Irp);

    KeSetEvent((PKEVENT)Context, IO_NO_INCREMENT, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

/* Moves Length bytes between the disk at Lbo and the master MDL from BufferOffset */
static NTSTATUS
ExnTransferRun (
    PEXN_VCB Vcb,
    UCHAR MajorFunction,
    PIRP MasterIrp,
    ULONG BufferOffset,
    LONGLONG Lbo,
    ULONG Length
    )
{
    PMDL MasterMdl = MasterIrp->MdlAddress;
    PMDL Mdl = MasterMdl;
    PUCHAR Va;
    PIRP Irp;
    PIO_STACK_LOCATION IrpSp;
    KEVENT Event;
    NTSTATUS Status;

    Irp = IoAllocateIrp(Vcb->TargetDeviceObject->StackSize, FALSE);
    if (Irp == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    if (BufferOffset != 0 || Length != MmGetMdlByteCount(MasterMdl)) {

        Va = (PUCHAR)MmGetMdlVirtualAddress(MasterMdl) + BufferOffset;

        Mdl = IoAllocateMdl(Va, Length, FALSE, FALSE, NULL);
        if (Mdl == NULL) {
            IoFreeIrp(Irp);
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        IoBuildPartialMdl(MasterMdl, Mdl, Va, Length);
    }

    KeInitializeEvent(&Event, NotificationEvent, FALSE);

    Irp->MdlAddress = Mdl;
    Irp->Flags = IRP_NOCACHE | (MasterIrp->Flags & IRP_PAGING_IO);
    Irp->RequestorMode = KernelMode;
    Irp->Tail.Overlay.Thread = PsGetCurrentThread();
    Irp->UserIosb = NULL;

    IrpSp = IoGetNextIrpStackLocation(Irp);
    IrpSp->MajorFunction = MajorFunction;
    IrpSp->Parameters.Read.Length = Length;
    IrpSp->Parameters.Read.ByteOffset.QuadPart = Lbo;

    if (Vcb->VerifyThread == KeGetCurrentThread()) {
        IrpSp->Flags |= SL_OVERRIDE_VERIFY_VOLUME;
    }

    IoSetCompletionRoutine(Irp, ExnSyncCompletion, &Event, TRUE, TRUE, TRUE);

#ifdef EXN_TRACEIO
    DBGP("[run "); DBGX((ULONG)(Lbo >> 9)); DBGP(" "); DBGX(Length); DBGP("]\n");
#endif
    Status = IoCallDriver(Vcb->TargetDeviceObject, Irp);

    if (Status == STATUS_PENDING) {
        KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
    }

    Status = Irp->IoStatus.Status;

    if (NT_SUCCESS(Status) && Irp->IoStatus.Information != Length) {
        Status = STATUS_UNEXPECTED_IO_ERROR;
    }

    if (Mdl != MasterMdl) {
        IoFreeMdl(Mdl);
    }

    Irp->MdlAddress = NULL;
    IoFreeIrp(Irp);

    return Status;
}

/*
 * A non-cached write that covers only part of a block that is still a hole:
 * the block is allocated and zeroed first, so its other bytes read as zero.
 */
static NTSTATUS
ExnPrepareEdge (
    PEXN_VCB Vcb,
    PEXN_FCB Fcb,
    LONGLONG Vbo
    )
{
    LONGLONG Lbo;
    ULONG Contiguous;
    PVOID Zero;
    NTSTATUS Status;

    Status = ExnVboToLbo(Vcb, Fcb, Vbo, FALSE, &Lbo, &Contiguous);
    if (!NT_SUCCESS(Status) || Lbo != -1) {
        return Status;
    }
    Status = ExnVboToLbo(Vcb, Fcb, Vbo, TRUE, &Lbo, &Contiguous);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    Zero = ExAllocatePoolWithTag(NonPagedPool, Vcb->Vol.BlockSize, EXN_TAG_BUFFER);
    if (Zero == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(Zero, Vcb->Vol.BlockSize);
    Lbo &= ~(LONGLONG)(Vcb->Vol.BlockSize - 1);
    Status = ExnWriteDisk(Vcb, (ULONGLONG)Lbo >> EXN_SECTOR_SHIFT, Vcb->Vol.SectorsPerBlock, Zero);
    ExFreePool(Zero);
    return Status;
}

/*
 * Moves ByteCount bytes (whole sectors) between the disk and Irp->MdlAddress,
 * from StartingVbo of Fcb. Reads return zeros from ValidData on and in
 * holes; writes give holes their blocks.
 */
NTSTATUS
ExnNonCachedIo (
    PEXN_IRP_CONTEXT Ctx,
    PEXN_FCB Fcb,
    UCHAR MajorFunction,
    LONGLONG StartingVbo,
    ULONG ByteCount,
    LONGLONG ValidData
    )
{
    PEXN_VCB Vcb = Fcb->Vcb;
    PIRP Irp = Ctx->Irp;
    BOOLEAN Write = (BOOLEAN)(MajorFunction == IRP_MJ_WRITE);
    LONGLONG TransferEnd = StartingVbo + ByteCount;
    LONGLONG Vbo, Lbo;
    ULONG Contiguous, Offset, Length, ZeroOffset;
    ULONG BlockMask = Vcb->Vol.BlockSize - 1;
    PUCHAR Buffer = NULL;
    NTSTATUS Status = STATUS_SUCCESS;

    /* data kept in the inode (inline data, a fast symbolic link) */
    if (!ExnIsVfcb(Fcb) && ((Fcb->Inode.Flags & EXT_IF_INLINE_DATA) || (Fcb->FcbState & FCB_STATE_SYMLINK))) {
        if (!Write) {
            ULONG Got = 0;
            Buffer = (PUCHAR)ExnMdlAddress(Irp->MdlAddress);
            if (Buffer == NULL) {
                return STATUS_INSUFFICIENT_RESOURCES;
            }
            RtlZeroMemory(Buffer, ByteCount);
            ExnEngineEnter(Vcb);
            Status = ExtReadFile(&Vcb->Vol, Fcb->Ino, &Fcb->Inode, (ULONGLONG)StartingVbo,
                                 ByteCount, Buffer, &Got);
            return ExnEngineLeave(Vcb, Status);
        }
        if (Fcb->FcbState & FCB_STATE_SYMLINK) {
            return STATUS_ACCESS_DENIED;
        }
        ExnEngineEnter(Vcb);
        Status = ExtUninline(&Vcb->Vol, Fcb->Ino, &Fcb->Inode);
        ExnForgetMapping(Fcb);
        Status = ExnEngineLeave(Vcb, Status);
        if (!NT_SUCCESS(Status)) {
            return Status;
        }
    }

    if (!Write && TransferEnd > ValidData) {
        TransferEnd = ExnRoundUp(ValidData, EXN_SECTOR_SIZE);
        if (TransferEnd < StartingVbo) {
            TransferEnd = StartingVbo;
        }
        if (TransferEnd > StartingVbo + ByteCount) {
            TransferEnd = StartingVbo + ByteCount;
        }
    }

    if (Write && !ExnIsVfcb(Fcb) && Fcb->Runs == NULL) {
        if (StartingVbo & BlockMask) {
            Status = ExnPrepareEdge(Vcb, Fcb, StartingVbo);
        }
        if (NT_SUCCESS(Status) && (TransferEnd & BlockMask) &&
            ((TransferEnd - 1) & ~(LONGLONG)BlockMask) != (StartingVbo & ~(LONGLONG)BlockMask)) {
            Status = ExnPrepareEdge(Vcb, Fcb, TransferEnd - 1);
        } else if (NT_SUCCESS(Status) && (TransferEnd & BlockMask) && !(StartingVbo & BlockMask)) {
            Status = ExnPrepareEdge(Vcb, Fcb, StartingVbo);
        }
        if (!NT_SUCCESS(Status)) {
            return Status;
        }
    }

    Vbo = StartingVbo;
    Offset = 0;

    while (Vbo < TransferEnd) {

        Status = ExnVboToLbo(Vcb, Fcb, Vbo, Write, &Lbo, &Contiguous);
        if (!NT_SUCCESS(Status)) {
            EXN_DBG((EXN_PFX "No block for offset %I64X of inode %lu: %lX\n", Vbo, Fcb->Ino, Status));
            return Status;
        }

        Length = (ULONG)(TransferEnd - Vbo);
        if (Length > Contiguous) {
            Length = Contiguous;
        }
        if (Length > EXN_MAX_TRANSFER) {
            Length = EXN_MAX_TRANSFER;   /* never hand the disk stack an oversized IRP */
        }

        if (Lbo == -1) {
            /* a hole reads as zeros */
            if (Buffer == NULL) {
                Buffer = (PUCHAR)ExnMdlAddress(Irp->MdlAddress);
                if (Buffer == NULL) {
                    return STATUS_INSUFFICIENT_RESOURCES;
                }
            }
            RtlZeroMemory(Buffer + Offset, Length);
        } else {
            Status = ExnTransferRun(Vcb, MajorFunction, Irp, Offset, Lbo, Length);
            if (!NT_SUCCESS(Status)) {
                EXN_DBG((EXN_PFX "Disk transfer at %I64X failed %lX\n", Lbo, Status));
                return Status;
            }
        }

        Vbo += Length;
        Offset += Length;
    }

    if (!Write && StartingVbo + ByteCount > ValidData) {

        ZeroOffset = (ValidData > StartingVbo) ? (ULONG)(ValidData - StartingVbo) : 0;
        if (Buffer == NULL) {
            Buffer = (PUCHAR)ExnMdlAddress(Irp->MdlAddress);
            if (Buffer == NULL) {
                return STATUS_INSUFFICIENT_RESOURCES;
            }
        }
        RtlZeroMemory(Buffer + ZeroOffset, ByteCount - ZeroOffset);
    }

    return Status;
}

/*
 * Writes zeros over [Start, End) of Fcb on the disk where it has blocks
 * (holes are zero already); a partial first sector keeps its bytes before
 * Start. End is rounded up to a sector.
 */
NTSTATUS
ExnZeroDisk (
    PEXN_VCB Vcb,
    PEXN_FCB Fcb,
    LONGLONG Start,
    LONGLONG End
    )
{
    PUCHAR Zero;
    LONGLONG Vbo, Lbo;
    ULONG Contiguous, Length, Within;
    NTSTATUS Status = STATUS_SUCCESS;

    End = ExnRoundUp(End, EXN_SECTOR_SIZE);
    if (Start >= End) {
        return STATUS_SUCCESS;
    }

    Zero = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, EXN_ZERO_CHUNK, EXN_TAG_BUFFER);
    if (Zero == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Vbo = Start & ~(LONGLONG)(EXN_SECTOR_SIZE - 1);
    Within = (ULONG)(Start - Vbo);

    if (Within != 0) {
        Status = ExnVboToLbo(Vcb, Fcb, Vbo, FALSE, &Lbo, &Contiguous);
        if (!NT_SUCCESS(Status)) goto Done;
        if (Lbo != -1) {
            Status = ExnReadDisk(Vcb, (ULONGLONG)Lbo >> EXN_SECTOR_SHIFT, 1, Zero);
            if (!NT_SUCCESS(Status)) goto Done;
            RtlZeroMemory(Zero + Within, EXN_SECTOR_SIZE - Within);
            Status = ExnWriteDisk(Vcb, (ULONGLONG)Lbo >> EXN_SECTOR_SHIFT, 1, Zero);
            if (!NT_SUCCESS(Status)) goto Done;
        }
        Vbo += EXN_SECTOR_SIZE;
    }

    RtlZeroMemory(Zero, EXN_ZERO_CHUNK);

    while (Vbo < End) {
        Status = ExnVboToLbo(Vcb, Fcb, Vbo, FALSE, &Lbo, &Contiguous);
        if (!NT_SUCCESS(Status)) goto Done;
        Length = EXN_ZERO_CHUNK;
        if (Length > Contiguous) {
            Length = Contiguous;
        }
        if ((LONGLONG)Length > End - Vbo) {
            Length = (ULONG)(End - Vbo);
        }
        if (Lbo != -1) {
            Status = ExnWriteDisk(Vcb, (ULONGLONG)Lbo >> EXN_SECTOR_SHIFT,
                                  (Length + EXN_SECTOR_SIZE - 1) >> EXN_SECTOR_SHIFT, Zero);
            if (!NT_SUCCESS(Status)) goto Done;
        }
        Vbo += Length;
    }

Done:
    ExFreePool(Zero);
    return Status;
}
