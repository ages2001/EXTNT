/*
 * EXTNT - IRP_MJ_QUERY_VOLUME_INFORMATION and IRP_MJ_SET_VOLUME_INFORMATION
 */

#include "ext.h"

static NTSTATUS
ExnCopyString (
    PWSTR Target,
    ULONG Room,
    const WCHAR *Source,
    ULONG Bytes,
    PULONG Copied
    )
{
    *Copied = (Bytes < Room) ? Bytes : Room;
    RtlCopyMemory(Target, Source, *Copied);

    return (*Copied < Bytes) ? STATUS_BUFFER_OVERFLOW : STATUS_SUCCESS;
}

/* Blocks: all, free, and free for ordinary users (less the root reserve) */
static VOID
ExnVolumeBlocks (
    PEXN_VCB Vcb,
    PULONGLONG Total,
    PULONGLONG Free,
    PULONGLONG Available
    )
{
    ULONGLONG Reserved;

    ExnEngineEnter(Vcb);
    *Total = Vcb->Vol.BlocksCount;
    *Free = Vcb->Vol.FreeBlocksCount;
    Reserved = Vcb->Vol.Super->RBlocksCountLo;
    if (Vcb->Vol.Has64Bit) {
        Reserved |= (ULONGLONG)Vcb->Vol.Super->RBlocksCountHi << 32;
    }
    ExnRelease(&Vcb->EngineResource);

    /* the reserve is root's: files EXTNT makes belong to root unless
       another owner is configured, and then it is out of reach */
    if (ExnData.DefaultUid == 0) {
        Reserved = 0;
    }
    *Available = (*Free > Reserved) ? *Free - Reserved : 0;
}

/* "EXT2", "EXT3" or "EXT4": what Linux would call the volume */
static const WCHAR *
ExnFsName (
    PEXN_VCB Vcb
    )
{
    if ((Vcb->Vol.FeatureIncompat & (EXT_FI_EXTENTS | EXT_FI_64BIT | EXT_FI_FLEX_BG | EXT_FI_INLINE_DATA |
                                     EXT_FI_EA_INODE | EXT_FI_LARGEDIR | EXT_FI_CSUM_SEED)) ||
        (Vcb->Vol.FeatureRoCompat & (EXT_RO_HUGE_FILE | EXT_RO_GDT_CSUM | EXT_RO_DIR_NLINK |
                                     EXT_RO_EXTRA_ISIZE | EXT_RO_METADATA_CSUM | EXT_RO_BIGALLOC))) {
        return L"EXT4";
    }
    return Vcb->Vol.HasJournal ? L"EXT3" : L"EXT2";
}

NTSTATUS
ExnCommonQueryVolumeInformation (
    PEXN_IRP_CONTEXT Ctx
    )
{
    PIRP Irp = Ctx->Irp;
    PIO_STACK_LOCATION IrpSp = Ctx->IrpSp;
    PEXN_VCB Vcb = Ctx->Vcb;
    ULONG Class = IrpSp->Parameters.QueryVolume.FsInformationClass;
    ULONG Length = IrpSp->Parameters.QueryVolume.Length;
    PVOID Buffer = Irp->AssociatedIrp.SystemBuffer;
    PFILE_FS_VOLUME_INFORMATION Volume;
    PFILE_FS_SIZE_INFORMATION Size;
    PFILE_FS_DEVICE_INFORMATION Device;
    PFILE_FS_ATTRIBUTE_INFORMATION Attribute;
    PEXN_FILE_FS_FULL_SIZE_INFORMATION FullSize;
    ULONG Base;
    ULONG Copied;
    ULONGLONG Total = 0, Free = 0, Available = 0;
    ULONG Used = 0;
    const WCHAR *Name;
    NTSTATUS Status = STATUS_SUCCESS;

    (VOID)ExAcquireResourceSharedLite(&Vcb->Resource, TRUE);

    __try {

        Status = ExnVerifyVcb(Ctx, Vcb);
        if (!NT_SUCCESS(Status)) {
            __leave;
        }

        switch (Class) {

        case FileFsVolumeInformation:
            Base = FIELD_OFFSET(FILE_FS_VOLUME_INFORMATION, VolumeLabel);
            if (Length < Base) {
                Status = STATUS_INFO_LENGTH_MISMATCH;
                break;
            }

            Volume = (PFILE_FS_VOLUME_INFORMATION)Buffer;
            RtlZeroMemory(Volume, Base);
            Volume->VolumeSerialNumber = Vcb->SerialNumber;
            Volume->VolumeLabelLength = Vcb->LabelLength * sizeof(WCHAR);
            Volume->SupportsObjects = FALSE;

            Status = ExnCopyString(Volume->VolumeLabel, Length - Base, Vcb->Label,
                                    Vcb->LabelLength * sizeof(WCHAR), &Copied);
            Used = Base + Copied;
            break;

        case FileFsSizeInformation:
            if (Length < sizeof(FILE_FS_SIZE_INFORMATION)) {
                Status = STATUS_INFO_LENGTH_MISMATCH;
                break;
            }
            ExnVolumeBlocks(Vcb, &Total, &Free, &Available);
            Size = (PFILE_FS_SIZE_INFORMATION)Buffer;
            Size->TotalAllocationUnits.QuadPart = (LONGLONG)Total;
            Size->AvailableAllocationUnits.QuadPart = (LONGLONG)Available;
            Size->SectorsPerAllocationUnit = Vcb->Vol.SectorsPerBlock;
            Size->BytesPerSector = EXN_SECTOR_SIZE;
            Used = sizeof(FILE_FS_SIZE_INFORMATION);
            break;

        case EXN_CLASS_FS_FULL_SIZE:
            if (Length < sizeof(EXN_FILE_FS_FULL_SIZE_INFORMATION)) {
                Status = STATUS_INFO_LENGTH_MISMATCH;
                break;
            }
            ExnVolumeBlocks(Vcb, &Total, &Free, &Available);
            FullSize = (PEXN_FILE_FS_FULL_SIZE_INFORMATION)Buffer;
            FullSize->TotalAllocationUnits.QuadPart = (LONGLONG)Total;
            FullSize->CallerAvailableAllocationUnits.QuadPart = (LONGLONG)Available;
            FullSize->ActualAvailableAllocationUnits.QuadPart = (LONGLONG)Free;
            FullSize->SectorsPerAllocationUnit = Vcb->Vol.SectorsPerBlock;
            FullSize->BytesPerSector = EXN_SECTOR_SIZE;
            Used = sizeof(EXN_FILE_FS_FULL_SIZE_INFORMATION);
            break;

        case FileFsDeviceInformation:
            if (Length < sizeof(FILE_FS_DEVICE_INFORMATION)) {
                Status = STATUS_INFO_LENGTH_MISMATCH;
                break;
            }
            Device = (PFILE_FS_DEVICE_INFORMATION)Buffer;
            Device->DeviceType = FILE_DEVICE_DISK;
            Device->Characteristics = Vcb->Vpb->RealDevice->Characteristics;
            Used = sizeof(FILE_FS_DEVICE_INFORMATION);
            break;

        case FileFsAttributeInformation:
            Base = FIELD_OFFSET(FILE_FS_ATTRIBUTE_INFORMATION, FileSystemName);
            if (Length < Base) {
                Status = STATUS_INFO_LENGTH_MISMATCH;
                break;
            }

            Attribute = (PFILE_FS_ATTRIBUTE_INFORMATION)Buffer;
            Attribute->FileSystemAttributes = FILE_CASE_SENSITIVE_SEARCH | FILE_CASE_PRESERVED_NAMES |
                                              FILE_UNICODE_ON_DISK;
            if (Vcb->VcbState & VCB_STATE_READ_ONLY) {
                Attribute->FileSystemAttributes |= FILE_READ_ONLY_VOLUME;
            }
            Attribute->MaximumComponentNameLength = EXN_MAX_NAME;
            Name = ExnFsName(Vcb);
            Attribute->FileSystemNameLength = 4 * sizeof(WCHAR);

            Status = ExnCopyString(Attribute->FileSystemName, Length - Base, Name,
                                    4 * sizeof(WCHAR), &Copied);
            Used = Base + Copied;
            break;

        default:
            Status = STATUS_INVALID_PARAMETER;
            break;
        }

        if (NT_SUCCESS(Status) || Status == STATUS_BUFFER_OVERFLOW) {
            Irp->IoStatus.Information = Used;
        }

    } __finally {

        ExnRelease(&Vcb->Resource);
    }

    return Status;
}

/* Some NT4 headers lack FILE_FS_LABEL_INFORMATION */
typedef struct _EXN_FS_LABEL {
    ULONG       VolumeLabelLength;
    WCHAR       VolumeLabel[1];
} EXN_FS_LABEL, *PEXN_FS_LABEL;

NTSTATUS
ExnCommonSetVolumeInformation (
    PEXN_IRP_CONTEXT Ctx
    )
{
    PIO_STACK_LOCATION IrpSp = Ctx->IrpSp;
    PEXN_VCB Vcb = Ctx->Vcb;
    PEXN_FS_LABEL Label = (PEXN_FS_LABEL)Ctx->Irp->AssociatedIrp.SystemBuffer;
    /* NT4 ntddk.h has no SetVolume member; QueryVolume has the same layout */
    ULONG Length = IrpSp->Parameters.QueryVolume.Length;
    UCHAR Bytes[EXN_MAX_NAME + 1];
    ULONG ByteLength = 0;
    UNICODE_STRING U;
    ULONG Count;
    ULONG i;
    NTSTATUS Status;

    if (IrpSp->Parameters.QueryVolume.FsInformationClass != FileFsLabelInformation) {
        return STATUS_INVALID_PARAMETER;
    }

    if (Length < FIELD_OFFSET(EXN_FS_LABEL, VolumeLabel) ||
        Label->VolumeLabelLength > Length - FIELD_OFFSET(EXN_FS_LABEL, VolumeLabel) ||
        (Label->VolumeLabelLength & 1) != 0 ||
        Label->VolumeLabelLength > EXN_MAX_LABEL * sizeof(WCHAR)) {

        return STATUS_INVALID_VOLUME_LABEL;
    }

    Count = Label->VolumeLabelLength / sizeof(WCHAR);

    for (i = 0; i < Count; i++) {
        if (Label->VolumeLabel[i] < 0x20) {
            return STATUS_INVALID_VOLUME_LABEL;
        }
    }

    /* s_volume_name: up to 16 bytes of UTF-8, as e2label takes it */
    U.Buffer = Label->VolumeLabel;
    U.Length = U.MaximumLength = (USHORT)(Count * sizeof(WCHAR));
    if (Count != 0 && (!NT_SUCCESS(ExnUnicodeToExt(&U, Bytes, &ByteLength)) || ByteLength > 16)) {
        return STATUS_INVALID_VOLUME_LABEL;
    }
    Bytes[ByteLength] = 0;

    (VOID)ExAcquireResourceExclusiveLite(&Vcb->Resource, TRUE);

    __try {

        Status = ExnVerifyWritable(Ctx, Vcb);
        if (!NT_SUCCESS(Status)) {
            __leave;
        }

        ExnEngineEnter(Vcb);
        Status = ExnEngineLeave(Vcb, ExtSetLabel(&Vcb->Vol, (const char *)Bytes));
        if (!NT_SUCCESS(Status)) {
            __leave;
        }

        ExnLoadLabel(Vcb);

    } __finally {

        ExnRelease(&Vcb->Resource);
    }

    return Status;
}
