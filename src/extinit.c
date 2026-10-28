/*
 * EXTNT - driver entry and settings
 *
 * Settings live in the service key (HKLM\System\CurrentControlSet\
 * Services\Extnt) and are read at every mount, so a change applies to
 * volumes mounted afterwards:
 *
 *   EnableWriteSupport (REG_DWORD)  1 or missing: read/write, 0: read-only
 *   HideDotFiles       (REG_DWORD)  1 or missing: ".name" is hidden
 *   FollowSymlinks     (REG_DWORD)  1 or missing: opens follow symbolic links
 *   CaseSensitive      (REG_DWORD)  0 or missing: names are found in any case
 *                                   (an exact match first), 1: exact only
 *   DefaultUid, DefaultGid (REG_DWORD)  owner of new files (0 = root)
 *   FileMode, DirMode  (REG_DWORD)  permissions of new files (0644) and
 *                                   directories (0755)
 */

#include "ext.h"

EXN_DATA ExnData;

/* extinst recognises this driver by this string */
#ifdef __GNUC__
__attribute__((used))
#endif
const char ExnSignature[] = "EXTNT file system driver";

#ifdef EXN_CROSS
PIRP ExnXTopLevelIrp;
#endif

/* Keeps a NUL-terminated copy of the service key path */
static VOID
ExnSaveRegistryPath (
    PUNICODE_STRING RegistryPath
    )
{
    USHORT Size;

    if (RegistryPath == NULL || RegistryPath->Length == 0) {
        return;
    }

    Size = (USHORT)(RegistryPath->Length + sizeof(WCHAR));
    ExnData.RegistryPath.Buffer = (PWCHAR)ExAllocatePoolWithTag(PagedPool, Size, EXN_TAG_REGISTRY);

    if (ExnData.RegistryPath.Buffer != NULL) {
        RtlCopyMemory(ExnData.RegistryPath.Buffer, RegistryPath->Buffer, RegistryPath->Length);
        ExnData.RegistryPath.Buffer[RegistryPath->Length / sizeof(WCHAR)] = 0;
        ExnData.RegistryPath.Length = RegistryPath->Length;
        ExnData.RegistryPath.MaximumLength = Size;
    }
}

static ULONG
ExnQueryDword (
    PCWSTR Name,
    ULONG Default
    )
{
    RTL_QUERY_REGISTRY_TABLE Table[2];
    ULONG Value = Default;

    if (ExnData.RegistryPath.Buffer == NULL) {
        return Default;
    }

    RtlZeroMemory(Table, sizeof(Table));
    Table[0].Flags = RTL_QUERY_REGISTRY_DIRECT;
    Table[0].Name = (PWSTR)Name;
    Table[0].EntryContext = &Value;
    Table[0].DefaultType = REG_DWORD;
    Table[0].DefaultData = &Default;
    Table[0].DefaultLength = sizeof(ULONG);

    if (!NT_SUCCESS(RtlQueryRegistryValues(RTL_REGISTRY_ABSOLUTE, ExnData.RegistryPath.Buffer,
                                           Table, NULL, NULL))) {
        return Default;
    }
    return Value;
}

/* Reads the settings; called at every mount */
static VOID
ExnLoadSettings (
    VOID
    )
{
    ExnData.Writable = (BOOLEAN)(ExnQueryDword(L"EnableWriteSupport", 1) != 0);
    ExnData.HideDotFiles = (BOOLEAN)(ExnQueryDword(L"HideDotFiles", 1) != 0);
    ExnData.FollowLinks = (BOOLEAN)(ExnQueryDword(L"FollowSymlinks", 1) != 0);
    ExnData.CaseSensitive = (BOOLEAN)(ExnQueryDword(L"CaseSensitive", 0) != 0);
    ExnData.DefaultUid = ExnQueryDword(L"DefaultUid", 0);
    ExnData.DefaultGid = ExnQueryDword(L"DefaultGid", 0);
    ExnData.FileMode = ExnQueryDword(L"FileMode", 0644) & 07777;
    ExnData.DirMode = ExnQueryDword(L"DirMode", 0755) & 07777;
}

/* Settings again (a mount is starting); FALSE: mount read-only */
BOOLEAN
ExnWriteSupportEnabled (
    VOID
    )
{
    ExnLoadSettings();
    return ExnData.Writable;
}

#ifdef EXN_SEHTEST
/* Self-test of ExnTryCall (debug builds): two raises, both must be caught */
static VOID ExnRaiseBody(PVOID Context) { ExRaiseStatus(*(PNTSTATUS)Context); }
static VOID ExnNestedBody(PVOID Context) { (VOID)ExnTryCall(ExnRaiseBody, Context, TRUE); ExRaiseStatus(STATUS_DISK_FULL); }
static VOID
ExnSehSelfTest (
    VOID
    )
{
    NTSTATUS Code = STATUS_INVALID_USER_BUFFER, R;
    R = ExnTryCall(ExnRaiseBody, &Code, FALSE);
    DBGP("[EXN sehtest 1 "); DBGX(R); DBGP("]\n");
    Code = STATUS_IN_PAGE_ERROR;
    R = ExnTryCall(ExnNestedBody, &Code, TRUE);
    DBGP("[EXN sehtest 2 "); DBGX(R); DBGP("]\n");
    R = ExnSafeCopy(&Code, (PVOID)(ULONG_PTR)16, sizeof(Code));
    DBGP("[EXN sehtest 3 "); DBGX(R); DBGP("]\n");
}
#endif

NTSTATUS
NTAPI
DriverEntry (
    PDRIVER_OBJECT DriverObject,
    PUNICODE_STRING RegistryPath
    )
{
    UNICODE_STRING Name;
    PDEVICE_OBJECT DeviceObject;
    NTSTATUS Status;
    ULONG i;

#ifdef EXN_NT31
    /* Bind the executive-resource API to the running kernel (Lite on NT 3.51+,
     * legacy on NT 3.1) before any resource is initialised. */
    if (!ExnResolveResourceApi()) {
        DBGP("[EXTNT:resource API not found]\n");
        return STATUS_PROCEDURE_NOT_FOUND;
    }
#endif

    RtlZeroMemory(&ExnData, sizeof(ExnData));

#ifdef EXN_SEHTEST
    ExnSehSelfTest();
#endif

    ExnSaveRegistryPath(RegistryPath);
    ExnLoadSettings();

    RtlInitUnicodeString(&Name, EXN_FS_DEVICE_NAME);

    Status = IoCreateDevice(DriverObject,
                            0,
                            &Name,
                            FILE_DEVICE_DISK_FILE_SYSTEM,
                            0,
                            FALSE,
                            &DeviceObject);

    if (!NT_SUCCESS(Status)) {
        EXN_DBG((EXN_PFX "IoCreateDevice failed %lX\n", Status));
        return Status;
    }

    ExnData.DriverObject = DriverObject;
    ExnData.FileSystemDeviceObject = DeviceObject;

    ExInitializeResourceLite(&ExnData.Resource);
    InitializeListHead(&ExnData.VcbList);

    /*
     * Wire the dispatch table. The DRIVER_OBJECT.MajorFunction[] array is
     * sized IRP_MJ_MAXIMUM_FUNCTION+1 by the running kernel, and that count
     * grew over time: NT 3.1 - 3.51 stop well below the value the modern
     * (WDK / mingw) headers define for IRP_MJ_MAXIMUM_FUNCTION (0x1b), so a
     * loop to the header's maximum writes past the end of the kernel's array
     * and corrupts the adjacent pool block. Only the low majors exist on NT
     * 3.1 anyway (no Power/WMI/PnP), so cap at the highest one EXTNT actually
     * serves - IRP_MJ_CLEANUP (0x12), which every NT since 3.1 provides.
     */
#ifdef EXN_NT31
    for (i = 0; i <= IRP_MJ_CLEANUP; i++) {
        DriverObject->MajorFunction[i] = ExnFsdDispatch;
    }
#else
    for (i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++) {
        DriverObject->MajorFunction[i] = ExnFsdDispatch;
    }
#endif

    ExnInitializeFastIo(&ExnData.FastIoDispatch);
    DriverObject->FastIoDispatch = &ExnData.FastIoDispatch;

    ExnInitializeCacheCallbacks();

#ifdef DO_DEVICE_INITIALIZING
    DeviceObject->Flags &= ~DO_DEVICE_INITIALIZING;
#endif

    /* Volumes are left clean at shutdown */
    (VOID)IoRegisterShutdownNotification(DeviceObject);

    IoRegisterFileSystem(DeviceObject);

    /* Linux partitions get no drive letter from Windows: give them one */
    ExnStartDriveLetters();

    EXN_DBG((EXN_PFX "Loaded\n"));

    return STATUS_SUCCESS;
}
