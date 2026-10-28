/*
 * EXTNT - drive letters for ext partitions
 *
 * Windows gives drive letters only to partitions of the types it knows
 * (FAT, HPFS/NTFS...); a Linux partition (type 83h, or any other) gets
 * none, so nothing would ever open it and ask a file system to mount it.
 * A system thread looks at every disk a few times after start (disks of
 * Plug and Play controllers can arrive late), reads each partition's
 * superblock through the whole-disk device (so nothing gets mounted on the
 * way) and links a free letter to every ext partition that has none:
 * \DosDevices\X: -> \Device\HarddiskN\PartitionM. The letter is kept in the
 * service key ("Letter.HarddiskN.PartitionM" = "X") and the same one is
 * taken next time when free; it can be set there by hand too.
 *
 *   AssignDriveLetters (REG_DWORD)  1 or missing: on, 0: off
 */

#include "ext.h"

#define EXN_MAX_DISKS           32
#define EXN_LAYOUT_ENTRIES      128

static const char ExnHex[] = "0123456789";

/* "\Device\HarddiskN\PartitionM" */
static VOID
ExnPartitionName (
    ULONG Disk,
    ULONG Partition,
    PWCHAR Out
    )
{
    static const WCHAR Head[] = L"\\Device\\Harddisk";
    static const WCHAR Mid[] = L"\\Partition";
    ULONG n = 0, i;
    WCHAR Digits[12];
    ULONG d;

    for (i = 0; Head[i]; i++) Out[n++] = Head[i];
    d = 0;
    do { Digits[d++] = (WCHAR)ExnHex[Disk % 10]; Disk /= 10; } while (Disk);
    while (d) Out[n++] = Digits[--d];
    for (i = 0; Mid[i]; i++) Out[n++] = Mid[i];
    d = 0;
    do { Digits[d++] = (WCHAR)ExnHex[Partition % 10]; Partition /= 10; } while (Partition);
    while (d) Out[n++] = Digits[--d];
    Out[n] = 0;
}

/* Target of a symbolic link object, or FALSE */
static BOOLEAN
ExnLinkTarget (
    PCWSTR Name,
    PWCHAR Target,
    ULONG TargetChars
    )
{
    UNICODE_STRING N, T;
    OBJECT_ATTRIBUTES Oa;
    HANDLE H;
    NTSTATUS Status;

    RtlInitUnicodeString(&N, Name);
    InitializeObjectAttributes(&Oa, &N, OBJ_CASE_INSENSITIVE, NULL, NULL);
    Status = ZwOpenSymbolicLinkObject(&H, GENERIC_READ, &Oa);
    if (!NT_SUCCESS(Status)) {
        return FALSE;
    }
    T.Buffer = Target;
    T.Length = 0;
    T.MaximumLength = (USHORT)((TargetChars - 1) * sizeof(WCHAR));
    Status = ZwQuerySymbolicLinkObject(H, &T, NULL);
    ZwClose(H);
    if (!NT_SUCCESS(Status)) {
        return FALSE;
    }
    Target[T.Length / sizeof(WCHAR)] = 0;
    return TRUE;
}

static BOOLEAN
ExnSameName (
    PCWSTR A,
    PCWSTR B
    )
{
    UNICODE_STRING Ua, Ub;
    RtlInitUnicodeString(&Ua, A);
    RtlInitUnicodeString(&Ub, B);
    return RtlEqualUnicodeString(&Ua, &Ub, TRUE);
}

/* Bit n set: letter 'A'+n is taken; Owner gets the letter linked to Name/Alias, or 0 */
static ULONG
ExnUsedLetters (
    PCWSTR Name,
    PCWSTR Alias,
    PWCHAR Owner
    )
{
    WCHAR Link[] = L"\\DosDevices\\A:";
    WCHAR Target[260];
    ULONG Used = 0;
    ULONG i;

    *Owner = 0;
    for (i = 0; i < 26; i++) {
        Link[12] = (WCHAR)(L'A' + i);
        if (ExnLinkTarget(Link, Target, 260)) {
            Used |= 1UL << i;
            if (ExnSameName(Target, Name) || (Alias[0] != 0 && ExnSameName(Target, Alias))) {
                *Owner = (WCHAR)(L'A' + i);
            }
        }
    }
    return Used;
}

/* The value name the letter of a partition is kept under */
static VOID
ExnLetterValueName (
    ULONG Disk,
    ULONG Partition,
    PWCHAR Out
    )
{
    static const WCHAR A[] = L"Letter.Harddisk";
    static const WCHAR B[] = L".Partition";
    ULONG n = 0, i, d;
    WCHAR Digits[12];

    for (i = 0; A[i]; i++) Out[n++] = A[i];
    d = 0;
    do { Digits[d++] = (WCHAR)ExnHex[Disk % 10]; Disk /= 10; } while (Disk);
    while (d) Out[n++] = Digits[--d];
    for (i = 0; B[i]; i++) Out[n++] = B[i];
    d = 0;
    do { Digits[d++] = (WCHAR)ExnHex[Partition % 10]; Partition /= 10; } while (Partition);
    while (d) Out[n++] = Digits[--d];
    Out[n] = 0;
}

static WCHAR
ExnSavedLetter (
    PCWSTR ValueName
    )
{
    RTL_QUERY_REGISTRY_TABLE Table[2];
    WCHAR Buffer[8];
    UNICODE_STRING Value;

    if (ExnData.RegistryPath.Buffer == NULL) {
        return 0;
    }
    Buffer[0] = 0;
    Value.Buffer = Buffer;
    Value.Length = 0;
    Value.MaximumLength = sizeof(Buffer);
    RtlZeroMemory(Table, sizeof(Table));
    Table[0].Flags = RTL_QUERY_REGISTRY_DIRECT;
    Table[0].Name = (PWSTR)ValueName;
    Table[0].EntryContext = &Value;
    if (!NT_SUCCESS(RtlQueryRegistryValues(RTL_REGISTRY_ABSOLUTE, ExnData.RegistryPath.Buffer,
                                           Table, NULL, NULL)) || Value.Length == 0) {
        return 0;
    }
    if (Buffer[0] >= L'a' && Buffer[0] <= L'z') {
        return (WCHAR)(Buffer[0] - 32);
    }
    return (Buffer[0] >= L'C' && Buffer[0] <= L'Z') ? Buffer[0] : 0;
}

static BOOLEAN
ExnAutoLetters (
    VOID
    )
{
    RTL_QUERY_REGISTRY_TABLE Table[2];
    ULONG Value = 1, Default = 1;

    if (ExnData.RegistryPath.Buffer == NULL) {
        return TRUE;
    }
    RtlZeroMemory(Table, sizeof(Table));
    Table[0].Flags = RTL_QUERY_REGISTRY_DIRECT;
    Table[0].Name = L"AssignDriveLetters";
    Table[0].EntryContext = &Value;
    Table[0].DefaultType = REG_DWORD;
    Table[0].DefaultData = &Default;
    Table[0].DefaultLength = sizeof(ULONG);
    (VOID)RtlQueryRegistryValues(RTL_REGISTRY_ABSOLUTE, ExnData.RegistryPath.Buffer, Table, NULL, NULL);
    return (BOOLEAN)(Value != 0);
}

/* One partition: an ext superblock 1024 bytes in? */
static BOOLEAN
ExnPartitionIsExt (
    PDEVICE_OBJECT Disk,
    LONGLONG Start,
    PUCHAR Buffer
    )
{
    const EXT_SUPERBLOCK *Sb = (const EXT_SUPERBLOCK *)Buffer;

    if (!NT_SUCCESS(ExnSyncIo(Disk, IRP_MJ_READ, Start + 1024, 1024, Buffer, TRUE))) {
        return FALSE;
    }
    if (Sb->Magic == 0x137D) return TRUE;       /* original ext (read-only) */
    return (BOOLEAN)(ExtIsSuperblock(Buffer) && Sb->LogBlockSize <= 6 && Sb->BlocksPerGroup != 0 &&
                     Sb->InodesPerGroup != 0 && Sb->RevLevel <= 1);
}

static VOID
ExnScanDisk (
    ULONG DiskNumber,
    PDRIVE_LAYOUT_INFORMATION Layout,
    ULONG LayoutSize,
    PUCHAR Buffer
    )
{
    WCHAR Name[64];
    WCHAR Alias[260];
    WCHAR ValueName[64];
    WCHAR Link[] = L"\\DosDevices\\A:";
    UNICODE_STRING DiskName, LinkName, TargetName;
    PFILE_OBJECT FileObject;
    PDEVICE_OBJECT Device;
    WCHAR Owner, Letter;
    ULONG Used, i, Counted;
    NTSTATUS Status;

    ExnPartitionName(DiskNumber, 0, Name);
    RtlInitUnicodeString(&DiskName, Name);
    Status = IoGetDeviceObjectPointer(&DiskName, FILE_READ_ATTRIBUTES, &FileObject, &Device);
    if (!NT_SUCCESS(Status)) {
        return;
    }

    RtlZeroMemory(Layout, LayoutSize);
    Status = ExnDeviceIoctl(Device, IOCTL_DISK_GET_DRIVE_LAYOUT, NULL, 0, Layout, LayoutSize, TRUE, NULL);

    Counted = 0;
    for (i = 0; NT_SUCCESS(Status) && i < Layout->PartitionCount && i < EXN_LAYOUT_ENTRIES; i++) {

        PPARTITION_INFORMATION P = &Layout->PartitionEntry[i];
        ULONG Number;

        if (P->PartitionType == PARTITION_ENTRY_UNUSED || IsContainerPartition(P->PartitionType)) {
            continue;
        }
        /* NT 3.1 - 4.0 leave PartitionNumber 0 here: the disk driver numbers
           the used, non-container entries in this order */
        Number = P->PartitionNumber ? P->PartitionNumber : ++Counted;
        if (P->PartitionNumber) Counted = P->PartitionNumber;
        if (P->PartitionLength.QuadPart < 8192) {
            continue;
        }
        /* a type Windows mounts itself (an ext volume made bootable by
           extboot is 06h): it gets its letter from the system */
        if (IsRecognizedPartition(P->PartitionType)) {
            continue;
        }
        /* an ext volume on any type; a Linux partition (83h) even before it
           is formatted, so extfmt can reach it by its letter */
        if (P->PartitionType != 0x83 &&
            !ExnPartitionIsExt(Device, P->StartingOffset.QuadPart, Buffer)) {
            continue;
        }

        ExnPartitionName(DiskNumber, Number, Name);
        Alias[0] = 0;
        (VOID)ExnLinkTarget(Name, Alias, 260);      /* 2000/XP: \Device\HarddiskVolumeN */

        Used = ExnUsedLetters(Name, Alias, &Owner);
        if (Owner != 0) {
            continue;                               /* it has one */
        }

        ExnLetterValueName(DiskNumber, Number, ValueName);
        Letter = ExnSavedLetter(ValueName);
        if (Letter == 0 || (Used & (1UL << (Letter - L'A')))) {
            for (Letter = L'D'; Letter <= L'Z'; Letter++) {
                if (!(Used & (1UL << (Letter - L'A')))) {
                    break;
                }
            }
            if (Letter > L'Z') {
                break;
            }
        }

        Link[12] = Letter;
        RtlInitUnicodeString(&LinkName, Link);
        RtlInitUnicodeString(&TargetName, Name);
        Status = IoCreateSymbolicLink(&LinkName, &TargetName);
        if (NT_SUCCESS(Status)) {
            WCHAR Value[2];
            Value[0] = Letter;
            Value[1] = 0;
            EXN_DBG((EXN_PFX "%c: is ext (disk %lu partition %lu)\n", (char)Letter, DiskNumber, Number));
            if (ExnData.RegistryPath.Buffer != NULL) {
                (VOID)RtlWriteRegistryValue(RTL_REGISTRY_ABSOLUTE, ExnData.RegistryPath.Buffer,
                                            ValueName, REG_SZ, Value, sizeof(Value));
            }
        }
        Status = STATUS_SUCCESS;
    }

    ObDereferenceObject(FileObject);
}

static VOID
ExnAssignLetters (
    VOID
    )
{
    ULONG LayoutSize = sizeof(DRIVE_LAYOUT_INFORMATION) + EXN_LAYOUT_ENTRIES * sizeof(PARTITION_INFORMATION);
    PDRIVE_LAYOUT_INFORMATION Layout;
    PUCHAR Buffer;
    ULONG Disk;

    Layout = (PDRIVE_LAYOUT_INFORMATION)ExAllocatePoolWithTag(NonPagedPool, LayoutSize, EXN_TAG_BUFFER);
    Buffer = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, 1024, EXN_TAG_BUFFER);
    if (Layout != NULL && Buffer != NULL) {
        for (Disk = 0; Disk < EXN_MAX_DISKS; Disk++) {
            ExnScanDisk(Disk, Layout, LayoutSize, Buffer);
        }
    }
    if (Layout != NULL) ExFreePool(Layout);
    if (Buffer != NULL) ExFreePool(Buffer);
}

static VOID
NTAPI
ExnLetterThread (
    PVOID Context
    )
{
    static const LONG Delays[] = { 0, 3, 5, 10, 20, 30 };
    LARGE_INTEGER Due;
    ULONG i;

    UNREFERENCED_PARAMETER(Context);

    /* Loaded at boot (EXTNT starting Windows from an ext volume), the
       system has not given out its own letters yet: wait for them, or ours
       would take the ones it means for its volumes */
    for (i = 0; i < 90; i++) {
        WCHAR Owner;
        if (ExnUsedLetters(L"", L"", &Owner) & ~3UL) {
            break;
        }
        Due.QuadPart = -10000000;
        KeDelayExecutionThread(KernelMode, FALSE, &Due);
    }

    for (i = 0; i < sizeof(Delays) / sizeof(Delays[0]); i++) {
        if (Delays[i] != 0) {
            Due.QuadPart = -(LONGLONG)Delays[i] * 10000000;
            KeDelayExecutionThread(KernelMode, FALSE, &Due);
        }
        ExnAssignLetters();
    }
    PsTerminateSystemThread(STATUS_SUCCESS);
}

/* ------------------------------------------------------------------ */
/* The drive letter the registry already binds to this volume          */
/* ------------------------------------------------------------------ */
/*
 * On Windows 2000 and XP the system records each volume's drive letter in
 * HKLM\SYSTEM\MountedDevices as \DosDevices\X: = (disk signature + partition
 * byte offset). The I/O manager only replays this for partitions of a
 * recognized MBR type, so a Linux-type (83h) boot volume would get no
 * letter and the session manager dies (c000021a). EXTNT therefore claims the
 * registry's letter for the volume itself, at mount, before the session
 * manager runs - which lets the boot volume keep the honest 83h type.
 */

/* Links drive letter Letter to disk D partition P, if the letter is free */
static BOOLEAN
ExnMakeLetter (
    WCHAR Letter,
    ULONG Disk,
    ULONG Part
    )
{
    WCHAR Target[64], Cur[64];
    WCHAR Link[] = L"\\DosDevices\\A:";
    UNICODE_STRING Dos, Dev;

    if (Letter < L'A' || Letter > L'Z') {
        return FALSE;
    }
    Link[12] = Letter;
    Cur[0] = 0;
    if (ExnLinkTarget(Link, Cur, 64)) {
        return FALSE;           /* already assigned */
    }
    ExnPartitionName(Disk, Part, Target);
    RtlInitUnicodeString(&Dos, Link);
    RtlInitUnicodeString(&Dev, Target);
    return (BOOLEAN)NT_SUCCESS(IoCreateSymbolicLink(&Dos, &Dev));
}

VOID
ExnClaimRegistryLetter (
    PEXN_VCB Vcb,
    PDEVICE_OBJECT TargetDevice,
    LONGLONG StartingOffset
    )
{
    ULONG Disk = 0, Part = 0, Signature = 0, Used, i;
    BOOLEAN Active = FALSE, Found = FALSE;
    WCHAR Owner, Letter = 0;
    WCHAR Target[64];
    UNICODE_STRING KeyName, ValueName;
    OBJECT_ATTRIBUTES Oa;
    HANDLE Key;
    NTSTATUS Status;
    PKEY_VALUE_FULL_INFORMATION Info;
    ULONG InfoSize = 256;
    UCHAR Match[12];

    UNREFERENCED_PARAMETER(TargetDevice);

    if (!ExnAutoLetters()) {
        return;
    }

    /*
     * Find the disk, partition number, active flag and disk signature of the
     * partition at StartingOffset by scanning every disk's layout. This works
     * on every NT version (no storage IOCTL, no device-object matching), and
     * NT 3.1 - 4.0's PartitionNumber of 0 is replaced by counting as the disk
     * driver does.
     */
    {
        PDRIVE_LAYOUT_INFORMATION Layout;
        ULONG LayoutSize = sizeof(DRIVE_LAYOUT_INFORMATION) + EXN_LAYOUT_ENTRIES * sizeof(PARTITION_INFORMATION);
        ULONG d;
        Layout = (PDRIVE_LAYOUT_INFORMATION)ExAllocatePoolWithTag(NonPagedPool, LayoutSize, EXN_TAG_BUFFER);
        if (Layout == NULL) {
            return;
        }
        for (d = 0; d < 32 && !Found; d++) {
            WCHAR Name[64];
            UNICODE_STRING DiskName;
            PFILE_OBJECT FileObject;
            PDEVICE_OBJECT Device;
            ULONG Counted = 0;
            ExnPartitionName(d, 0, Name);
            RtlInitUnicodeString(&DiskName, Name);
            if (!NT_SUCCESS(IoGetDeviceObjectPointer(&DiskName, FILE_READ_ATTRIBUTES, &FileObject, &Device))) {
                if (d == 0) continue;
                break;
            }
            RtlZeroMemory(Layout, LayoutSize);
            Status = ExnDeviceIoctl(Device, IOCTL_DISK_GET_DRIVE_LAYOUT, NULL, 0, Layout, LayoutSize, TRUE, NULL);
            for (i = 0; NT_SUCCESS(Status) && i < Layout->PartitionCount && i < EXN_LAYOUT_ENTRIES; i++) {
                PPARTITION_INFORMATION P = &Layout->PartitionEntry[i];
                ULONG Number;
                if (P->PartitionType == PARTITION_ENTRY_UNUSED || IsContainerPartition(P->PartitionType)) {
                    continue;
                }
                Number = P->PartitionNumber ? P->PartitionNumber : ++Counted;
                if (P->PartitionNumber) Counted = P->PartitionNumber;
                if (P->StartingOffset.QuadPart == StartingOffset) {
                    Disk = d; Part = Number; Active = (BOOLEAN)(P->BootIndicator != 0);
                    Signature = Layout->Signature;
                    Found = TRUE;
                    break;
                }
            }
            ObDereferenceObject(FileObject);
        }
        ExFreePool(Layout);
    }
    if (!Found || Part == 0) {
        return;
    }

    /* the value data the registry would hold for this volume */
    RtlCopyMemory(Match, &Signature, 4);
    RtlCopyMemory(Match + 4, &StartingOffset, 8);

    RtlInitUnicodeString(&KeyName, L"\\Registry\\Machine\\SYSTEM\\MountedDevices");
    InitializeObjectAttributes(&Oa, &KeyName, OBJ_CASE_INSENSITIVE, NULL, NULL);
    if (!NT_SUCCESS(ZwOpenKey(&Key, KEY_READ, &Oa))) {
        /* NT 3.1 - 4.0: no MountedDevices. The boot volume is the active
           partition; give it C: so the session manager finds its paths. */
        if (Active && ExnMakeLetter(L'C', Disk, Part)) {
            EXN_DBG((EXN_PFX "C: for the boot volume (disk %lu partition %lu)\n", Disk, Part));
        }
        return;
    }

    Info = (PKEY_VALUE_FULL_INFORMATION)ExAllocatePoolWithTag(NonPagedPool, InfoSize, EXN_TAG_BUFFER);
    if (Info == NULL) {
        ZwClose(Key);
        return;
    }

    for (i = 0; Letter == 0; i++) {
        ULONG Got;
        PWCHAR Name;
        PUCHAR Data;
        Status = ZwEnumerateValueKey(Key, i, KeyValueFullInformation, Info, InfoSize, &Got);
        if (Status == STATUS_BUFFER_OVERFLOW || Status == STATUS_BUFFER_TOO_SMALL) {
            continue;       /* a long value we do not care about */
        }
        if (!NT_SUCCESS(Status)) {
            break;          /* no more values */
        }
        if (Info->DataLength != 12 || Info->Type != REG_BINARY) {
            continue;
        }
        Name = Info->Name;
        /* "\DosDevices\X:" : 13 chars, X at [12] */
        if (Info->NameLength != 13 * sizeof(WCHAR)) {
            continue;
        }
        if (Name[0] != L'\\' || Name[10] != L'\\' || Name[12] != L':') {
            continue;
        }
        Data = (PUCHAR)Info + Info->DataOffset;
        if (RtlCompareMemory(Data, Match, 12) == 12) {
            Letter = Name[11];
            if (Letter >= L'a' && Letter <= L'z') {
                Letter = (WCHAR)(Letter - L'a' + L'A');
            }
        }
    }
    ExFreePool(Info);
    ZwClose(Key);

    if (Letter == 0 && Active) {
        /* No entry matched (e.g. the disk signature changed since Windows
           recorded it): give the active boot partition C:. */
        Letter = L'C';
    }

    if (ExnMakeLetter(Letter, Disk, Part)) {
        EXN_DBG((EXN_PFX "%c: for the boot volume (disk %lu partition %lu)\n",
                 (char)Letter, Disk, Part));
    }
    (VOID)Used;
    (VOID)Owner;
    (VOID)Vcb;
    (VOID)Target;
}

VOID
ExnStartDriveLetters (
    VOID
    )
{
    HANDLE Thread;
    OBJECT_ATTRIBUTES Oa;

    if (!ExnAutoLetters()) {
        return;
    }
    InitializeObjectAttributes(&Oa, NULL, 0, NULL, NULL);    /* NT 3.1 - 4.0 know no OBJ_KERNEL_HANDLE */
    if (NT_SUCCESS(PsCreateSystemThread(&Thread, THREAD_ALL_ACCESS, &Oa, NULL, NULL,
                                        ExnLetterThread, NULL))) {
        ZwClose(Thread);
    }
}
