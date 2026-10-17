/*
 * EXTNT - IRP_MJ_FILE_SYSTEM_CONTROL (mount, verify, lock/unlock,
 * dismount, queries, EXTNT's own controls) and IRP_MJ_PNP
 */

#include "ext.h"
#include "extctl.h"

/* ------------------------------------------------------------------ */
/* Mount                                                               */
/* ------------------------------------------------------------------ */

/* Hot-plug devices (W2K) get the removable-media flush policy */
#define EXN_IOCTL_GET_HOTPLUG_INFO CTL_CODE(0x0000002d, 0x0305, METHOD_BUFFERED, FILE_ANY_ACCESS)

typedef struct _EXN_HOTPLUG_INFO {
    ULONG       Size;
    BOOLEAN     MediaRemovable;
    BOOLEAN     MediaHotplug;
    BOOLEAN     DeviceHotplug;
    BOOLEAN     WriteCacheEnableOverride;
} EXN_HOTPLUG_INFO;

static VOID
ExnCheckDeviceState (
    PEXN_VCB Vcb,
    PDEVICE_OBJECT TargetDevice
    )
{
    EXN_HOTPLUG_INFO Hotplug;
    NTSTATUS Status;

    Status = ExnDeviceIoctl(TargetDevice, IOCTL_DISK_IS_WRITABLE, NULL, 0, NULL, 0, TRUE, NULL);

    if (Status == STATUS_MEDIA_WRITE_PROTECTED || !ExnWriteSupportEnabled()) {
        Vcb->VcbState |= VCB_STATE_READ_ONLY;
    }

    if (Vcb->VcbState & VCB_STATE_REMOVABLE) {
        Vcb->VcbState |= VCB_STATE_FLUSH_ON_CLOSE;
    }

    RtlZeroMemory(&Hotplug, sizeof(Hotplug));

    Status = ExnDeviceIoctl(TargetDevice, EXN_IOCTL_GET_HOTPLUG_INFO, NULL, 0,
                             &Hotplug, sizeof(Hotplug), TRUE, NULL);

    if (NT_SUCCESS(Status) && (Hotplug.MediaRemovable || Hotplug.DeviceHotplug)) {
        Vcb->VcbState |= VCB_STATE_FLUSH_ON_CLOSE;
    }
}

/* The label as NT shows it: s_volume_name, UTF-8 */
VOID
ExnLoadLabel (
    PEXN_VCB Vcb
    )
{
    UNICODE_STRING U;
    ULONG Bytes = 0;
    ULONG i;
    PVPB Vpb = Vcb->Vpb;

    while (Bytes < 16 && Vcb->Vol.VolumeName[Bytes] != 0) {
        Bytes++;
    }

    Vcb->LabelLength = 0;
    if (Bytes != 0 && NT_SUCCESS(ExnExtToUnicode((const UCHAR *)Vcb->Vol.VolumeName, Bytes, &U))) {
        Vcb->LabelLength = U.Length / sizeof(WCHAR);
        if (Vcb->LabelLength > EXN_MAX_LABEL) {
            Vcb->LabelLength = EXN_MAX_LABEL;
        }
        RtlCopyMemory(Vcb->Label, U.Buffer, Vcb->LabelLength * sizeof(WCHAR));
        ExFreePool(U.Buffer);
    }

    Vpb->VolumeLabelLength = (USHORT)(Vcb->LabelLength * sizeof(WCHAR));
    for (i = 0; i < Vcb->LabelLength; i++) {
        Vpb->VolumeLabel[i] = Vcb->Label[i];
    }
}

/* Reads the superblock (byte 1024, 1024 bytes) into Buffer (two sectors) */
static BOOLEAN
ExnReadSuperblock (
    PDEVICE_OBJECT Device,
    PUCHAR Buffer
    )
{
    const EXT_SUPERBLOCK *Sb = (const EXT_SUPERBLOCK *)Buffer;

    if (!NT_SUCCESS(ExnReadSectors(Device, 1024, 1024, Buffer, TRUE))) {
        return FALSE;
    }
    if (!ExtIsSuperblock(Buffer)) {
        return FALSE;
    }
    if (Sb->Magic == 0x137D) {
        return TRUE;        /* the original 1992 ext (read-only): no groups */
    }
    /* sane enough to bother: block size 1K..64K, groups that hold something */
    return (BOOLEAN)(Sb->LogBlockSize <= 6 && Sb->BlocksPerGroup != 0 && Sb->InodesPerGroup != 0 &&
                     Sb->RevLevel <= 1);
}

/* Is the ext volume this VCB had (its UUID) still on the disk? */
BOOLEAN
ExnSameVolume (
    PEXN_VCB Vcb
    )
{
    PUCHAR Super = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, 1024, EXN_TAG_BUFFER);
    BOOLEAN Same = FALSE;

    if (Super == NULL) {
        return TRUE;            /* cannot tell: as before */
    }
    if (ExnReadSuperblock(Vcb->TargetDeviceObject, Super)) {
        if (((PEXT_SUPERBLOCK)Super)->Magic == 0x137D) {
            Same = Vcb->Vol.OldExt;         /* the 1992 ext has no UUID */
        } else {
            Same = (BOOLEAN)(!Vcb->Vol.OldExt &&
                             RtlCompareMemory(((PEXT_SUPERBLOCK)Super)->Uuid, Vcb->Vol.Uuid, 16) == 16);
        }
    }
    ExFreePool(Super);
    return Same;
}

/*
 * (Re)reads the engine's view of the volume: superblock, group descriptors;
 * replays the journal when Linux (or EXTNT) left one to replay.
 * Vcb->Resource exclusive; writing not started.
 */
NTSTATUS
ExnLoadVolume (
    PEXN_VCB Vcb
    )
{
    BOOLEAN ReadOnly = (BOOLEAN)((Vcb->VcbState & VCB_STATE_READ_ONLY) != 0);
    NTSTATUS Status;

    ExnEngineEnter(Vcb);

    ExnMetaPurge(Vcb);
    ExtUnmountFree(&Vcb->Vol);
    RtlZeroMemory(&Vcb->Vol, sizeof(EXT_VOL));
    Vcb->Vol.HostContext = Vcb;
    Vcb->Writing = FALSE;

    Status = ExtMount(&Vcb->Vol, ReadOnly);

    if (NT_SUCCESS(Status) && Vcb->PartitionBytes != 0 &&
        (Vcb->Vol.BlocksCount << Vcb->Vol.BlockShift) > (ULONGLONG)Vcb->PartitionBytes) {
        EXN_DBG((EXN_PFX "file system (%I64u blocks) larger than the partition\n",
                 Vcb->Vol.BlocksCount));
        Status = STATUS_DISK_CORRUPT_ERROR;
    }

    if (NT_SUCCESS(Status) && Vcb->Vol.NeedsRecovery) {
        if (!Vcb->Vol.ReadOnly) {
            EXN_DBG((EXN_PFX "replaying the journal\n"));
            Status = ExtJournalRecover(&Vcb->Vol);
            if (NT_SUCCESS(Status)) {
                Status = ExnCommit(Vcb);    /* no journal open: writes what is cached */
            }
        } else {
            /* cannot write the replay: what the journal holds is not seen */
            EXN_DBG((EXN_PFX "journal needs recovery, volume read-only: not replayed\n"));
        }
    }

    if (NT_SUCCESS(Status)) {
        if (Vcb->Vol.ReadOnly) {
            Vcb->VcbState |= VCB_STATE_READ_ONLY;
        }
        Vcb->VolumeBytes = (LONGLONG)(Vcb->Vol.BlocksCount << Vcb->Vol.BlockShift);
        if (Vcb->PartitionBytes < Vcb->VolumeBytes) {
            Vcb->PartitionBytes = Vcb->VolumeBytes;
        }
        Vcb->SerialNumber = Vcb->Vol.SerialNumber;

        /* made bootable by extboot? */
        Vcb->BootView = FALSE;
        {
            PUCHAR Sec = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, 512, EXN_TAG_BUFFER);
            if (Sec != NULL) {
                if (NT_SUCCESS(ExnReadDisk(Vcb, 0, 1, Sec)) && ExtShimPresent(Sec)) {
                    Vcb->BootView = TRUE;
                }
                ExFreePool(Sec);
            }
        }
    } else {
        ExtUnmountFree(&Vcb->Vol);
    }

    ExnMetaPurge(Vcb);
    ExnRelease(&Vcb->EngineResource);

    if (Status == EXT_CORRUPT || Status == EXT_NOT_SUPPORTED) {
        Status = STATUS_UNRECOGNIZED_VOLUME;
    }
    return Status;
}

/* After a mount or a reload: in use, and what a crashed Linux left on the orphan list */
static VOID
ExnBeginUse (
    PEXN_VCB Vcb
    )
{
    NTSTATUS Status;

    if (Vcb->VcbState & VCB_STATE_READ_ONLY) {
        return;
    }
    Status = ExnStartWriting(Vcb);
    if (NT_SUCCESS(Status) && Vcb->Writing) {
        ExnEngineEnter(Vcb);
        Status = ExnEngineLeave(Vcb, ExtProcessOrphans(&Vcb->Vol));
        if (!NT_SUCCESS(Status)) {
            EXN_DBG((EXN_PFX "orphan list: %lX\n", Status));
        }
    }
}

static NTSTATUS
ExnMountVolume (
    PEXN_IRP_CONTEXT Ctx
    )
{
    PIO_STACK_LOCATION IrpSp = Ctx->IrpSp;
    PDEVICE_OBJECT TargetDevice = IrpSp->Parameters.MountVolume.DeviceObject;
    PVPB Vpb = IrpSp->Parameters.MountVolume.Vpb;
    PDEVICE_OBJECT RealDevice = Vpb->RealDevice;
    DISK_GEOMETRY Geometry;
    PARTITION_INFORMATION Partition;
    PDEVICE_OBJECT VolumeDevice = NULL;
    PEXN_VCB Vcb = NULL;
    PUCHAR Super = NULL;
    LONGLONG PartitionBytes;
    BOOLEAN ClearedVerify = FALSE;
    BOOLEAN Resources = FALSE;
    NTSTATUS Status;

    EXN_DBG((EXN_PFX "mount offered: device %p\n", TargetDevice));
    Status = ExnDeviceIoctl(TargetDevice, IOCTL_DISK_GET_DRIVE_GEOMETRY, NULL, 0,
                             &Geometry, sizeof(Geometry), TRUE, NULL);
    if (!NT_SUCCESS(Status) || Geometry.BytesPerSector != EXN_SECTOR_SIZE) {
        return STATUS_UNRECOGNIZED_VOLUME;
    }

    RtlZeroMemory(&Partition, sizeof(Partition));
    Status = ExnDeviceIoctl(TargetDevice, IOCTL_DISK_GET_PARTITION_INFO, NULL, 0,
                             &Partition, sizeof(Partition), TRUE, NULL);
    PartitionBytes = NT_SUCCESS(Status) ? Partition.PartitionLength.QuadPart : 0;
    if (PartitionBytes == 0) {
        /* Floppies and superfloppies: the geometry gives the size */
        PartitionBytes = Geometry.Cylinders.QuadPart * Geometry.TracksPerCylinder *
                         Geometry.SectorsPerTrack * EXN_SECTOR_SIZE;
    }

    Super = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, 1024, EXN_TAG_BUFFER);
    if (Super == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    __try {

        /* Cheap test first: most volumes offered to us are not ext */
        if (!ExnReadSuperblock(TargetDevice, Super)) {
            Status = STATUS_UNRECOGNIZED_VOLUME;
            __leave;
        }

        Status = IoCreateDevice(ExnData.DriverObject,
                                sizeof(EXN_VCB),
                                NULL,
                                FILE_DEVICE_DISK_FILE_SYSTEM,
                                0,
                                FALSE,
                                &VolumeDevice);
        if (!NT_SUCCESS(Status)) {
            __leave;
        }

        VolumeDevice->StackSize = (CCHAR)(TargetDevice->StackSize + 1);

        if (TargetDevice->AlignmentRequirement > VolumeDevice->AlignmentRequirement) {
            VolumeDevice->AlignmentRequirement = TargetDevice->AlignmentRequirement;
        }

        VolumeDevice->SectorSize = EXN_SECTOR_SIZE;

#ifdef DO_DEVICE_INITIALIZING
        VolumeDevice->Flags &= ~DO_DEVICE_INITIALIZING;
#endif

        Vcb = (PEXN_VCB)VolumeDevice->DeviceExtension;
        RtlZeroMemory(Vcb, sizeof(EXN_VCB));

        Vcb->NodeTypeCode = EXN_NTC_VCB;
        Vcb->NodeByteSize = (CSHORT)sizeof(EXN_VCB);
        ExInitializeResourceLite(&Vcb->Resource);
        ExInitializeResourceLite(&Vcb->EngineResource);
        Resources = TRUE;
        InitializeListHead(&Vcb->FcbList);
        InitializeListHead(&Vcb->DirNotifyList);
        InitializeListHead(&Vcb->VcbLinks);
        FsRtlNotifyInitializeSync(&Vcb->NotifySync);
        ExnMetaInitialize(Vcb);

        Vcb->Vpb = Vpb;
        Vcb->TargetDeviceObject = TargetDevice;
        Vcb->VolumeDeviceObject = VolumeDevice;
        Vcb->VerifyThread = KeGetCurrentThread();
        Vcb->PartitionBytes = PartitionBytes;

        if (RealDevice->Characteristics & FILE_REMOVABLE_MEDIA) {
            Vcb->VcbState |= VCB_STATE_REMOVABLE;
        }

        ExnCheckDeviceState(Vcb, TargetDevice);

        if (RealDevice->Flags & DO_VERIFY_VOLUME) {
            RealDevice->Flags &= ~DO_VERIFY_VOLUME;
            ClearedVerify = TRUE;
        }

        (VOID)ExAcquireResourceExclusiveLite(&Vcb->Resource, TRUE);
        Status = ExnLoadVolume(Vcb);
        ExnRelease(&Vcb->Resource);
        if (!NT_SUCCESS(Status)) {
            EXN_DBG((EXN_PFX "mount failed %lX\n", Status));
            if (Status == STATUS_DISK_CORRUPT_ERROR || Status == STATUS_FILE_CORRUPT_ERROR) {
                Status = STATUS_UNRECOGNIZED_VOLUME;
            }
            __leave;
        }

        Vcb->RootDcb = ExnCreateRootDcb(Vcb);
        Vcb->VolumeFcb = ExnCreateVolumeFcb(Vcb);
        if (Vcb->RootDcb == NULL || Vcb->VolumeFcb == NULL) {
            Status = STATUS_INSUFFICIENT_RESOURCES;
            __leave;
        }

        ExnLoadLabel(Vcb);

        Vpb->SerialNumber = Vcb->SerialNumber;
        Vpb->DeviceObject = VolumeDevice;
        Vcb->VcbState |= VCB_STATE_MOUNTED;
        Vcb->VerifyThread = NULL;

        (VOID)ObReferenceObjectByPointer(TargetDevice, 0, NULL, KernelMode);

        (VOID)ExAcquireResourceExclusiveLite(&ExnData.Resource, TRUE);
        InsertTailList(&ExnData.VcbList, &Vcb->VcbLinks);
        ExnRelease(&ExnData.Resource);

        (VOID)ExAcquireResourceExclusiveLite(&Vcb->Resource, TRUE);
        ExnBeginUse(Vcb);
        ExnRelease(&Vcb->Resource);

        /*
         * Claim the drive letter the registry binds to this volume, now, before
         * the session manager runs: the boot volume needs its letter (usually
         * C:) even on an unrecognized (Linux-type 83h) partition, which the I/O
         * manager would otherwise leave unlettered. No-op on NT 3.1 - 4.0 (no
         * MountedDevices key) and when the letter is already assigned.
         */
        ExnClaimRegistryLetter(Vcb, TargetDevice, Partition.StartingOffset.QuadPart);

        EXN_DBG((EXN_PFX "Mounted ext%c %08lX: %I64u blocks of %lu%s%s\n",
                 (Vcb->Vol.FeatureIncompat & (EXT_FI_EXTENTS | EXT_FI_64BIT | EXT_FI_FLEX_BG)) ? '4' :
                 Vcb->Vol.HasJournal ? '3' : '2',
                 Vcb->SerialNumber, Vcb->Vol.BlocksCount, Vcb->Vol.BlockSize,
                 Vcb->Vol.Dirty ? ", not clean" : "",
                 (Vcb->VcbState & VCB_STATE_READ_ONLY) ? ", read-only" : ""));

    } __finally {

        if (Super != NULL) {
            ExFreePool(Super);
        }

        if (!NT_SUCCESS(Status)) {

            if (Vcb != NULL) {

                Vpb->DeviceObject = NULL;

                if (ClearedVerify) {
                    RealDevice->Flags |= DO_VERIFY_VOLUME;
                }

                while (!IsListEmpty(&Vcb->FcbList)) {
                    ExnDeleteFcb(CONTAINING_RECORD(Vcb->FcbList.Flink, EXN_FCB, FcbLinks));
                }
                if (Vcb->VolumeFcb != NULL) {
                    ExnDeleteFcb(Vcb->VolumeFcb);
                }

                ExnMetaPurge(Vcb);
                ExtUnmountFree(&Vcb->Vol);
                FsRtlNotifyUninitializeSync(&Vcb->NotifySync);
                if (Resources) {
                    ExDeleteResourceLite(&Vcb->EngineResource);
                    ExDeleteResourceLite(&Vcb->Resource);
                }
            }

            if (VolumeDevice != NULL) {
                IoDeleteDevice(VolumeDevice);
            }
        }
    }

    return Status;
}

/* ------------------------------------------------------------------ */
/* Verify                                                              */
/* ------------------------------------------------------------------ */

static NTSTATUS
ExnVerifyVolume (
    PEXN_IRP_CONTEXT Ctx
    )
{
    PIO_STACK_LOCATION IrpSp = Ctx->IrpSp;
    PVPB Vpb = IrpSp->Parameters.VerifyVolume.Vpb;
    PDEVICE_OBJECT VolumeDevice = IrpSp->Parameters.VerifyVolume.DeviceObject;
    PEXN_VCB Vcb;
    PUCHAR Super = NULL;
    BOOLEAN Delete = FALSE;
    NTSTATUS Status = STATUS_SUCCESS;

    if (VolumeDevice == NULL || VolumeDevice->DriverObject != ExnData.DriverObject ||
        VolumeDevice == ExnData.FileSystemDeviceObject) {

        return STATUS_WRONG_VOLUME;
    }

    Vcb = (PEXN_VCB)VolumeDevice->DeviceExtension;

    (VOID)ExAcquireResourceExclusiveLite(&Vcb->Resource, TRUE);

    __try {

        if (Vcb->VcbState & VCB_STATE_DISMOUNTED) {
            Status = STATUS_WRONG_VOLUME;
            __leave;
        }

        if (!(Vpb->RealDevice->Flags & DO_VERIFY_VOLUME)) {
            __leave;
        }

        Vcb->VerifyThread = KeGetCurrentThread();

        Super = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, 1024, EXN_TAG_BUFFER);
        if (Super == NULL) {
            Status = STATUS_INSUFFICIENT_RESOURCES;
            __leave;
        }

        if (ExnReadSuperblock(Vcb->TargetDeviceObject, Super) &&
            RtlCompareMemory(((PEXT_SUPERBLOCK)Super)->Uuid, Vcb->Vol.Uuid, 16) == 16) {

            /* Same volume; it may have been changed while away (another
               system wrote it): read it again */
            (VOID)ExnStopWriting(Vcb);
            ExnPurgeCachedFiles(Vcb);
            Status = ExnLoadVolume(Vcb);
            if (NT_SUCCESS(Status)) {
                ExnLoadLabel(Vcb);
                ExnBeginUse(Vcb);
            } else {
                Delete = ExnDismountVcb(Vcb);
                Status = STATUS_WRONG_VOLUME;
            }

        } else {

            EXN_DBG((EXN_PFX "Verify: volume %08lX is gone\n", Vcb->SerialNumber));

            Delete = ExnDismountVcb(Vcb);
            Status = STATUS_WRONG_VOLUME;
        }

        Vpb->RealDevice->Flags &= ~DO_VERIFY_VOLUME;

    } __finally {

        if (Super != NULL) {
            ExFreePool(Super);
        }

        Vcb->VerifyThread = NULL;
        ExnRelease(&Vcb->Resource);
    }

    if (Delete) {
        ExnDeleteVcb(Vcb);
        Ctx->Vcb = NULL;
    }

    return Status;
}

/* ------------------------------------------------------------------ */
/* User requests                                                       */
/* ------------------------------------------------------------------ */

static NTSTATUS
ExnGetVolumeOpen (
    PEXN_IRP_CONTEXT Ctx,
    PEXN_CCB *Ccb
    )
{
    PFILE_OBJECT FileObject = Ctx->IrpSp->FileObject;
    PEXN_FCB Fcb;

    if (FileObject == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    Fcb = (PEXN_FCB)FileObject->FsContext;
    *Ccb = (PEXN_CCB)FileObject->FsContext2;

    if (Fcb == NULL || *Ccb == NULL || !ExnIsVfcb(Fcb)) {
        return STATUS_INVALID_PARAMETER;
    }

    return STATUS_SUCCESS;
}

static VOID
ExnNotifyVolume (
    PFILE_OBJECT FileObject,
    ULONG Event
    )
{
#ifndef EXN_NT4
    (VOID)FsRtlNotifyVolumeEvent(FileObject, Event);
#else
    UNREFERENCED_PARAMETER(FileObject);
    UNREFERENCED_PARAMETER(Event);
#endif
}

#ifndef FSRTL_VOLUME_DISMOUNT
#define FSRTL_VOLUME_DISMOUNT       1
#endif
#ifndef FSRTL_VOLUME_LOCK
#define FSRTL_VOLUME_LOCK           3
#endif
#ifndef FSRTL_VOLUME_LOCK_FAILED
#define FSRTL_VOLUME_LOCK_FAILED    4
#endif
#ifndef FSRTL_VOLUME_UNLOCK
#define FSRTL_VOLUME_UNLOCK         5
#endif

static NTSTATUS
ExnLockVolume (
    PEXN_IRP_CONTEXT Ctx
    )
{
    PEXN_VCB Vcb = Ctx->Vcb;
    PFILE_OBJECT FileObject = Ctx->IrpSp->FileObject;
    PEXN_CCB Ccb;
    KIRQL Irql;
    NTSTATUS Status;

    Status = ExnGetVolumeOpen(Ctx, &Ccb);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    ExnNotifyVolume(FileObject, FSRTL_VOLUME_LOCK);

    (VOID)ExAcquireResourceExclusiveLite(&Vcb->Resource, TRUE);

    __try {

        if (Vcb->VcbState & (VCB_STATE_LOCKED | VCB_STATE_PNP_LOCKED)) {
            Status = STATUS_ACCESS_DENIED;
            __leave;
        }

        /* Whoever locks is about to work on the disk itself */
        if (!(Vcb->VcbState & VCB_STATE_DISMOUNTED)) {
            (VOID)ExnFlushVolume(Vcb, TRUE);
            ExnPurgeCachedFiles(Vcb);
        }

        /* Only the caller's handle may be open */
        if (Vcb->UncleanCount != 1) {
            Status = STATUS_ACCESS_DENIED;
            __leave;
        }

        Vcb->VcbState |= VCB_STATE_LOCKED;
        Vcb->LockFileObject = FileObject;

        IoAcquireVpbSpinLock(&Irql);
        Vcb->Vpb->Flags |= VPB_LOCKED;
        IoReleaseVpbSpinLock(Irql);

    } __finally {

        ExnRelease(&Vcb->Resource);
    }

    if (!NT_SUCCESS(Status)) {
        (VOID)ExAcquireResourceExclusiveLite(&Vcb->Resource, TRUE);
        ExnResumeWriting(Vcb);
        ExnRelease(&Vcb->Resource);
        ExnNotifyVolume(FileObject, FSRTL_VOLUME_LOCK_FAILED);
    }

    return Status;
}

static NTSTATUS
ExnUnlockVolume (
    PEXN_IRP_CONTEXT Ctx
    )
{
    PEXN_VCB Vcb = Ctx->Vcb;
    PFILE_OBJECT FileObject = Ctx->IrpSp->FileObject;
    PEXN_CCB Ccb;
    NTSTATUS Status;

    Status = ExnGetVolumeOpen(Ctx, &Ccb);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    (VOID)ExAcquireResourceExclusiveLite(&Vcb->Resource, TRUE);

    __try {

        if (Vcb->LockFileObject != FileObject) {

            Status = STATUS_NOT_LOCKED;

        } else {

            ExnUnlockVcb(Vcb);

            /* Metadata may have been rewritten under us */
            if (Vcb->VcbState & VCB_STATE_DASD_WRITTEN) {
                (VOID)ExnDismountVcb(Vcb);
            } else {
                ExnResumeWriting(Vcb);
            }
        }

    } __finally {

        ExnRelease(&Vcb->Resource);
    }

    if (NT_SUCCESS(Status)) {
        ExnNotifyVolume(FileObject, FSRTL_VOLUME_UNLOCK);
    }

    return Status;
}

static NTSTATUS
ExnDismountVolume (
    PEXN_IRP_CONTEXT Ctx
    )
{
    PEXN_VCB Vcb = Ctx->Vcb;
    PFILE_OBJECT FileObject = Ctx->IrpSp->FileObject;
    PEXN_CCB Ccb;
    NTSTATUS Status;

    Status = ExnGetVolumeOpen(Ctx, &Ccb);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    (VOID)ExAcquireResourceExclusiveLite(&Vcb->Resource, TRUE);

    /*
     * Programs running from the volume (the system volume while autochk
     * runs, say) cannot be dismounted from under: refuse, as a lock would be.
     */
    {
        PLIST_ENTRY Entry;
        for (Entry = Vcb->FcbList.Flink; Entry != &Vcb->FcbList; Entry = Entry->Flink) {
            PEXN_FCB Fcb = CONTAINING_RECORD(Entry, EXN_FCB, FcbLinks);
            if ((Fcb->FcbState & FCB_STATE_PAGING_FILE) ||
                (Fcb->SectionObjectPointers.ImageSectionObject != NULL &&
                 !MmFlushImageSection(&Fcb->SectionObjectPointers, MmFlushForWrite))) {
                ExnRelease(&Vcb->Resource);
                EXN_DBG((EXN_PFX "dismount refused: the volume is in use by running programs\n"));
                return STATUS_ACCESS_DENIED;
            }
        }
    }

    ExnNotifyVolume(FileObject, FSRTL_VOLUME_DISMOUNT);

    __try {

        if (!(Vcb->VcbState & VCB_STATE_DISMOUNTED)) {
            (VOID)ExnFlushVolume(Vcb, TRUE);
        }

        /* The caller's handle stays open, so the VCB cannot go away here */
        Ccb->Flags |= CCB_FLAG_DISMOUNTED_VOLUME;
        (VOID)ExnDismountVcb(Vcb);

    } __finally {

        ExnRelease(&Vcb->Resource);
    }

    return STATUS_SUCCESS;
}

static NTSTATUS
ExnIsVolumeDirty (
    PEXN_IRP_CONTEXT Ctx
    )
{
    PEXN_VCB Vcb = Ctx->Vcb;
    PIRP Irp = Ctx->Irp;
    PULONG Result = (PULONG)Irp->AssociatedIrp.SystemBuffer;

    if (ExnXSp(Ctx->IrpSp)->Parameters.FileSystemControl.OutputBufferLength < sizeof(ULONG)) {
        return STATUS_INVALID_PARAMETER;
    }

    /* Dirty for a check: not cleanly unmounted when we found it, or errors
       recorded (not merely in use) */
    *Result = (Vcb->Vol.Dirty || (Vcb->Vol.Super != NULL && (Vcb->Vol.Super->State & EXT_ERROR_FS))) ?
              VOLUME_IS_DIRTY : 0;
    Irp->IoStatus.Information = sizeof(ULONG);

    return STATUS_SUCCESS;
}

static NTSTATUS
ExnMarkVolumeDirtyRequest (
    PEXN_IRP_CONTEXT Ctx
    )
{
    PEXN_VCB Vcb = Ctx->Vcb;
    NTSTATUS Status;

    (VOID)ExAcquireResourceExclusiveLite(&Vcb->Resource, TRUE);

    __try {

        Status = ExnVerifyWritable(Ctx, Vcb);
        if (!NT_SUCCESS(Status)) {
            __leave;
        }

        /* e2fsck checks a volume that says it has errors */
        ExnEngineEnter(Vcb);
        Vcb->Vol.Super->State |= EXT_ERROR_FS;
        Vcb->Vol.Dirty = TRUE;
        Status = ExnEngineLeave(Vcb, ExtFlushSuper(&Vcb->Vol));

    } __finally {

        ExnRelease(&Vcb->Resource);
    }

    return Status;
}

/* EXTNT's own controls (extctl.h) */
static NTSTATUS
ExnPrivateRequest (
    PEXN_IRP_CONTEXT Ctx,
    ULONG Code
    )
{
    PEXN_VCB Vcb = Ctx->Vcb;
    PIRP Irp = Ctx->Irp;
    PFILE_OBJECT FileObject = Ctx->IrpSp->FileObject;
    PEXN_FCB Fcb = (FileObject != NULL) ? (PEXN_FCB)FileObject->FsContext : NULL;
    ULONG InLength = ExnXSp(Ctx->IrpSp)->Parameters.FileSystemControl.InputBufferLength;
    ULONG OutLength = ExnXSp(Ctx->IrpSp)->Parameters.FileSystemControl.OutputBufferLength;
    PVOID Buffer = Irp->AssociatedIrp.SystemBuffer;
    BOOLEAN FcbAcquired = FALSE;
    NTSTATUS Status;

    if (Fcb == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    (VOID)ExAcquireResourceExclusiveLite(&Vcb->Resource, TRUE);

    __try {

        Status = ExnVerifyVcb(Ctx, Vcb);
        if (!NT_SUCCESS(Status)) {
            __leave;
        }

        switch (Code) {

        case EXN_FSCTL_QUERY_VOLUME: {
            PEXN_VOLUME_INFO Info = (PEXN_VOLUME_INFO)Buffer;
            ULONG i;
            if (OutLength < sizeof(EXN_VOLUME_INFO)) {
                Status = STATUS_BUFFER_TOO_SMALL;
                __leave;
            }
            RtlZeroMemory(Info, sizeof(EXN_VOLUME_INFO));
            ExnEngineEnter(Vcb);
            Info->Version = 1;
            Info->BlockSize = Vcb->Vol.BlockSize;
            Info->BlocksCount = Vcb->Vol.BlocksCount;
            Info->FreeBlocks = Vcb->Vol.FreeBlocksCount;
            Info->InodesCount = Vcb->Vol.InodesCount;
            Info->FreeInodes = Vcb->Vol.FreeInodesCount;
            Info->InodeSize = Vcb->Vol.InodeSize;
            Info->GroupCount = Vcb->Vol.GroupCount;
            Info->FeatureCompat = Vcb->Vol.FeatureCompat;
            Info->FeatureIncompat = Vcb->Vol.FeatureIncompat;
            Info->FeatureRoCompat = Vcb->Vol.FeatureRoCompat;
            Info->ReadOnly = (Vcb->VcbState & VCB_STATE_READ_ONLY) ? 1 : 0;
            Info->ReadOnlyForced = Vcb->Vol.ReadOnlyForced ? 1 : 0;
            Info->HasJournal = Vcb->Vol.HasJournal ? 1 : 0;
            Info->Commits = Vcb->Commits;
            RtlCopyMemory(Info->Uuid, Vcb->Vol.Uuid, 16);
            for (i = 0; i < 16 && Vcb->Vol.VolumeName[i]; i++) {
                Info->Label[i] = Vcb->Vol.VolumeName[i];
            }
            ExnRelease(&Vcb->EngineResource);
            Irp->IoStatus.Information = sizeof(EXN_VOLUME_INFO);
            break;
        }

        case EXN_FSCTL_COMMIT:
            if (!(Vcb->VcbState & VCB_STATE_READ_ONLY)) {
                Status = ExnFlushVolume(Vcb, FALSE);
            }
            break;

        case EXN_FSCTL_GET_POSIX: {
            PEXN_POSIX_INFO Out = (PEXN_POSIX_INFO)Buffer;
            PEXT_INODE I;
            if (ExnIsVfcb(Fcb)) {
                Status = STATUS_INVALID_PARAMETER;
                __leave;
            }
            if (OutLength < sizeof(EXN_POSIX_INFO)) {
                Status = STATUS_BUFFER_TOO_SMALL;
                __leave;
            }
            (VOID)ExAcquireResourceSharedLite(&Fcb->Resource, TRUE);
            FcbAcquired = TRUE;
            I = &Fcb->Inode;
            RtlZeroMemory(Out, sizeof(EXN_POSIX_INFO));
            Out->Valid = EXN_POSIX_MODE | EXN_POSIX_UID | EXN_POSIX_GID;
            Out->Mode = I->Mode;
            Out->Uid = (ULONG)I->UidLo | ((ULONG)I->UidHi << 16);
            Out->Gid = (ULONG)I->GidLo | ((ULONG)I->GidHi << 16);
            Out->Links = I->LinksCount;
            Out->Inode = Fcb->Ino;
            Out->Flags = I->Flags;
            Out->Generation = I->Generation;
            Irp->IoStatus.Information = sizeof(EXN_POSIX_INFO);
            break;
        }

        case EXN_FSCTL_SET_POSIX: {
            PEXN_POSIX_INFO In = (PEXN_POSIX_INFO)Buffer;
            EXT_INODE Before;
            if (ExnIsVfcb(Fcb)) {
                Status = STATUS_INVALID_PARAMETER;
                __leave;
            }
            if (InLength < sizeof(EXN_POSIX_INFO)) {
                Status = STATUS_INVALID_PARAMETER;
                __leave;
            }
            Status = ExnVerifyWritable(Ctx, Vcb);
            if (!NT_SUCCESS(Status)) {
                __leave;
            }
            (VOID)ExAcquireResourceExclusiveLite(&Fcb->Resource, TRUE);
            FcbAcquired = TRUE;
            if (Fcb->FcbState & FCB_STATE_DELETED) {
                Status = STATUS_FILE_DELETED;
                __leave;
            }
            if (Fcb->FcbState & FCB_STATE_INODE_DIRTY) {
                Status = ExnUpdateInode(Vcb, Fcb);
                if (!NT_SUCCESS(Status)) {
                    __leave;
                }
            }
            ExnEngineEnter(Vcb);
            Before = Fcb->Inode;
            if (In->Valid & EXN_POSIX_MODE) {
                Fcb->Inode.Mode = (USHORT)((Fcb->Inode.Mode & EXT_S_IFMT) | (In->Mode & 07777));
            }
            if (In->Valid & EXN_POSIX_UID) {
                Fcb->Inode.UidLo = (USHORT)In->Uid;
                Fcb->Inode.UidHi = (USHORT)(In->Uid >> 16);
            }
            if (In->Valid & EXN_POSIX_GID) {
                Fcb->Inode.GidLo = (USHORT)In->Gid;
                Fcb->Inode.GidHi = (USHORT)(In->Gid >> 16);
            }
            Fcb->Inode.Ctime = ExtHostNow();
            Status = ExtWriteInode(&Vcb->Vol, Fcb->Ino, &Fcb->Inode);
            if (!NT_SUCCESS(Status)) {
                Fcb->Inode = Before;
            }
            Status = ExnEngineLeave(Vcb, Status);
            if (NT_SUCCESS(Status)) {
                KeQuerySystemTime(&Fcb->ChangeTime);
                Fcb->Attributes = ExnComputeAttributes(Vcb, &Fcb->Inode, Fcb->ExtName, Fcb->ExtNameLength,
                                                       Fcb->DosAttributes);
                ExnNotifyChange(Vcb, Fcb, FILE_NOTIFY_CHANGE_ATTRIBUTES | FILE_NOTIFY_CHANGE_SECURITY,
                                FILE_ACTION_MODIFIED);
            }
            break;
        }

        case EXN_FSCTL_READ_LINK: {
            ULONG Length = 0;
            PCHAR Text;
            if (ExnIsVfcb(Fcb) || (Fcb->Inode.Mode & EXT_S_IFMT) != EXT_S_IFLNK) {
                Status = STATUS_NOT_A_REPARSE_POINT;
                __leave;
            }
            Text = (PCHAR)ExAllocatePoolWithTag(PagedPool, Vcb->Vol.BlockSize + 1, EXN_TAG_BUFFER);
            if (Text == NULL) {
                Status = STATUS_INSUFFICIENT_RESOURCES;
                __leave;
            }
            (VOID)ExAcquireResourceSharedLite(&Fcb->Resource, TRUE);
            FcbAcquired = TRUE;
            ExnEngineEnter(Vcb);
            Status = ExnEngineLeave(Vcb, ExtReadLink(&Vcb->Vol, Fcb->Ino, &Fcb->Inode, Text,
                                                     Vcb->Vol.BlockSize + 1, &Length));
            if (NT_SUCCESS(Status)) {
                if (OutLength < Length) {
                    Status = STATUS_BUFFER_OVERFLOW;
                    Length = OutLength;
                }
                RtlCopyMemory(Buffer, Text, Length);
                Irp->IoStatus.Information = Length;
            }
            ExFreePool(Text);
            break;
        }

        case EXN_FSCTL_CREATE_LINK: {
            PEXN_LINK_REQUEST Req = (PEXN_LINK_REQUEST)Buffer;
            UNICODE_STRING Name;
            UCHAR Bytes[EXN_MAX_NAME + 1];
            ULONG Length;
            PCHAR Target;
            ULONG NewIno;
            EXT_INODE NewInode;
            EXT_ENTRY *Found;

            if (!ExnIsDcb(Fcb)) {
                Status = STATUS_NOT_A_DIRECTORY;
                __leave;
            }
            if (InLength < FIELD_OFFSET(EXN_LINK_REQUEST, Name) ||
                (ULONG)FIELD_OFFSET(EXN_LINK_REQUEST, Name) + Req->NameLength + Req->TargetLength > InLength ||
                Req->NameLength == 0 || (Req->NameLength & 1) || Req->TargetLength == 0 ||
                Req->TargetLength >= Vcb->Vol.BlockSize) {
                Status = STATUS_INVALID_PARAMETER;
                __leave;
            }
            Status = ExnVerifyWritable(Ctx, Vcb);
            if (!NT_SUCCESS(Status)) {
                __leave;
            }
            Name.Buffer = Req->Name;
            Name.Length = Name.MaximumLength = Req->NameLength;
            if (!ExnIsValidUnicodeName(&Name) ||
                !NT_SUCCESS(ExnUnicodeToExt(&Name, Bytes, &Length)) || Length == 0) {
                Status = STATUS_OBJECT_NAME_INVALID;
                __leave;
            }
            Bytes[Length] = 0;

            (VOID)ExAcquireResourceExclusiveLite(&Fcb->Resource, TRUE);
            FcbAcquired = TRUE;
            if (Fcb->FcbState & (FCB_STATE_DELETED | FCB_STATE_DELETE_PENDING)) {
                Status = STATUS_DELETE_PENDING;
                __leave;
            }

            Found = (EXT_ENTRY *)ExAllocatePoolWithTag(PagedPool, sizeof(EXT_ENTRY), EXN_TAG_BUFFER);
            if (Found == NULL) {
                Status = STATUS_INSUFFICIENT_RESOURCES;
                __leave;
            }
            Status = ExnLookupName(Vcb, Fcb, &Name, ExnData.CaseSensitive, Found);
            ExFreePool(Found);
            if (NT_SUCCESS(Status)) {
                Status = STATUS_OBJECT_NAME_COLLISION;
                __leave;
            }
            if (Status != STATUS_OBJECT_NAME_NOT_FOUND) {
                __leave;
            }

            Target = (PCHAR)ExAllocatePoolWithTag(PagedPool, Req->TargetLength + 1, EXN_TAG_BUFFER);
            if (Target == NULL) {
                Status = STATUS_INSUFFICIENT_RESOURCES;
                __leave;
            }
            RtlCopyMemory(Target, (PUCHAR)Req->Name + Req->NameLength, Req->TargetLength);
            Target[Req->TargetLength] = 0;

            ExnEngineEnter(Vcb);
            Status = ExtCreate(&Vcb->Vol, Fcb->Ino, &Fcb->Inode, (const char *)Bytes, Length,
                               (USHORT)(EXT_S_IFLNK | 0777), Target, &NewIno, &NewInode);
            if (NT_SUCCESS(Status) && (ExnData.DefaultUid || ExnData.DefaultGid)) {
                NewInode.UidLo = (USHORT)ExnData.DefaultUid;
                NewInode.UidHi = (USHORT)(ExnData.DefaultUid >> 16);
                NewInode.GidLo = (USHORT)ExnData.DefaultGid;
                NewInode.GidHi = (USHORT)(ExnData.DefaultGid >> 16);
                Status = ExtWriteInode(&Vcb->Vol, NewIno, &NewInode);
            }
            Status = ExnEngineLeave(Vcb, Status);
            ExFreePool(Target);

            if (NT_SUCCESS(Status)) {
                ExnIndexAdd(Fcb, Bytes, Length, NewIno);
                if (NT_SUCCESS(ExnBuildFullName(Fcb))) {
                    UNICODE_STRING Full;
                    USHORT Prefix = (USHORT)(Fcb->FullName.Length + (Fcb->ParentDcb != NULL ? sizeof(WCHAR) : 0));
                    Full.MaximumLength = (USHORT)(Prefix + Name.Length);
                    Full.Buffer = (PWSTR)ExAllocatePoolWithTag(PagedPool, Full.MaximumLength, EXN_TAG_NAME);
                    if (Full.Buffer != NULL) {
                        RtlCopyMemory(Full.Buffer, Fcb->FullName.Buffer, Fcb->FullName.Length);
                        if (Fcb->ParentDcb != NULL) {
                            Full.Buffer[Fcb->FullName.Length / sizeof(WCHAR)] = L'\\';
                        }
                        RtlCopyMemory((PUCHAR)Full.Buffer + Prefix, Name.Buffer, Name.Length);
                        Full.Length = Full.MaximumLength;
                        FsRtlNotifyFullReportChange(Vcb->NotifySync, &Vcb->DirNotifyList, (PSTRING)&Full,
                                                    Prefix, NULL, NULL, FILE_NOTIFY_CHANGE_FILE_NAME,
                                                    FILE_ACTION_ADDED, NULL);
                        ExFreePool(Full.Buffer);
                    }
                }
            }
            break;
        }

        default:
            Status = STATUS_INVALID_DEVICE_REQUEST;
            break;
        }

    } __finally {

        if (FcbAcquired) {
            ExnRelease(&Fcb->Resource);
        }
        ExnRelease(&Vcb->Resource);
    }

    return Status;
}

NTSTATUS
ExnUserFsRequest (
    PEXN_IRP_CONTEXT Ctx
    )
{
    ULONG Code = ExnXSp(Ctx->IrpSp)->Parameters.FileSystemControl.FsControlCode;

    switch (Code) {

    case FSCTL_LOCK_VOLUME:
        return ExnLockVolume(Ctx);

    case FSCTL_UNLOCK_VOLUME:
        return ExnUnlockVolume(Ctx);

    case FSCTL_DISMOUNT_VOLUME:
        return ExnDismountVolume(Ctx);

    case FSCTL_IS_VOLUME_MOUNTED:
        return ExnVerifyVcb(Ctx, Ctx->Vcb);

    case FSCTL_IS_PATHNAME_VALID:
        return STATUS_SUCCESS;

    case FSCTL_IS_VOLUME_DIRTY:
        return ExnIsVolumeDirty(Ctx);

    case FSCTL_MARK_VOLUME_DIRTY:
        return ExnMarkVolumeDirtyRequest(Ctx);

    case EXN_FSCTL_QUERY_VOLUME:
    case EXN_FSCTL_GET_POSIX:
    case EXN_FSCTL_SET_POSIX:
    case EXN_FSCTL_READ_LINK:
    case EXN_FSCTL_CREATE_LINK:
    case EXN_FSCTL_COMMIT:
        return ExnPrivateRequest(Ctx, Code);

    case FSCTL_REQUEST_OPLOCK_LEVEL_1:
    case FSCTL_REQUEST_OPLOCK_LEVEL_2:
    case FSCTL_REQUEST_BATCH_OPLOCK:
    case FSCTL_REQUEST_FILTER_OPLOCK:
        return STATUS_OPLOCK_NOT_GRANTED;

    case FSCTL_OPLOCK_BREAK_ACKNOWLEDGE:
    case FSCTL_OPBATCH_ACK_CLOSE_PENDING:
    case FSCTL_OPLOCK_BREAK_NOTIFY:
    case FSCTL_OPLOCK_BREAK_ACK_NO_2:
        return STATUS_INVALID_OPLOCK_PROTOCOL;
    }

    return STATUS_INVALID_DEVICE_REQUEST;
}

NTSTATUS
ExnCommonFileSystemControl (
    PEXN_IRP_CONTEXT Ctx
    )
{
    switch (Ctx->IrpSp->MinorFunction) {

    case IRP_MN_MOUNT_VOLUME:
        if (Ctx->Vcb != NULL) {
            return STATUS_INVALID_DEVICE_REQUEST;
        }
        return ExnMountVolume(Ctx);

    case IRP_MN_VERIFY_VOLUME:
        return ExnVerifyVolume(Ctx);

    case IRP_MN_USER_FS_REQUEST:
#ifdef IRP_MN_KERNEL_CALL
    case IRP_MN_KERNEL_CALL:
#endif
        if (Ctx->Vcb == NULL) {
            return STATUS_INVALID_DEVICE_REQUEST;
        }
        return ExnUserFsRequest(Ctx);
    }

    return STATUS_INVALID_DEVICE_REQUEST;
}

/* ------------------------------------------------------------------ */
/* Plug and Play (Windows 2000)                                        */
/* ------------------------------------------------------------------ */

#ifndef EXN_NT4

static NTSTATUS
NTAPI
ExnPnpCompletion (
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

/* Sends the IRP down and waits; the caller completes it */
static NTSTATUS
ExnForwardAndWait (
    PEXN_IRP_CONTEXT Ctx,
    PEXN_VCB Vcb
    )
{
    PIRP Irp = Ctx->Irp;
    KEVENT Event;

    KeInitializeEvent(&Event, NotificationEvent, FALSE);

    ExnCopyStackToNext(Irp);
    IoSetCompletionRoutine(Irp, ExnPnpCompletion, &Event, TRUE, TRUE, TRUE);

    if (IoCallDriver(Vcb->TargetDeviceObject, Irp) == STATUS_PENDING) {
        KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
    }

    return Irp->IoStatus.Status;
}

NTSTATUS
ExnCommonPnp (
    PEXN_IRP_CONTEXT Ctx
    )
{
    PEXN_VCB Vcb = Ctx->Vcb;
    PIRP Irp = Ctx->Irp;
    BOOLEAN Delete = FALSE;
    NTSTATUS Status;

    switch (Ctx->IrpSp->MinorFunction) {

    case IRP_MN_QUERY_REMOVE_DEVICE:

        (VOID)ExAcquireResourceExclusiveLite(&Vcb->Resource, TRUE);

        if (!(Vcb->VcbState & VCB_STATE_DISMOUNTED)) {
            (VOID)ExnFlushVolume(Vcb, TRUE);
            ExnPurgeCachedFiles(Vcb);
        }

        if (Vcb->UncleanCount != 0 || (Vcb->VcbState & VCB_STATE_LOCKED)) {
            ExnResumeWriting(Vcb);
            ExnRelease(&Vcb->Resource);
            return STATUS_ACCESS_DENIED;
        }

        Vcb->VcbState |= VCB_STATE_PNP_LOCKED;
        ExnRelease(&Vcb->Resource);

        Status = ExnForwardAndWait(Ctx, Vcb);

        (VOID)ExAcquireResourceExclusiveLite(&Vcb->Resource, TRUE);

        if (NT_SUCCESS(Status)) {
            Delete = ExnDismountVcb(Vcb);
        } else {
            Vcb->VcbState &= ~VCB_STATE_PNP_LOCKED;
            ExnResumeWriting(Vcb);
        }

        ExnRelease(&Vcb->Resource);
        break;

    case IRP_MN_REMOVE_DEVICE:
    case IRP_MN_SURPRISE_REMOVAL:

        Status = ExnForwardAndWait(Ctx, Vcb);

        (VOID)ExAcquireResourceExclusiveLite(&Vcb->Resource, TRUE);
        Vcb->VcbState &= ~VCB_STATE_PNP_LOCKED;
        Vcb->VcbState |= VCB_STATE_REMOVED;
        Delete = ExnDismountVcb(Vcb);
        ExnRelease(&Vcb->Resource);
        break;

    case IRP_MN_CANCEL_REMOVE_DEVICE:

        Status = ExnForwardAndWait(Ctx, Vcb);

        (VOID)ExAcquireResourceExclusiveLite(&Vcb->Resource, TRUE);
        Vcb->VcbState &= ~VCB_STATE_PNP_LOCKED;
        ExnResumeWriting(Vcb);
        ExnRelease(&Vcb->Resource);
        break;

    default:

        if (Vcb->VcbState & VCB_STATE_REMOVED) {
            return STATUS_NO_SUCH_DEVICE;
        }

        ExnSkipStack(Irp);
        Ctx->Flags |= EXN_CTX_NO_COMPLETE;
        return IoCallDriver(Vcb->TargetDeviceObject, Irp);
    }

    if (Delete) {
        ExnDeleteVcb(Vcb);
        Ctx->Vcb = NULL;
    }

    return Status;
}

#else

NTSTATUS
ExnCommonPnp (
    PEXN_IRP_CONTEXT Ctx
    )
{
    UNREFERENCED_PARAMETER(Ctx);
    return STATUS_INVALID_DEVICE_REQUEST;
}

#endif
