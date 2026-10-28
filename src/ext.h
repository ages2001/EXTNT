/*
 * EXTNT - ext2 / ext3 / ext4 file system driver for Windows NT 3.1, 3.5,
 * 3.51, 4.0, 2000, XP and XP x64.
 *
 *   EXN_NT4    Windows NT 3.1 - 4.0 builds
 *   EXN_CROSS  GCC cross build (implies EXN_NT31 and EXN_NT4): one binary
 *              for NT 3.1 - 4.0, also loaded by 2000 / XP
 *
 * The on-disk work is done by the shared engine (extsup.c, extmap.c,
 * extext.c, extdir.c, extfile.c, extea.c, extjbd.c, exthash.c), which
 * reaches the disk through the metadata cache in extmeta.c. File data is
 * cached by the Cache Manager in each file's own section and goes to the
 * disk non-cached, mapped through the file's extent tree / block map.
 */

#ifndef _EXT_H_
#define _EXT_H_

#ifndef EXT_KERNEL
#define EXT_KERNEL
#endif

#ifdef EXN_CROSS
#ifndef EXN_NT31
#define EXN_NT31
#endif
#ifndef EXN_NT4
#define EXN_NT4
#endif
#endif /* EXN_CROSS */

/*
 * GCC has no __try/__finally: the handlers' cleanup blocks become plain code
 * reached by goto (__leave). Exceptions are caught with ExnTryCall.
 */
#ifdef __GNUC__
#define __try
#define __finally               __exn_leave: __attribute__((unused));
#define __except(x)             if (0)
#define __leave                 goto __exn_leave
#ifndef AbnormalTermination
#define AbnormalTermination()   0
#endif
#ifndef GetExceptionInformation
#define GetExceptionInformation() ((PEXCEPTION_POINTERS)0)
#endif
#ifndef GetExceptionCode
#define GetExceptionCode()      0
#endif
#ifndef EXCEPTION_EXECUTE_HANDLER
#define EXCEPTION_EXECUTE_HANDLER 1
#endif
#ifndef EXCEPTION_CONTINUE_SEARCH
#define EXCEPTION_CONTINUE_SEARCH 0
#endif
#endif /* __GNUC__ */

#include <ntifs.h>
#include <ntdddisk.h>

#include "extsup.h"
#include "extctl.h"
#include "extshim.h"

/* ------------------------------------------------------------------ */
/* NT 3.1 import profile                                               */
/* ------------------------------------------------------------------ */

/*
 * Windows NT 3.1 does not export the Lite executive-resource functions,
 * MmCanFileBeTruncated, or the FsRtlNotifyFull / NotifySync change-
 * notification family (all introduced in NT 3.5/4.0). Mapping the Lite
 * calls to their originals is exact; truncation always allowed is the safe
 * choice where the memory manager cannot answer; change notification is an
 * optional feature and is left out on NT 3.1. One binary then loads on NT
 * 3.1 through 4.0. (NT 3.51 and later export the Lite set, so a build
 * without EXN_NT31 uses it directly.)
 */
#ifdef EXN_NT31

/*
 * The mingw DDK aliases the legacy resource names to their Lite forms (the
 * only ones modern ntoskrnl exports). Undo that and import the executive-
 * resource functions that are common to EVERY NT the one binary must load
 * on - 3.1, 3.51 and 4.0 - resolving the Lite names to them.
 *
 * The catch is the shared acquire: NT 3.1 exports ExAcquireResourceShared
 * but not the Lite form, while NT 3.51+ export ExAcquireResourceSharedLite
 * but DROPPED ExAcquireResourceShared. There is no shared-acquire name
 * common to all three, and an import the running kernel cannot resolve makes
 * the OS loader reject the driver as "missing or corrupt" (this is exactly
 * why the single binary failed to load on NT 3.51). The exclusive acquire,
 * by contrast, is exported under the same name (ExAcquireResourceExclusive)
 * by every one of them. So the one binary acquires exclusively wherever the
 * code asks for shared: exclusive is always a safe substitute for shared
 * (it is strictly more restrictive, the lock hierarchy stays acyclic, so no
 * new deadlock is possible), at the only cost of serialising concurrent
 * readers - immaterial on the uniprocessor retro systems this targets.
 * (A Visual C++ build pinned to a single NT version can instead import that
 * version's real shared acquire and keep true reader concurrency.)
 */
#undef ExAcquireResourceExclusive
#undef ExAcquireResourceShared
#undef ExReleaseResourceForThread
#undef ExInitializeResource
#undef ExDeleteResource
#undef ExAcquireResourceExclusiveLite
#undef ExAcquireResourceSharedLite
#undef ExReleaseResourceForThreadLite
#undef ExInitializeResourceLite
#undef ExDeleteResourceLite

/*
 * Resolved at DriverEntry from the running ntoskrnl (see exnres.c): the Lite
 * form on NT 3.51+, the legacy form on NT 3.1. The code calls the Lite names;
 * they expand to calls through these pointers. (Init/Delete return NTSTATUS in
 * the Lite ABI and VOID in the legacy one; the pointer type is NTSTATUS and
 * the legacy return is simply ignored - harmless on stdcall x86.)
 */
typedef NTSTATUS (NTAPI *EXN_PINITRES)(PERESOURCE);
typedef BOOLEAN  (NTAPI *EXN_PACQRES)(PERESOURCE, BOOLEAN);
typedef VOID     (NTAPI *EXN_PRELRES)(PERESOURCE, ERESOURCE_THREAD);
typedef NTSTATUS (NTAPI *EXN_PDELRES)(PERESOURCE);

extern EXN_PINITRES ExnpInitRes;
extern EXN_PACQRES  ExnpAcqExcl;
extern EXN_PACQRES  ExnpAcqShared;
extern EXN_PDELRES  ExnpDelRes;
extern EXN_PRELRES  ExnpRelForThread;

BOOLEAN ExnResolveResourceApi(VOID);

#define ExInitializeResourceLite(r)          ExnpInitRes(r)
#define ExAcquireResourceExclusiveLite(r,w)  ExnpAcqExcl((r),(w))
#define ExAcquireResourceSharedLite(r,w)     ExnpAcqShared((r),(w))
#define ExDeleteResourceLite(r)              ExnpDelRes(r)
#define ExReleaseResourceForThreadLite(r,t)  ExnpRelForThread((r),(t))

/*
 * NT 4.0 introduced the fast-call Iof/Obf forms; NT 3.1 has only the
 * stdcall originals. The DDK maps the plain names to the fast-call ones,
 * so undo that and declare the stdcall exports NT 3.1 actually has.
 */
#undef IoCallDriver
#undef IoCompleteRequest
#undef ObDereferenceObject
__declspec(dllimport) NTSTATUS NTAPI IoCallDriver(PDEVICE_OBJECT, PIRP);
__declspec(dllimport) VOID     NTAPI IoCompleteRequest(PIRP, CCHAR);
__declspec(dllimport) VOID     NTAPI ObDereferenceObject(PVOID);

#define MmCanFileBeTruncated(S, N)      TRUE
#define FsRtlNotifyInitializeSync(S)    ((void)0)
#define FsRtlNotifyUninitializeSync(S)  ((void)0)
#define FsRtlNotifyCleanup(a, b, c)     ((void)0)
#define FsRtlNotifyFullChangeDirectory(a,b,c,d,e,f,g,h,i,j)  ((void)0)
#define FsRtlNotifyFullReportChange(a,b,c,d,e,f,g,h,i)       ((void)0)
#endif /* EXN_NT31 */

#ifdef __GNUC__
/*
 * The mingw DDK IO_STACK_LOCATION already carries the file-system members,
 * so the EXN_NT4 extended-stack cast is a plain identity there.
 */
typedef IO_STACK_LOCATION EXTENDED_IO_STACK_LOCATION, *PEXTENDED_IO_STACK_LOCATION;

/* The DDK headers redefine these SEH intrinsics; re-assert the shim so no
   _abnormal_termination / exception-record references are emitted. */
#undef AbnormalTermination
#define AbnormalTermination()       0
#undef GetExceptionCode
#define GetExceptionCode()          0
#undef GetExceptionInformation
#define GetExceptionInformation()   ((PEXCEPTION_POINTERS)0)
#endif

#ifdef EXN_CROSS
/*
 * NT 3.1 does not export ExAllocatePoolWithTag, the Ke critical-region
 * calls, or the top-level-IRP / verify / requestor helpers (newer builds
 * access them inline through the thread). The universal binary drops the
 * pool tag, makes the critical region a no-op pair, and keeps the top-level
 * IRP in one process-wide slot. A Visual C++ NT 3.1 build would instead use
 * the real inline thread-field accessors.
 */
#define ExAllocatePoolWithTag(Type, Size, Tag)  ExAllocatePool((Type), (Size))
#define KeEnterCriticalRegion()                 ((void)0)
#define KeLeaveCriticalRegion()                 ((void)0)
extern PIRP ExnXTopLevelIrp;
#define IoGetTopLevelIrp()          (ExnXTopLevelIrp)
#define IoSetTopLevelIrp(I)         (ExnXTopLevelIrp = (PIRP)(I))
/* (cleanup runs in the process closing the handle: its locks go) */
#define IoGetRequestorProcess(Irp)  IoGetCurrentProcess()
#define IoGetDeviceToVerify(T)      ((PDEVICE_OBJECT)0)
#define IoSetDeviceToVerify(T, D)   ((void)0)
#endif

/* ------------------------------------------------------------------ */
/* Target configuration                                                */
/* ------------------------------------------------------------------ */

/*
 * The NT4 DDK IO_STACK_LOCATION lacks the file system members
 * (QueryDirectory, NotifyDirectory, FileSystemControl, LockControl);
 * the free ntifs.h provides them through EXTENDED_IO_STACK_LOCATION.
 */
#ifdef EXN_NT4
#define ExnXSp(IrpSp)           ((PEXTENDED_IO_STACK_LOCATION)(IrpSp))
#define ExnMdlAddress(Mdl)      MmGetSystemAddressForMdl(Mdl)
#define EXN_STATUS_DISMOUNTED   STATUS_FILE_INVALID
#else
#define ExnXSp(IrpSp)           (IrpSp)
#define ExnMdlAddress(Mdl)      MmGetSystemAddressForMdlSafe((Mdl), NormalPagePriority)
#define EXN_STATUS_DISMOUNTED   STATUS_VOLUME_DISMOUNTED
#endif

/* Same as the W2K IoSkipCurrentIrpStackLocation, which NT4 lacks */
#define ExnSkipStack(Irp) {                         \
    (Irp)->CurrentLocation++;                       \
    (Irp)->Tail.Overlay.CurrentStackLocation++;     \
}

#define ExnCopyStackToNext(Irp) {                                           \
    PIO_STACK_LOCATION _Sp = IoGetCurrentIrpStackLocation(Irp);            \
    PIO_STACK_LOCATION _Next = IoGetNextIrpStackLocation(Irp);             \
    RtlCopyMemory(_Next, _Sp, FIELD_OFFSET(IO_STACK_LOCATION, CompletionRoutine)); \
    _Next->Control = 0;                                                     \
}

/* ExReleaseResourceLite is not exported by every NT4 build */
#define ExnRelease(Resource) \
    ExReleaseResourceForThreadLite((Resource), ExGetCurrentResourceThread())

#ifndef VPB_LOCKED
#define VPB_LOCKED                  0x00000002
#endif

/* File system IRP details some DDK headers leave out */
#ifndef IRP_MN_QUERY_DIRECTORY
#define IRP_MN_QUERY_DIRECTORY          0x01
#endif
#ifndef IRP_MN_NOTIFY_CHANGE_DIRECTORY
#define IRP_MN_NOTIFY_CHANGE_DIRECTORY  0x02
#endif
#ifndef IRP_MN_USER_FS_REQUEST
#define IRP_MN_USER_FS_REQUEST          0x00
#endif
#ifndef IRP_MN_MOUNT_VOLUME
#define IRP_MN_MOUNT_VOLUME             0x01
#endif
#ifndef IRP_MN_VERIFY_VOLUME
#define IRP_MN_VERIFY_VOLUME            0x02
#endif
#ifndef SL_RESTART_SCAN
#define SL_RESTART_SCAN                 0x01
#endif
#ifndef SL_RETURN_SINGLE_ENTRY
#define SL_RETURN_SINGLE_ENTRY          0x02
#endif
#ifndef SL_INDEX_SPECIFIED
#define SL_INDEX_SPECIFIED              0x04
#endif
#ifndef SL_WATCH_TREE
#define SL_WATCH_TREE                   0x01
#endif
#ifndef SL_OPEN_PAGING_FILE
#define SL_OPEN_PAGING_FILE             0x02
#endif
#ifndef SL_OPEN_TARGET_DIRECTORY
#define SL_OPEN_TARGET_DIRECTORY        0x04
#endif
#ifndef SL_OVERRIDE_VERIFY_VOLUME
#define SL_OVERRIDE_VERIFY_VOLUME       0x02
#endif

#ifndef IO_REMOUNT
#define IO_REMOUNT                  0x00000001
#endif

#ifndef FILE_READ_ONLY_VOLUME
#define FILE_READ_ONLY_VOLUME       0x00080000
#endif

#ifndef FSCTL_IS_VOLUME_DIRTY
#define FSCTL_IS_VOLUME_DIRTY       CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 30, METHOD_BUFFERED, FILE_ANY_ACCESS)
#endif

#ifndef VOLUME_IS_DIRTY
#define VOLUME_IS_DIRTY             0x00000001
#endif

#ifndef FILE_WRITE_TO_END_OF_FILE
#define FILE_WRITE_TO_END_OF_FILE   0xffffffff
#endif

#ifndef IOCTL_DISK_IS_WRITABLE
#define IOCTL_DISK_IS_WRITABLE      CTL_CODE(IOCTL_DISK_BASE, 0x0009, METHOD_BUFFERED, FILE_ANY_ACCESS)
#endif

/* Status codes newer than some DDK headers */
#ifndef STATUS_USER_MAPPED_FILE
#define STATUS_USER_MAPPED_FILE     ((NTSTATUS)0xC0000243L)
#endif
#ifndef STATUS_CANNOT_MAKE
#define STATUS_CANNOT_MAKE          ((NTSTATUS)0xC000016BL)
#endif

/* Information classes newer than NT4, answered on both targets */
#define EXN_CLASS_ATTRIBUTE_TAG             35
#define EXN_CLASS_FS_FULL_SIZE              7

typedef struct _EXN_FILE_ATTRIBUTE_TAG_INFORMATION {
    ULONG       FileAttributes;
    ULONG       ReparseTag;
} EXN_FILE_ATTRIBUTE_TAG_INFORMATION, *PEXN_FILE_ATTRIBUTE_TAG_INFORMATION;

typedef struct _EXN_FILE_FS_FULL_SIZE_INFORMATION {
    LARGE_INTEGER TotalAllocationUnits;
    LARGE_INTEGER CallerAvailableAllocationUnits;
    LARGE_INTEGER ActualAvailableAllocationUnits;
    ULONG       SectorsPerAllocationUnit;
    ULONG       BytesPerSector;
} EXN_FILE_FS_FULL_SIZE_INFORMATION, *PEXN_FILE_FS_FULL_SIZE_INFORMATION;

/* ------------------------------------------------------------------ */
/* Debug output                                                        */
/* ------------------------------------------------------------------ */

#define EXN_PFX                 "[EXTNT] "

#if defined(EXN_DBGPORT)
VOID     ExnDbgPrintf(const char *Format, ...);
#define EXN_DBG(args)           ExnDbgPrintf args
#elif DBG || defined(EXN_DEBUG)
#define EXN_DBG(args)           DbgPrint args
#else
#define EXN_DBG(args)
#endif

/*
 * Debug trace to the QEMU "debugcon" port (0xE9): readable bytes with no
 * kernel-debugger protocol. Boot QEMU with -debugcon file:/path. Compile
 * with -DEXN_DBGPORT to enable; a no-op otherwise.
 */
#ifdef EXN_DBGPORT
static __inline void ExnDbgCh(unsigned char c)
{ unsigned short port = 0x00E9;
  __asm__ __volatile__ ("outb %b0, %w1" : : "a"(c), "d"(port)); }
static __inline void ExnDbgStr(const char *s)
{ while (*s) ExnDbgCh((unsigned char)*s++); }
static __inline void ExnDbgHex(ULONG_PTR v)
{ int i, n = (int)(sizeof(v) * 2); char h[17]; const char *d="0123456789abcdef";
  for (i=n-1;i>=0;i--){ h[i]=d[v&0xF]; v>>=4; } h[n]=0; ExnDbgStr(h); }
static __inline void ExnDbgUni(const void *us)
{ const unsigned short *p; unsigned short len, i;
  len = ((const UNICODE_STRING *)us)->Length;
  p   = (const unsigned short *)((const UNICODE_STRING *)us)->Buffer;
  if (!p) { ExnDbgStr("(null)"); return; }
  for (i=0;i<len/2 && i<120;i++) ExnDbgCh((unsigned char)(p[i]<128?p[i]:'?')); }
#define DBGP(s)     ExnDbgStr(s)
#define DBGX(v)     ExnDbgHex((ULONG_PTR)(v))
#define DBGU(u)     ExnDbgUni(&(u))
#else
#define DBGP(s)     ((void)0)
#define DBGX(v)     ((void)0)
#define DBGU(u)     ((void)0)
#endif


/* ------------------------------------------------------------------ */
/* Structured exception handling                                       */
/* ------------------------------------------------------------------ */

/*
 * The Cache Manager, the Memory Manager and the probe routines report I/O
 * errors and bad user buffers by raising. Code that calls them runs that
 * part through ExnTryCall (exnseh.c), which returns the exception code
 * (STATUS_SUCCESS when nothing was raised). With ExpectedOnly only the
 * codes FsRtlIsNtstatusExpected accepts (and in-page errors) are caught;
 * anything else is a bug and stops the system where it happened.
 */
typedef VOID (*EXN_TRY_BODY)(PVOID Context);

NTSTATUS ExnTryCall(EXN_TRY_BODY Body, PVOID Context, BOOLEAN ExpectedOnly);

/* RtlCopyMemory to or from a caller's buffer, faults caught (mapped status) */
NTSTATUS ExnSafeCopy(PVOID Destination, const VOID *Source, SIZE_T Length);

/* Maps an exception code to the status a request returns */
NTSTATUS ExnExceptionStatus(NTSTATUS Code);

/* ------------------------------------------------------------------ */
/* Limits and tags                                                     */
/* ------------------------------------------------------------------ */

#define EXN_FS_DEVICE_NAME      L"\\Extnt"
#define EXN_TAG                 'tnxE'
#define EXN_TAG_FCB             'Fnxe'
#define EXN_TAG_CCB             'Cnxe'
#define EXN_TAG_RUNS            'Rnxe'
#define EXN_TAG_BUFFER          'Bnxe'
#define EXN_TAG_NAME            'Nnxe'
#define EXN_TAG_WORK            'Wnxe'
#define EXN_TAG_META            'Mnxe'
#define EXN_TAG_EA              'Enxe'
#define EXN_TAG_ENGINE          'enxe'
#define EXN_TAG_INDEX           'Inxe'
#define EXN_TAG_REGISTRY        'Gnxe'

#define EXN_PAGE_SIZE           0x1000
#define EXN_ZERO_CHUNK          0x10000
#define EXN_MAX_TRANSFER        0x100000    /* cap one disk IRP; miniport can't take a multi-hundred-MB transfer */
#define EXN_MAX_LABEL           16          /* s_volume_name: 16 bytes */
#define EXN_MAX_NAME            EXT_MAX_NAME   /* bytes of UTF-8 */
#define EXN_SECTOR_SIZE         512
#define EXN_SECTOR_SHIFT        9
#define EXN_MAX_LINK_DEPTH      8           /* symbolic links followed per open */

/* NT attribute bits we keep (the rest are computed) */
#define EXN_ATTR_SETTABLE       (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN | \
                                 FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_ARCHIVE)

/* ------------------------------------------------------------------ */
/* Node types                                                          */
/* ------------------------------------------------------------------ */

#define EXN_NTC_VCB             ((CSHORT)0x0E21)
#define EXN_NTC_FCB             ((CSHORT)0x0E22)    /* file (or symbolic link opened as itself) */
#define EXN_NTC_DCB             ((CSHORT)0x0E23)    /* directory */
#define EXN_NTC_VFCB            ((CSHORT)0x0E24)    /* whole volume (DASD) */
#define EXN_NTC_CCB             ((CSHORT)0x0E25)

#define ExnNodeType(Ptr)        (*((CSHORT *)(Ptr)))

/* ------------------------------------------------------------------ */
/* Structures                                                          */
/* ------------------------------------------------------------------ */

typedef struct _EXN_VCB EXN_VCB, *PEXN_VCB;

/*
 * NT 3.1's ERESOURCE is larger than later headers describe:
 * every embedded ERESOURCE is followed by padding.
 */
#define EXN_ERESOURCE_PAD       0x40

/* A run of a paging file (kept nonpaged: paging I/O never enters the engine) */
typedef struct _EXN_RUN {
    ULONGLONG   FileBlock;
    ULONGLONG   DiskBlock;
    ULONG       Length;
} EXN_RUN, *PEXN_RUN;

/* Case-insensitive name index of a big directory (see extdctl.c) */
typedef struct _EXN_NAME_INDEX EXN_NAME_INDEX, *PEXN_NAME_INDEX;

typedef struct _EXN_FCB {
    FSRTL_COMMON_FCB_HEADER Header;
    SECTION_OBJECT_POINTERS SectionObjectPointers;
    ERESOURCE   Resource;
    UCHAR       ResourcePad[EXN_ERESOURCE_PAD];
    ERESOURCE   PagingIoResource;
    UCHAR       PagingIoResourcePad[EXN_ERESOURCE_PAD];
    PEXN_VCB    Vcb;
    struct _EXN_FCB *ParentDcb;     /* where it was first opened (directories: the parent) */
    LIST_ENTRY  FcbLinks;
    ULONG       RefCount;           /* file objects + child FCBs */
    ULONG       UncleanCount;
    ULONG       NonCachedUncleanCount;
    ULONG       FcbState;
    SHARE_ACCESS ShareAccess;
    FILE_LOCK   FileLock;
    ULONG       FileLockSpare[16];  /* NT 3.1's FILE_LOCK is larger than the headers' */
    PKTHREAD    LazyWriteThread;

    ULONG       Ino;                /* the file's identity */
    EXT_INODE   Inode;              /* in-memory copy (under the engine) */
    ULONG       Attributes;         /* NT attributes */
    ULONG       DosAttributes;      /* kept in user.DOSATTRIB: hidden/system/archive */
    LARGE_INTEGER CreationTime;
    LARGE_INTEGER LastWriteTime;
    LARGE_INTEGER LastAccessTime;
    LARGE_INTEGER ChangeTime;
    UNICODE_STRING Name;            /* last component as NT sees it */
    UNICODE_STRING FullName;        /* built on demand */
    ULONG       ExtNameLength;      /* the name as stored (UTF-8 bytes) */
    UCHAR       ExtName[EXN_MAX_NAME + 1];

    /* last mapping, so sequential paging I/O does not walk the tree each time
       (under the paging resource; cleared whenever the map changes) */
    ULONGLONG   MapFileBlock;
    ULONGLONG   MapDiskBlock;
    ULONG       MapRun;             /* 0 = nothing cached */
    BOOLEAN     MapHole;

    /* a paging file's whole map */
    PEXN_RUN    Runs;
    ULONG       RunCount;

    PEXN_NAME_INDEX NameIndex;      /* directories */

    /* the name a pending delete removes (the handle that asked for it:
       with hard links that is not necessarily the FCB's own name) */
    ULONG       DeleteParentIno;
    ULONG       DeleteNameLength;
    UCHAR       DeleteName[EXN_MAX_NAME + 1];
} EXN_FCB, *PEXN_FCB;

#define FCB_STATE_ROOT              0x0001
#define FCB_STATE_DELETE_PENDING    0x0004
#define FCB_STATE_DELETED           0x0008  /* off the disk, kept for Cc and Mm */
#define FCB_STATE_INODE_DIRTY       0x0010  /* the inode lags behind the FCB */
#define FCB_STATE_TRUNCATE_ON_CLOSE 0x0020
#define FCB_STATE_SYMLINK           0x0040  /* a symbolic link opened as itself */
#define FCB_STATE_PAGING_FILE       0x0200
#define FCB_STATE_VISITED           0x0400  /* volume flush walk */

typedef struct _EXN_CCB {
    CSHORT      NodeTypeCode;
    CSHORT      NodeByteSize;
    ULONG       Flags;
    ULONG       QueryState;         /* 0: ".", 1: "..", 2: entries */
    ULONGLONG   Cookie;             /* directory position of the next entry */
    ULONG       EaIndex;            /* next EA for a query-EA continuation */
    UNICODE_STRING Pattern;         /* upcased */
    /* the name this handle was opened under (hard links: delete/rename act on it) */
    ULONG       ParentIno;
    ULONG       NameLength;
    UCHAR       Name[EXN_MAX_NAME + 1];
} EXN_CCB, *PEXN_CCB;

#define CCB_FLAG_PATTERN_SET        0x0001
#define CCB_FLAG_WILDCARD           0x0002
#define CCB_FLAG_MATCH_ALL          0x0004
#define CCB_FLAG_VOLUME_OPEN        0x0008
#define CCB_FLAG_DISMOUNTED_VOLUME  0x0010
#define CCB_FLAG_DELETE_ON_CLOSE    0x0020
#define CCB_FLAG_USER_SET_WRITE     0x0040
#define CCB_FLAG_USER_SET_ACCESS    0x0080
#define CCB_FLAG_WRITE_HANDLE       0x0100
#define CCB_FLAG_HAVE_NAME          0x0200

/* One cached metadata block (a run of sectors the engine maps) */
typedef struct _EXN_META {
    LIST_ENTRY  Links;              /* LRU (most recent at the head) */
    struct _EXN_META *HashNext;
    ULONGLONG   Sector;
    ULONG       Count;
    ULONG       Refs;
    BOOLEAN     Dirty;
    BOOLEAN     IsData;             /* file data the engine moved: written at once */
    PUCHAR      Data;
} EXN_META, *PEXN_META;

#define EXN_META_HASH           1024
#define EXN_META_MAX_BYTES      (2 * 1024 * 1024)   /* clean blocks kept */
#define EXN_COMMIT_SECONDS      5                   /* journal commit interval */

struct _EXN_VCB {
    CSHORT      NodeTypeCode;
    CSHORT      NodeByteSize;
    ERESOURCE   Resource;
    UCHAR       ResourcePad[EXN_ERESOURCE_PAD];
    ERESOURCE   EngineResource;     /* one thread in the engine at a time */
    UCHAR       EngineResourcePad[EXN_ERESOURCE_PAD];
    LIST_ENTRY  VcbLinks;
    PVPB        Vpb;
    PDEVICE_OBJECT TargetDeviceObject;
    PDEVICE_OBJECT VolumeDeviceObject;
    ULONG       VcbState;
    ULONG       OpenCount;
    ULONG       UncleanCount;
    ULONG       WriteCount;
    PFILE_OBJECT LockFileObject;
    PKTHREAD    VerifyThread;
    PEXN_FCB    VolumeFcb;
    PEXN_FCB    RootDcb;
    LIST_ENTRY  FcbList;

    EXT_VOL     Vol;                /* engine state */
    LONGLONG    VolumeBytes;        /* file system size */
    LONGLONG    PartitionBytes;
    ULONG       SerialNumber;
    BOOLEAN     Writing;            /* ExtStartWriting done: the volume is in use */
    BOOLEAN     BootView;           /* extboot made it bootable: keep its FAT view current */
    ULONG       LabelLength;        /* characters */
    WCHAR       Label[EXN_MAX_LABEL];

    /* Metadata cache (under EngineResource) */
    LIST_ENTRY  MetaLru;
    PEXN_META   MetaHash[EXN_META_HASH];
    ULONG       MetaBytes;
    ULONG       MetaDirty;          /* dirty blocks */
    ULONG       MetaDirtyBytes;
    ULONG       MetaDirtyLimit;     /* commit when more is dirty */
    NTSTATUS    MetaWriteStatus;    /* first write error since the last flush */
    ULONG       Commits;

    /* Periodic journal commit */
    KTIMER      CommitTimer;
    KDPC        CommitDpc;
    WORK_QUEUE_ITEM CommitItem;
    LONG        CommitQueued;
    BOOLEAN     TimerSet;
    KEVENT      CommitIdle;         /* signalled when no commit item is pending */

    PNOTIFY_SYNC NotifySync;
    LIST_ENTRY  DirNotifyList;
};

#define VCB_STATE_MOUNTED           0x0001
#define VCB_STATE_LOCKED            0x0002
#define VCB_STATE_DISMOUNTED        0x0004
#define VCB_STATE_IN_DISMOUNT       0x0008
#define VCB_STATE_DELETE_PENDING    0x0010
#define VCB_STATE_DASD_WRITTEN      0x0020
#define VCB_STATE_REMOVABLE         0x0040
#define VCB_STATE_PNP_LOCKED        0x0080
#define VCB_STATE_FREE_VPB          0x0100
#define VCB_STATE_REMOVED           0x0200
#define VCB_STATE_READ_ONLY         0x0400
#define VCB_STATE_FLUSH_ON_CLOSE    0x1000
#define VCB_STATE_SHUTDOWN          0x2000

typedef struct _EXN_DATA {
    PDRIVER_OBJECT  DriverObject;
    PDEVICE_OBJECT  FileSystemDeviceObject;
    ERESOURCE       Resource;
    UCHAR           ResourcePad[EXN_ERESOURCE_PAD];
    LIST_ENTRY      VcbList;
    FAST_IO_DISPATCH FastIoDispatch;
    CACHE_MANAGER_CALLBACKS CacheManagerCallbacks;
    UNICODE_STRING  RegistryPath;
    BOOLEAN         HideDotFiles;   /* registry: names starting with '.' are hidden */
    BOOLEAN         FollowLinks;    /* registry: follow symbolic links on open */
    BOOLEAN         CaseSensitive;  /* registry: exact-case lookups only */
    BOOLEAN         Writable;       /* registry: allow writing at all */
    ULONG           DefaultUid, DefaultGid;
    ULONG           FileMode, DirMode;  /* permission bits of new files / dirs */
    BOOLEAN         ShuttingDown;   /* IRP_MJ_SHUTDOWN: skip the disk cache-flush */
} EXN_DATA, *PEXN_DATA;

extern EXN_DATA ExnData;

typedef struct _EXN_IRP_CONTEXT {
    PIRP            Irp;
    PIO_STACK_LOCATION IrpSp;
    PDEVICE_OBJECT  DeviceObject;
    PEXN_VCB        Vcb;
    ULONG           Flags;
    NTSTATUS        ExceptionStatus;
    NTSTATUS        DispatchStatus;
} EXN_IRP_CONTEXT, *PEXN_IRP_CONTEXT;

#define EXN_CTX_TOP_LEVEL           0x0001
#define EXN_CTX_NO_COMPLETE         0x0002

typedef struct _EXN_CLOSE_ITEM {
    WORK_QUEUE_ITEM Item;
    PEXN_VCB    Vcb;
    PEXN_FCB    Fcb;
    PEXN_CCB    Ccb;
} EXN_CLOSE_ITEM, *PEXN_CLOSE_ITEM;

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

#define ExnIsFcb(Fcb)       ((Fcb)->Header.NodeTypeCode == EXN_NTC_FCB)
#define ExnIsDcb(Fcb)       ((Fcb)->Header.NodeTypeCode == EXN_NTC_DCB)
#define ExnIsVfcb(Fcb)      ((Fcb)->Header.NodeTypeCode == EXN_NTC_VFCB)

#define ExnRoundUp(Value, Size) \
    (((Value) + ((Size) - 1)) & ~((LONGLONG)(Size) - 1))

#define EXN_WRITE_ACCESS    (FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_EA | \
                             FILE_WRITE_ATTRIBUTES | DELETE | FILE_DELETE_CHILD)

/* the largest file the volume's mapping can describe */
#define ExnMaxFileSize(Vcb) ((Vcb)->Vol.HasExtents ? \
    ((ULONGLONG)0xFFFFFFFF << (Vcb)->Vol.BlockShift) : \
    (((ULONGLONG)12 + ((Vcb)->Vol.BlockSize / 4) + \
      (ULONGLONG)((Vcb)->Vol.BlockSize / 4) * ((Vcb)->Vol.BlockSize / 4) + \
      (ULONGLONG)((Vcb)->Vol.BlockSize / 4) * ((Vcb)->Vol.BlockSize / 4) * ((Vcb)->Vol.BlockSize / 4)) \
      << (Vcb)->Vol.BlockShift))

/* engine status -> NT status (most engine codes are NT codes already) */
NTSTATUS ExnStatus(EXT_STATUS S);

/* ------------------------------------------------------------------ */
/* Prototypes                                                          */
/* ------------------------------------------------------------------ */

/* extinit.c */
NTSTATUS NTAPI DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath);
BOOLEAN ExnWriteSupportEnabled(VOID);

/* extdisp.c */
NTSTATUS NTAPI ExnFsdDispatch(PDEVICE_OBJECT DeviceObject, PIRP Irp);
NTSTATUS ExnVerifyVcb(PEXN_IRP_CONTEXT Ctx, PEXN_VCB Vcb);
NTSTATUS ExnVerifyWritable(PEXN_IRP_CONTEXT Ctx, PEXN_VCB Vcb);

/* extcrea.c */
NTSTATUS ExnCommonCreate(PEXN_IRP_CONTEXT Ctx);

/* extclos.c */
NTSTATUS ExnCommonCleanup(PEXN_IRP_CONTEXT Ctx);
NTSTATUS ExnCommonClose(PEXN_IRP_CONTEXT Ctx);
BOOLEAN  ExnTryTeardown(PEXN_VCB Vcb, ULONG InFlightReferences);
VOID     ExnDeleteVcb(PEXN_VCB Vcb);
BOOLEAN  ExnDismountVcb(PEXN_VCB Vcb);
VOID     ExnUnlockVcb(PEXN_VCB Vcb);
VOID     ExnPurgeCachedFiles(PEXN_VCB Vcb);
NTSTATUS ExnUnlinkName(PEXN_VCB Vcb, PEXN_FCB Fcb);
VOID     ExnSetDeleteName(PEXN_FCB Fcb, PEXN_CCB Ccb);
PEXT_INODE ExnDirInode(PEXN_VCB Vcb, ULONG Ino, PEXT_INODE Temp);

/* extread.c / extwrit.c */
NTSTATUS ExnCommonRead(PEXN_IRP_CONTEXT Ctx);
NTSTATUS ExnCommonWrite(PEXN_IRP_CONTEXT Ctx);
NTSTATUS ExnZeroFileRange(PEXN_FCB Fcb, PFILE_OBJECT FileObject, LONGLONG Start, LONGLONG End);

/* extdctl.c */
NTSTATUS ExnCommonDirectoryControl(PEXN_IRP_CONTEXT Ctx);
VOID     ExnNotifyCleanupList(PEXN_VCB Vcb, PEXN_CCB Ccb);
VOID     ExnNotifyChange(PEXN_VCB Vcb, PEXN_FCB Fcb, ULONG Filter, ULONG Action);
NTSTATUS ExnLookupName(PEXN_VCB Vcb, PEXN_FCB Dir, PCUNICODE_STRING Name, BOOLEAN CaseSensitive,
                       PEXT_ENTRY Entry);
VOID     ExnIndexAdd(PEXN_FCB Dir, const UCHAR *Name, ULONG Length, ULONG Ino);
VOID     ExnIndexRemove(PEXN_FCB Dir, const UCHAR *Name, ULONG Length);
VOID     ExnIndexFree(PEXN_FCB Dir);

/* extinfo.c */
NTSTATUS ExnCommonQueryInformation(PEXN_IRP_CONTEXT Ctx);
ULONG    ExnNtAttributes(ULONG Attributes);
VOID     ExnFillBasicInfo(PEXN_FCB Fcb, PFILE_BASIC_INFORMATION Info);
VOID     ExnFillStandardInfo(PEXN_FCB Fcb, PFILE_STANDARD_INFORMATION Info);
VOID     ExnFillNetworkOpenInfo(PEXN_FCB Fcb, PFILE_NETWORK_OPEN_INFORMATION Info);

/* extseti.c */
NTSTATUS ExnCommonSetInformation(PEXN_IRP_CONTEXT Ctx);
NTSTATUS ExnSetFileSize(PEXN_IRP_CONTEXT Ctx, PEXN_FCB Fcb, PFILE_OBJECT FileObject, LONGLONG NewSize);

/* exteas.c */
NTSTATUS ExnCommonQueryEa(PEXN_IRP_CONTEXT Ctx);
NTSTATUS ExnCommonSetEa(PEXN_IRP_CONTEXT Ctx);
NTSTATUS ExnSetEaBuffer(PEXN_VCB Vcb, PEXN_FCB Fcb, PFILE_FULL_EA_INFORMATION Buffer, ULONG Length,
                        BOOLEAN Replace);
ULONG    ExnNtEaSize(PEXN_VCB Vcb, PEXN_FCB Fcb);
NTSTATUS ExnCheckEaBuffer(PFILE_FULL_EA_INFORMATION Buffer, ULONG Length, PULONG ErrorOffset);

/* extvol.c */
NTSTATUS ExnCommonQueryVolumeInformation(PEXN_IRP_CONTEXT Ctx);
NTSTATUS ExnCommonSetVolumeInformation(PEXN_IRP_CONTEXT Ctx);

/* extfsct.c */
NTSTATUS ExnCommonFileSystemControl(PEXN_IRP_CONTEXT Ctx);
NTSTATUS ExnCommonPnp(PEXN_IRP_CONTEXT Ctx);
NTSTATUS ExnUserFsRequest(PEXN_IRP_CONTEXT Ctx);
VOID     ExnLoadLabel(PEXN_VCB Vcb);
NTSTATUS ExnLoadVolume(PEXN_VCB Vcb);
BOOLEAN  ExnSameVolume(PEXN_VCB Vcb);

/* extletr.c */
VOID     ExnStartDriveLetters(VOID);
VOID     ExnClaimRegistryLetter(PEXN_VCB Vcb, PDEVICE_OBJECT TargetDevice, LONGLONG StartingOffset);

/* extmisc.c */
NTSTATUS ExnCommonDeviceControl(PEXN_IRP_CONTEXT Ctx);
NTSTATUS ExnCommonLockControl(PEXN_IRP_CONTEXT Ctx);

/* extflsh.c */
NTSTATUS ExnCommonFlushBuffers(PEXN_IRP_CONTEXT Ctx);
NTSTATUS ExnCommonShutdown(PEXN_IRP_CONTEXT Ctx);
NTSTATUS ExnFlushFile(PEXN_VCB Vcb, PEXN_FCB Fcb);
NTSTATUS ExnFlushVolume(PEXN_VCB Vcb, BOOLEAN Final);
NTSTATUS ExnStartWriting(PEXN_VCB Vcb);
NTSTATUS ExnStopWriting(PEXN_VCB Vcb);
VOID     ExnResumeWriting(PEXN_VCB Vcb);

/* extfast.c */
VOID     ExnInitializeFastIo(PFAST_IO_DISPATCH FastIo);
VOID     ExnInitializeCacheCallbacks(VOID);
UCHAR    ExnIsFastIoPossible(PEXN_FCB Fcb);

/* extmeta.c: the engine's host and the metadata cache */
VOID     ExnEngineEnter(PEXN_VCB Vcb);
NTSTATUS ExnEngineLeave(PEXN_VCB Vcb, NTSTATUS Status);
VOID ExnUpdateBootView(PEXN_VCB Vcb);
#ifdef EXN_NT31
extern BOOLEAN ExnNt31;                       /* running on NT 3.1 (extres.c) */
#endif       /* extshim view (engine not held) */
NTSTATUS ExnCommit(PEXN_VCB Vcb);              /* journal commit (engine held) */
NTSTATUS ExnCommitNow(PEXN_VCB Vcb);           /* takes the engine itself */
VOID     ExnMetaPurge(PEXN_VCB Vcb);
VOID     ExnMetaInitialize(PEXN_VCB Vcb);
VOID     ExnCommitTimerStart(PEXN_VCB Vcb);
VOID     ExnCommitTimerStop(PEXN_VCB Vcb);

/* extio.c */
NTSTATUS ExnSyncIo(PDEVICE_OBJECT Device, UCHAR MajorFunction, LONGLONG Offset,
                   ULONG Length, PVOID Buffer, BOOLEAN OverrideVerify);
NTSTATUS ExnDeviceIoctl(PDEVICE_OBJECT Device, ULONG IoControlCode, PVOID InputBuffer,
                        ULONG InputLength, PVOID OutputBuffer, ULONG OutputLength,
                        BOOLEAN OverrideVerify, PULONG Information);
NTSTATUS ExnFlushDevice(PEXN_VCB Vcb);
NTSTATUS ExnLockUserBuffer(PIRP Irp, LOCK_OPERATION Operation, ULONG Length);
PVOID    ExnMapUserBuffer(PIRP Irp);
NTSTATUS ExnNonCachedIo(PEXN_IRP_CONTEXT Ctx, PEXN_FCB Fcb, UCHAR MajorFunction,
                        LONGLONG StartingVbo, ULONG ByteCount, LONGLONG ValidData);
NTSTATUS ExnZeroDisk(PEXN_VCB Vcb, PEXN_FCB Fcb, LONGLONG Start, LONGLONG End);
NTSTATUS ExnVboToLbo(PEXN_VCB Vcb, PEXN_FCB Fcb, LONGLONG Vbo, BOOLEAN Allocate,
                     PLONGLONG Lbo, PULONG Contiguous);
NTSTATUS ExnReadDisk(PEXN_VCB Vcb, ULONGLONG Sector, ULONG Count, PVOID Buffer);
NTSTATUS ExnWriteDisk(PEXN_VCB Vcb, ULONGLONG Sector, ULONG Count, const VOID *Buffer);
NTSTATUS ExnAllocateRange(PEXN_VCB Vcb, PEXN_FCB Fcb, LONGLONG Start, LONGLONG End);

#define ExnReadSectors(Device, Offset, Length, Buffer, Override) \
    ExnSyncIo((Device), IRP_MJ_READ, (Offset), (Length), (Buffer), (Override))
#define ExnWriteSectors(Device, Offset, Length, Buffer, Override) \
    ExnSyncIo((Device), IRP_MJ_WRITE, (Offset), (Length), (Buffer), (Override))

/* extstrc.c */
PEXN_FCB ExnCreateVolumeFcb(PEXN_VCB Vcb);
PEXN_FCB ExnCreateRootDcb(PEXN_VCB Vcb);
NTSTATUS ExnCreateFcb(PEXN_VCB Vcb, PEXN_FCB ParentDcb, PEXT_ENTRY Entry, BOOLEAN AsLink, PEXN_FCB *Fcb);
VOID     ExnDeleteFcb(PEXN_FCB Fcb);
VOID     ExnDereferenceFcb(PEXN_VCB Vcb, PEXN_FCB Fcb);
PEXN_FCB ExnFindFcbByIno(PEXN_VCB Vcb, ULONG Ino, BOOLEAN AsLink);
PEXN_CCB ExnCreateCcb(VOID);
VOID     ExnDeleteCcb(PEXN_CCB Ccb);
NTSTATUS ExnBuildFullName(PEXN_FCB Fcb);
VOID     ExnForgetNames(PEXN_VCB Vcb, PEXN_FCB Fcb);
BOOLEAN  ExnIsAncestor(PEXN_FCB Ancestor, PEXN_FCB Fcb);
LARGE_INTEGER ExnToNtTime(ULONG Seconds, ULONG Extra);
VOID     ExnFromNtTime(LARGE_INTEGER Time, PULONG Seconds, PULONG Extra);
NTSTATUS ExnLoadInode(PEXN_VCB Vcb, PEXN_FCB Fcb);
VOID     ExnInodeToFcb(PEXN_VCB Vcb, PEXN_FCB Fcb);
NTSTATUS ExnUpdateInode(PEXN_VCB Vcb, PEXN_FCB Fcb);
NTSTATUS ExnSetAllocation(PEXN_VCB Vcb, PEXN_FCB Fcb, ULONGLONG Bytes);
NTSTATUS ExnLoadRuns(PEXN_VCB Vcb, PEXN_FCB Fcb);
VOID     ExnForgetMapping(PEXN_FCB Fcb);
NTSTATUS ExnUnicodeToExt(PCUNICODE_STRING Name, PUCHAR Out, PULONG OutLength);
NTSTATUS ExnExtToUnicode(const UCHAR *In, ULONG InLength, PUNICODE_STRING Name);
BOOLEAN  ExnIsValidUnicodeName(PCUNICODE_STRING Name);
ULONG    ExnComputeAttributes(PEXN_VCB Vcb, PEXT_INODE Inode, const UCHAR *Name, ULONG NameLength,
                              ULONG DosAttributes);
ULONG    ExnLoadDosAttributes(PEXN_VCB Vcb, ULONG Ino, PEXT_INODE Inode);
NTSTATUS ExnStoreDosAttributes(PEXN_VCB Vcb, PEXN_FCB Fcb, ULONG Attributes);

#endif /* _EXT_H_ */
