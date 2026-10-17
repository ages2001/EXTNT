/*
 * EXTNT - IRP_MJ_CREATE
 *
 * Opens, creates, overwrites and supersedes files and directories (with
 * extended attributes from the create's EA buffer), opens the volume itself,
 * and opens the parent of a rename target (SL_OPEN_TARGET_DIRECTORY).
 *
 * Names are looked up exactly first and then without regard to case (NT
 * callers expect that; ext itself is case-sensitive). Symbolic links on the
 * path are followed within the volume (an absolute target starts at the
 * volume root); the last component is opened as the link itself when the
 * caller asks for DELETE access or FILE_OPEN_REPARSE_POINT, so deleting or
 * renaming a link acts on the link.
 *
 * EXTNT keeps no NT security: MAXIMUM_ALLOWED is granted as everything the
 * read-only attribute and the medium allow.
 */

#include "ext.h"

#ifndef FILE_ALL_ACCESS
#define FILE_ALL_ACCESS (STANDARD_RIGHTS_REQUIRED | SYNCHRONIZE | 0x1FF)
#endif
#ifndef FILE_EXISTS
#define FILE_EXISTS                 0x00000004
#endif
#ifndef FILE_DOES_NOT_EXIST
#define FILE_DOES_NOT_EXIST         0x00000005
#endif
#ifndef FILE_OPEN_REPARSE_POINT
#define FILE_OPEN_REPARSE_POINT     0x00200000
#endif
#ifndef SL_CASE_SENSITIVE
#define SL_CASE_SENSITIVE           0x80
#endif

#define EXN_DATA_WRITE_ACCESS  (FILE_WRITE_DATA | FILE_APPEND_DATA)

static BOOLEAN
ExnNextComponent (
    PUNICODE_STRING Remaining,
    PUNICODE_STRING Component
    )
{
    USHORT Count = Remaining->Length / sizeof(WCHAR);
    USHORT i;

    if (Count == 0) {
        return FALSE;
    }
    for (i = 0; i < Count && Remaining->Buffer[i] != L'\\'; i++) {
        ;
    }
    Component->Buffer = Remaining->Buffer;
    Component->Length = (USHORT)(i * sizeof(WCHAR));
    Component->MaximumLength = Component->Length;
    if (i < Count) {
        i++;
    }
    Remaining->Buffer += i;
    Remaining->Length = (USHORT)(Remaining->Length - i * sizeof(WCHAR));
    Remaining->MaximumLength = Remaining->Length;
    return TRUE;
}

static VOID
ExnDropUnopened (
    PEXN_VCB Vcb,
    PEXN_FCB Fcb
    )
{
    if (Fcb != NULL && Fcb->RefCount == 0 &&
        Fcb != Vcb->RootDcb && Fcb != Vcb->VolumeFcb) {
        Fcb->RefCount = 1;
        ExnDereferenceFcb(Vcb, Fcb);
    }
}

static ACCESS_MASK
ExnResolveAccess (
    PIO_STACK_LOCATION IrpSp,
    PEXN_VCB Vcb,
    PEXN_FCB Fcb
    )
{
    PIO_SECURITY_CONTEXT SecurityContext = IrpSp->Parameters.Create.SecurityContext;
    PACCESS_STATE AccessState = SecurityContext->AccessState;
    ACCESS_MASK Access = SecurityContext->DesiredAccess;
    ACCESS_MASK Granted;

    if (Access & MAXIMUM_ALLOWED) {

        Granted = FILE_ALL_ACCESS;

        if (Vcb->VcbState & VCB_STATE_READ_ONLY) {
            Granted &= ~EXN_WRITE_ACCESS;
        } else if (Fcb != NULL && ExnIsFcb(Fcb) && (Fcb->Attributes & FILE_ATTRIBUTE_READONLY)) {
            Granted &= ~EXN_DATA_WRITE_ACCESS;
        }

        Granted |= Access & ~MAXIMUM_ALLOWED;

        if (AccessState != NULL) {
            AccessState->PreviouslyGrantedAccess |= Granted;
            AccessState->RemainingDesiredAccess &= ~(Granted | MAXIMUM_ALLOWED);
        }

        Access = Granted;
    }

    return Access;
}

static NTSTATUS
ExnOpenVolume (
    PEXN_IRP_CONTEXT Ctx,
    PEXN_VCB Vcb,
    ACCESS_MASK DesiredAccess,
    USHORT ShareAccess,
    ULONG CreateDisposition,
    ULONG CreateOptions
    )
{
    PFILE_OBJECT FileObject = Ctx->IrpSp->FileObject;
    PEXN_FCB Vfcb = Vcb->VolumeFcb;
    PEXN_CCB Ccb;
    NTSTATUS Status;

    if (CreateOptions & FILE_DIRECTORY_FILE) {
        return STATUS_NOT_A_DIRECTORY;
    }
    if (CreateDisposition != FILE_OPEN && CreateDisposition != FILE_OPEN_IF) {
        return STATUS_ACCESS_DENIED;
    }
    if (CreateOptions & FILE_DELETE_ON_CLOSE) {
        return STATUS_CANNOT_DELETE;
    }

    if (Vfcb->UncleanCount != 0) {
        Status = IoCheckShareAccess(DesiredAccess, ShareAccess, FileObject, &Vfcb->ShareAccess, TRUE);
        if (!NT_SUCCESS(Status)) {
            return Status;
        }
    } else {
        IoSetShareAccess(DesiredAccess, ShareAccess, FileObject, &Vfcb->ShareAccess);
    }

    Ccb = ExnCreateCcb();
    if (Ccb == NULL) {
        IoRemoveShareAccess(FileObject, &Vfcb->ShareAccess);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Ccb->Flags |= CCB_FLAG_VOLUME_OPEN;

    FileObject->FsContext = Vfcb;
    FileObject->FsContext2 = Ccb;
    FileObject->SectionObjectPointer = &Vfcb->SectionObjectPointers;
    FileObject->Vpb = Vcb->Vpb;

    Vfcb->RefCount++;
    Vfcb->UncleanCount++;
    Vcb->OpenCount++;
    Vcb->UncleanCount++;

    Ctx->Irp->IoStatus.Information = FILE_OPENED;
    return STATUS_SUCCESS;
}

/* The name a handle was opened under (delete/rename of hard links use it) */
typedef struct _EXN_OPEN_NAME {
    ULONG       ParentIno;
    ULONG       Length;
    UCHAR       Name[EXN_MAX_NAME + 1];
} EXN_OPEN_NAME, *PEXN_OPEN_NAME;

static NTSTATUS
ExnOpenFcb (
    PEXN_IRP_CONTEXT Ctx,
    PEXN_VCB Vcb,
    PEXN_FCB Fcb,
    ACCESS_MASK DesiredAccess,
    USHORT ShareAccess,
    ULONG CreateOptions,
    PEXN_OPEN_NAME OpenName
    )
{
    PFILE_OBJECT FileObject = Ctx->IrpSp->FileObject;
    PEXN_CCB Ccb;
    NTSTATUS Status;

    if (Fcb->UncleanCount != 0) {
        Status = IoCheckShareAccess(DesiredAccess, ShareAccess, FileObject, &Fcb->ShareAccess, TRUE);
        if (!NT_SUCCESS(Status)) {
            return Status;
        }
    } else {
        IoSetShareAccess(DesiredAccess, ShareAccess, FileObject, &Fcb->ShareAccess);
    }

    Ccb = ExnCreateCcb();
    if (Ccb == NULL) {
        IoRemoveShareAccess(FileObject, &Fcb->ShareAccess);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    if (OpenName != NULL && OpenName->Length != 0) {
        Ccb->ParentIno = OpenName->ParentIno;
        Ccb->NameLength = OpenName->Length;
        RtlCopyMemory(Ccb->Name, OpenName->Name, OpenName->Length);
        Ccb->Name[OpenName->Length] = 0;
        Ccb->Flags |= CCB_FLAG_HAVE_NAME;
    }

    if (CreateOptions & FILE_DELETE_ON_CLOSE) {
        Ccb->Flags |= CCB_FLAG_DELETE_ON_CLOSE;
    }
    if (DesiredAccess & EXN_WRITE_ACCESS) {
        Ccb->Flags |= CCB_FLAG_WRITE_HANDLE;
        Vcb->WriteCount++;
    }

    FileObject->FsContext = Fcb;
    FileObject->FsContext2 = Ccb;
    FileObject->SectionObjectPointer = &Fcb->SectionObjectPointers;
    FileObject->Vpb = Vcb->Vpb;

    if (Ctx->IrpSp->Flags & SL_OPEN_PAGING_FILE) {
        Fcb->FcbState |= FCB_STATE_PAGING_FILE;
        Fcb->Header.IsFastIoPossible = FastIoIsNotPossible;
    }

    if (!(FileObject->Flags & FO_NO_INTERMEDIATE_BUFFERING) &&
        !(Fcb->FcbState & FCB_STATE_PAGING_FILE)) {
        FileObject->Flags |= FO_CACHE_SUPPORTED;
    }

    Fcb->RefCount++;
    Fcb->UncleanCount++;
    Vcb->OpenCount++;
    Vcb->UncleanCount++;

    if (FileObject->Flags & FO_NO_INTERMEDIATE_BUFFERING) {
        Fcb->NonCachedUncleanCount++;
    }
    if (ExnIsFcb(Fcb)) {
        Fcb->Header.IsFastIoPossible = ExnIsFastIoPossible(Fcb);
    }

    Ctx->Irp->IoStatus.Information = FILE_OPENED;
    return STATUS_SUCCESS;
}

static VOID
ExnUndoOpen (
    PEXN_IRP_CONTEXT Ctx,
    PEXN_VCB Vcb,
    PEXN_FCB Fcb
    )
{
    PFILE_OBJECT FileObject = Ctx->IrpSp->FileObject;
    PEXN_CCB Ccb = (PEXN_CCB)FileObject->FsContext2;

    IoRemoveShareAccess(FileObject, &Fcb->ShareAccess);
    if (Ccb->Flags & CCB_FLAG_WRITE_HANDLE) {
        Vcb->WriteCount--;
    }
    if (FileObject->Flags & FO_NO_INTERMEDIATE_BUFFERING) {
        Fcb->NonCachedUncleanCount--;
    }
    Fcb->UncleanCount--;
    Fcb->RefCount--;
    Vcb->OpenCount--;
    Vcb->UncleanCount--;
    ExnDeleteCcb(Ccb);
    FileObject->FsContext = NULL;
    FileObject->FsContext2 = NULL;
}

/* The create's EA buffer, checked; NULL if none */
static NTSTATUS
ExnCreateEaBuffer (
    PEXN_IRP_CONTEXT Ctx,
    PFILE_FULL_EA_INFORMATION *Buffer,
    PULONG Length
    )
{
    PFILE_FULL_EA_INFORMATION Ea = (PFILE_FULL_EA_INFORMATION)Ctx->Irp->AssociatedIrp.SystemBuffer;
    ULONG EaLength = Ctx->IrpSp->Parameters.Create.EaLength;
    ULONG ErrorOffset;
    NTSTATUS Status;

    *Buffer = NULL;
    *Length = 0;
    if (Ea == NULL || EaLength == 0) {
        return STATUS_SUCCESS;
    }
    Status = ExnCheckEaBuffer(Ea, EaLength, &ErrorOffset);
    if (!NT_SUCCESS(Status)) {
        Ctx->Irp->IoStatus.Information = ErrorOffset;
        return Status;
    }
    *Buffer = Ea;
    *Length = EaLength;
    return STATUS_SUCCESS;
}

/* Applies NT attributes asked for at create/overwrite: read-only to the mode
   bits, hidden/system/archive to user.DOSATTRIB */
static NTSTATUS
ExnApplyCreateAttributes (
    PEXN_VCB Vcb,
    PEXN_FCB Fcb,
    ULONG Attributes,
    BOOLEAN Replace
    )
{
    ULONG Dos = Attributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_ARCHIVE);
    NTSTATUS Status = STATUS_SUCCESS;

    if (!Replace) {
        Dos |= Fcb->DosAttributes;
    }
    ExnEngineEnter(Vcb);
    if (ExnIsFcb(Fcb)) {
        USHORT Mode = Fcb->Inode.Mode;
        if (Attributes & FILE_ATTRIBUTE_READONLY) {
            Mode &= (USHORT)~0222;
        } else if (Replace || !(Mode & 0200)) {
            if (Replace) Mode |= 0200;
        }
        if (Mode != Fcb->Inode.Mode) {
            Fcb->Inode.Mode = Mode;
            Status = ExtWriteInode(&Vcb->Vol, Fcb->Ino, &Fcb->Inode);
        }
    }
    if (NT_SUCCESS(Status) && Dos != Fcb->DosAttributes) {
        Status = ExnStoreDosAttributes(Vcb, Fcb, Dos);
    }
    Status = ExnEngineLeave(Vcb, Status);
    ExnInodeToFcb(Vcb, Fcb);
    return Status;
}

/* FILE_SUPERSEDE, FILE_OVERWRITE and FILE_OVERWRITE_IF of an existing file */
static NTSTATUS
ExnOverwriteFile (
    PEXN_IRP_CONTEXT Ctx,
    PEXN_VCB Vcb,
    PEXN_FCB Fcb,
    ULONG CreateDisposition
    )
{
    PFILE_OBJECT FileObject = Ctx->IrpSp->FileObject;
    ULONG Attributes = Ctx->IrpSp->Parameters.Create.FileAttributes & EXN_ATTR_SETTABLE;
    PFILE_FULL_EA_INFORMATION Ea;
    ULONG EaLength;
    LARGE_INTEGER Zero;
    NTSTATUS Status;

    Zero.QuadPart = 0;

    if (!MmCanFileBeTruncated(&Fcb->SectionObjectPointers, &Zero)) {
        return STATUS_USER_MAPPED_FILE;
    }

    Status = ExnCreateEaBuffer(Ctx, &Ea, &EaLength);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    (VOID)ExAcquireResourceExclusiveLite(&Fcb->Resource, TRUE);

    (VOID)CcPurgeCacheSection(&Fcb->SectionObjectPointers, NULL, 0, FALSE);

    Status = ExnSetFileSize(Ctx, Fcb, FileObject, 0);

    if (NT_SUCCESS(Status)) {
        /* The file's EAs are replaced by the ones given (none: removed) */
        Status = ExnSetEaBuffer(Vcb, Fcb, Ea, EaLength, TRUE);
    }

    if (NT_SUCCESS(Status)) {
        Status = ExnApplyCreateAttributes(Vcb, Fcb, Attributes | FILE_ATTRIBUTE_ARCHIVE,
                                          (BOOLEAN)(CreateDisposition == FILE_SUPERSEDE));
    }

    if (NT_SUCCESS(Status)) {

        KeQuerySystemTime(&Fcb->LastWriteTime);
        Fcb->LastAccessTime = Fcb->LastWriteTime;
        Fcb->ChangeTime = Fcb->LastWriteTime;

        Fcb->FcbState |= FCB_STATE_INODE_DIRTY;
        Status = ExnUpdateInode(Vcb, Fcb);

        ExnNotifyChange(Vcb, Fcb,
                         FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_ATTRIBUTES |
                         FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_EA,
                         FILE_ACTION_MODIFIED);
    }

    ExnRelease(&Fcb->Resource);
    return Status;
}

/* Name was not found in Parent: make it */
static NTSTATUS
ExnCreateNew (
    PEXN_IRP_CONTEXT Ctx,
    PEXN_VCB Vcb,
    PEXN_FCB Parent,
    const UCHAR *Name,
    ULONG NameLength,
    ULONG CreateDisposition,
    ULONG CreateOptions,
    USHORT ShareAccess,
    BOOLEAN TrailingBackslash,
    PEXN_FCB *Created
    )
{
    PIO_STACK_LOCATION IrpSp = Ctx->IrpSp;
    ULONG FileAttributes = IrpSp->Parameters.Create.FileAttributes;
    BOOLEAN IsDirectory = (BOOLEAN)((CreateOptions & FILE_DIRECTORY_FILE) != 0);
    LONGLONG AllocationSize = Ctx->Irp->Overlay.AllocationSize.QuadPart;
    PFILE_FULL_EA_INFORMATION Ea;
    ULONG EaLength, NewIno = 0;
    ACCESS_MASK DesiredAccess;
    PEXN_FCB Fcb = NULL;
    EXT_INODE NewInode;
    EXT_ENTRY Entry;
    EXN_OPEN_NAME OpenName;
    USHORT Mode;
    NTSTATUS Status;

    *Created = NULL;

    if (Vcb->VcbState & VCB_STATE_READ_ONLY) {
        return STATUS_MEDIA_WRITE_PROTECTED;
    }
    if (Parent->FcbState & (FCB_STATE_DELETE_PENDING | FCB_STATE_DELETED)) {
        return STATUS_DELETE_PENDING;
    }
    if (IsDirectory && CreateDisposition != FILE_CREATE && CreateDisposition != FILE_OPEN_IF) {
        return STATUS_INVALID_PARAMETER;
    }
    if (TrailingBackslash && !IsDirectory) {
        return STATUS_OBJECT_NAME_INVALID;
    }

    Status = ExnCreateEaBuffer(Ctx, &Ea, &EaLength);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    if (IsDirectory) {
        Mode = (USHORT)(EXT_S_IFDIR | (ExnData.DirMode & 07777));
    } else {
        Mode = (USHORT)(EXT_S_IFREG | (ExnData.FileMode & 07777));
        if (FileAttributes & FILE_ATTRIBUTE_READONLY) {
            Mode &= (USHORT)~0222;
        }
    }

    ExnEngineEnter(Vcb);
    Status = ExtCreate(&Vcb->Vol, Parent->Ino, &Parent->Inode, (const char *)Name, NameLength, Mode,
                       NULL, &NewIno, &NewInode);
    if (NT_SUCCESS(Status) && (ExnData.DefaultUid || ExnData.DefaultGid)) {
        NewInode.UidLo = (USHORT)ExnData.DefaultUid;
        NewInode.UidHi = (USHORT)(ExnData.DefaultUid >> 16);
        NewInode.GidLo = (USHORT)ExnData.DefaultGid;
        NewInode.GidHi = (USHORT)(ExnData.DefaultGid >> 16);
        Status = ExtWriteInode(&Vcb->Vol, NewIno, &NewInode);
    }
    Status = ExnEngineLeave(Vcb, Status);
    if (!NT_SUCCESS(Status)) {
        return (Status == EXT_EXISTS) ? STATUS_OBJECT_NAME_COLLISION :
               (Status == EXT_DISK_FULL) ? STATUS_DISK_FULL : Status;
    }
    ExnIndexAdd(Parent, Name, NameLength, NewIno);

    RtlZeroMemory(&Entry, sizeof(Entry));
    Entry.Inode = NewIno;
    Entry.NameLength = (USHORT)NameLength;
    RtlCopyMemory(Entry.Name, Name, NameLength);
    Status = ExnCreateFcb(Vcb, Parent, &Entry, FALSE, &Fcb);

    if (NT_SUCCESS(Status) &&
        (FileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_ARCHIVE))) {
        ExnEngineEnter(Vcb);
        Status = ExnEngineLeave(Vcb, ExnStoreDosAttributes(Vcb, Fcb, FileAttributes));
        ExnInodeToFcb(Vcb, Fcb);
    }

    if (NT_SUCCESS(Status) && Ea != NULL) {
        Status = ExnSetEaBuffer(Vcb, Fcb, Ea, EaLength, TRUE);
    }

    if (NT_SUCCESS(Status) && !IsDirectory && AllocationSize > 0) {
        Fcb->Header.AllocationSize.QuadPart = ExnRoundUp(AllocationSize, Vcb->Vol.BlockSize);
    }

    if (NT_SUCCESS(Status)) {
        OpenName.ParentIno = Parent->Ino;
        OpenName.Length = NameLength;
        RtlCopyMemory(OpenName.Name, Name, NameLength);
        DesiredAccess = ExnResolveAccess(IrpSp, Vcb, NULL);
        Status = ExnOpenFcb(Ctx, Vcb, Fcb, DesiredAccess, ShareAccess, CreateOptions, &OpenName);
    }

    if (!NT_SUCCESS(Status)) {
        if (Fcb != NULL) {
            Fcb->DeleteParentIno = Parent->Ino;
            Fcb->DeleteNameLength = NameLength;
            RtlCopyMemory(Fcb->DeleteName, Name, NameLength);
            Fcb->DeleteName[NameLength] = 0;
            (VOID)ExnUnlinkName(Vcb, Fcb);
            *Created = Fcb;
        } else {
            ExnEngineEnter(Vcb);
            (VOID)ExnEngineLeave(Vcb, ExtUnlink(&Vcb->Vol, Parent->Ino, &Parent->Inode,
                                                (const char *)Name, NameLength));
            ExnIndexRemove(Parent, Name, NameLength);
        }
        return Status;
    }

    *Created = Fcb;
    ExnInodeToFcb(Vcb, Parent);

    ExnNotifyChange(Vcb, Fcb,
                     IsDirectory ? FILE_NOTIFY_CHANGE_DIR_NAME : FILE_NOTIFY_CHANGE_FILE_NAME,
                     FILE_ACTION_ADDED);

    Ctx->Irp->IoStatus.Information = FILE_CREATED;
    return STATUS_SUCCESS;
}

/* SL_OPEN_TARGET_DIRECTORY: open Parent and leave only the last component */
static NTSTATUS
ExnOpenTargetDirectory (
    PEXN_IRP_CONTEXT Ctx,
    PEXN_VCB Vcb,
    PEXN_FCB Parent,
    PUNICODE_STRING Name,
    BOOLEAN CaseSensitive,
    ACCESS_MASK DesiredAccess,
    USHORT ShareAccess
    )
{
    PFILE_OBJECT FileObject = Ctx->IrpSp->FileObject;
    EXT_ENTRY Entry;
    BOOLEAN Exists;
    NTSTATUS Status;

    if (Vcb->VcbState & VCB_STATE_READ_ONLY) {
        return STATUS_MEDIA_WRITE_PROTECTED;
    }

    Status = ExnLookupName(Vcb, Parent, Name, CaseSensitive, &Entry);
    if (!NT_SUCCESS(Status) && Status != STATUS_OBJECT_NAME_NOT_FOUND) {
        return Status;
    }
    Exists = (BOOLEAN)NT_SUCCESS(Status);

    Status = ExnOpenFcb(Ctx, Vcb, Parent, DesiredAccess, ShareAccess, 0, NULL);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    RtlMoveMemory(FileObject->FileName.Buffer, Name->Buffer, Name->Length);
    FileObject->FileName.Length = Name->Length;

    Ctx->Irp->IoStatus.Information = Exists ? FILE_EXISTS : FILE_DOES_NOT_EXIST;
    return STATUS_SUCCESS;
}

/* Is a looked-up entry a symbolic link? */
static BOOLEAN
ExnEntryIsLink (
    PEXN_VCB Vcb,
    PEXT_ENTRY Entry
    )
{
    EXT_INODE Inode;
    NTSTATUS Status;

    if (Vcb->Vol.HasFiletype && Entry->FileType != EXT_FT_UNKNOWN) {
        return (BOOLEAN)(Entry->FileType == EXT_FT_SYMLINK);
    }
    ExnEngineEnter(Vcb);
    Status = ExnEngineLeave(Vcb, ExtReadInode(&Vcb->Vol, Entry->Inode, &Inode));
    return (BOOLEAN)(NT_SUCCESS(Status) && (Inode.Mode & EXT_S_IFMT) == EXT_S_IFLNK);
}

/*
 * Replaces the path still to walk by a symbolic link's target followed by
 * that rest. The target's '/' become '\'; *Absolute is set for a target
 * that starts at the root. The new path lives in *Buffer (freed by the
 * caller, and here when replaced).
 */
static NTSTATUS
ExnSpliceLink (
    PEXN_VCB Vcb,
    ULONG LinkIno,
    PUNICODE_STRING Remaining,
    PWSTR *Buffer,
    PBOOLEAN Absolute
    )
{
    EXT_INODE Inode;
    UCHAR *Target;
    ULONG TLen = 0, i, Start, Out = 0, Cap;
    PWSTR New;
    NTSTATUS Status;

    Target = (UCHAR *)ExAllocatePoolWithTag(PagedPool, 4096, EXN_TAG_NAME);
    if (Target == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    ExnEngineEnter(Vcb);
    Status = ExtReadInode(&Vcb->Vol, LinkIno, &Inode);
    if (NT_SUCCESS(Status)) {
        Status = ExtReadLink(&Vcb->Vol, LinkIno, &Inode, (char *)Target, 4096, &TLen);
    }
    Status = ExnEngineLeave(Vcb, Status);
    if (!NT_SUCCESS(Status) || TLen == 0) {
        ExFreePool(Target);
        return NT_SUCCESS(Status) ? STATUS_OBJECT_PATH_NOT_FOUND : Status;
    }

    Cap = (TLen * 2 + 2) * sizeof(WCHAR) + Remaining->Length;
    if (Cap > 0xFFF0) {
        ExFreePool(Target);
        return STATUS_NAME_TOO_LONG;
    }
    New = (PWSTR)ExAllocatePoolWithTag(PagedPool, Cap, EXN_TAG_NAME);
    if (New == NULL) {
        ExFreePool(Target);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    *Absolute = (BOOLEAN)(Target[0] == '/');
    for (i = 0; i <= TLen; ) {
        UNICODE_STRING Part;
        for (Start = i; i < TLen && Target[i] != '/'; i++) {
            ;
        }
        if (i > Start) {
            Status = ExnExtToUnicode(Target + Start, i - Start, &Part);
            if (!NT_SUCCESS(Status)) {
                ExFreePool(New);
                ExFreePool(Target);
                return Status;
            }
            if (Out != 0) {
                New[Out++] = L'\\';
            }
            RtlCopyMemory(New + Out, Part.Buffer, Part.Length);
            Out += Part.Length / sizeof(WCHAR);
            ExFreePool(Part.Buffer);
        }
        i++;
    }
    ExFreePool(Target);

    if (Remaining->Length != 0) {
        if (Out != 0) {
            New[Out++] = L'\\';
        }
        RtlCopyMemory(New + Out, Remaining->Buffer, Remaining->Length);
        Out += Remaining->Length / sizeof(WCHAR);
    }

    if (*Buffer != NULL) {
        ExFreePool(*Buffer);       /* Remaining pointed into it: copied above */
    }
    *Buffer = New;
    Remaining->Buffer = New;
    Remaining->Length = (USHORT)(Out * sizeof(WCHAR));
    Remaining->MaximumLength = Remaining->Length;
    return STATUS_SUCCESS;
}

NTSTATUS
ExnCommonCreate (
    PEXN_IRP_CONTEXT Ctx
    )
{
    PEXN_VCB Vcb = Ctx->Vcb;
    PIO_STACK_LOCATION IrpSp = Ctx->IrpSp;
    PFILE_OBJECT FileObject = IrpSp->FileObject;
    PFILE_OBJECT RelatedFileObject = FileObject->RelatedFileObject;
    PEXN_FCB RelatedFcb = NULL;
    PEXN_FCB Current = NULL;
    PEXN_FCB Parent = NULL;
    PEXN_FCB Child;
    PEXN_FCB Created;
    UNICODE_STRING Remaining;
    UNICODE_STRING Component;
    ULONG CreateDisposition;
    ULONG CreateOptions;
    ACCESS_MASK DesiredAccess;
    ACCESS_MASK ShareCheckAccess;
    USHORT ShareAccess;
    BOOLEAN TrailingBackslash = FALSE;
    BOOLEAN OpenTarget;
    BOOLEAN Overwrite;
    BOOLEAN LastComponent;
    BOOLEAN Opened = FALSE;
    BOOLEAN CaseSensitive;
    BOOLEAN AsLink;
    PWSTR LinkPath = NULL;
    ULONG LinkDepth = 0;
    EXT_ENTRY Entry;
    EXN_OPEN_NAME OpenName;
    UCHAR Name[EXN_MAX_NAME + 1];
    ULONG NameLength;
    NTSTATUS Status = STATUS_SUCCESS;

    CreateDisposition = (IrpSp->Parameters.Create.Options >> 24) & 0xFF;
    CreateOptions = IrpSp->Parameters.Create.Options & 0x00FFFFFF;
    ShareAccess = IrpSp->Parameters.Create.ShareAccess;
    OpenTarget = (BOOLEAN)((IrpSp->Flags & SL_OPEN_TARGET_DIRECTORY) != 0);
    CaseSensitive = (BOOLEAN)(ExnData.CaseSensitive || (IrpSp->Flags & SL_CASE_SENSITIVE));
    OpenName.Length = 0;

    Remaining = FileObject->FileName;

    if (RelatedFileObject != NULL) {
        RelatedFcb = (PEXN_FCB)RelatedFileObject->FsContext;
    }

    (VOID)ExAcquireResourceExclusiveLite(&Vcb->Resource, TRUE);

    __try {

        Status = ExnVerifyVcb(Ctx, Vcb);
        if (!NT_SUCCESS(Status)) {
            __leave;
        }

        if (Vcb->VcbState & (VCB_STATE_LOCKED | VCB_STATE_PNP_LOCKED)) {
            Status = STATUS_ACCESS_DENIED;
            __leave;
        }

        if (IrpSp->Flags & SL_OPEN_PAGING_FILE) {

            if (CreateOptions & FILE_DIRECTORY_FILE) {
                Status = STATUS_ACCESS_DENIED;
                __leave;
            }

            /* An existing paging file is opened as it is (Mm sizes it); a
               missing one is still created */
            if (CreateDisposition == FILE_SUPERSEDE || CreateDisposition == FILE_OVERWRITE_IF ||
                CreateDisposition == FILE_OPEN_IF) {
                CreateDisposition = FILE_OPEN_IF;
            } else if (CreateDisposition == FILE_OVERWRITE) {
                CreateDisposition = FILE_OPEN;
            }
        }

        if (CreateOptions & FILE_OPEN_BY_FILE_ID) {
            Status = STATUS_INVALID_PARAMETER;
            __leave;
        }

        if (CreateDisposition > FILE_OVERWRITE_IF ||
            ((CreateOptions & FILE_DIRECTORY_FILE) && (CreateOptions & FILE_NON_DIRECTORY_FILE))) {
            Status = STATUS_INVALID_PARAMETER;
            __leave;
        }

        /* The volume itself */
        if (Remaining.Length == 0 && (RelatedFcb == NULL || ExnIsVfcb(RelatedFcb))) {

            if (OpenTarget) {
                Status = STATUS_INVALID_PARAMETER;
                __leave;
            }

            DesiredAccess = ExnResolveAccess(IrpSp, Vcb, NULL);
            Status = ExnOpenVolume(Ctx, Vcb, DesiredAccess, ShareAccess,
                                   CreateDisposition, CreateOptions);
            __leave;
        }

        if (RelatedFcb != NULL) {

            if (Remaining.Length != 0 && Remaining.Buffer[0] == L'\\') {
                Status = STATUS_OBJECT_NAME_INVALID;
                __leave;
            }

            if (ExnIsVfcb(RelatedFcb)) {
                Current = Vcb->RootDcb;
            } else if (ExnIsFcb(RelatedFcb) && Remaining.Length != 0) {
                Status = STATUS_OBJECT_PATH_NOT_FOUND;
                __leave;
            } else {
                Current = RelatedFcb;
            }

        } else {

            if (Remaining.Buffer[0] != L'\\') {
                Status = STATUS_OBJECT_NAME_INVALID;
                __leave;
            }

            Remaining.Buffer++;
            Remaining.Length -= sizeof(WCHAR);
            Current = Vcb->RootDcb;
        }

        if (Remaining.Length != 0 &&
            Remaining.Buffer[Remaining.Length / sizeof(WCHAR) - 1] == L'\\') {

            TrailingBackslash = TRUE;
            Remaining.Length -= sizeof(WCHAR);

            if (Remaining.Length == 0 ||
                Remaining.Buffer[Remaining.Length / sizeof(WCHAR) - 1] == L'\\') {
                Status = STATUS_OBJECT_NAME_INVALID;
                __leave;
            }
        }

        Remaining.MaximumLength = Remaining.Length;

        if (Remaining.Length == 0 && OpenTarget) {
            Status = STATUS_INVALID_PARAMETER;
            __leave;
        }

        /* Walk the path */
        while (ExnNextComponent(&Remaining, &Component)) {

            LastComponent = (BOOLEAN)(Remaining.Length == 0);

            if (Component.Length == 0 ||
                (Component.Length == sizeof(WCHAR) && Component.Buffer[0] == L'.')) {
                continue;                       /* "a\\b" or "." from a link target */
            }
            if (Component.Length == 2 * sizeof(WCHAR) && Component.Buffer[0] == L'.' &&
                Component.Buffer[1] == L'.') {
                if (LinkDepth == 0) {
                    Status = STATUS_OBJECT_NAME_INVALID;
                    __leave;
                }
                if (Current->ParentDcb != NULL) {
                    /* not dropped: freeing an unopened child can free its
                       parent too; it stays cached until it is next opened */
                    Current = Current->ParentDcb;
                }
                if (LastComponent) {
                    break;
                }
                continue;
            }

            if (!ExnIsValidUnicodeName(&Component)) {
                Status = STATUS_OBJECT_NAME_INVALID;
                __leave;
            }

            Status = ExnUnicodeToExt(&Component, Name, &NameLength);
            if (!NT_SUCCESS(Status)) {
                Status = STATUS_OBJECT_NAME_INVALID;
                __leave;
            }

            if (!ExnIsDcb(Current)) {
                Status = STATUS_OBJECT_PATH_NOT_FOUND;
                __leave;
            }

            if (LastComponent && OpenTarget) {

                DesiredAccess = ExnResolveAccess(IrpSp, Vcb, Current);
                Status = ExnOpenTargetDirectory(Ctx, Vcb, Current, &Component, CaseSensitive,
                                                DesiredAccess, ShareAccess);
                if (NT_SUCCESS(Status)) {
                    Opened = TRUE;
                }
                __leave;
            }

            Status = ExnLookupName(Vcb, Current, &Component, CaseSensitive, &Entry);

            if (Status == STATUS_OBJECT_NAME_NOT_FOUND) {

                if (!LastComponent) {
                    Status = STATUS_OBJECT_PATH_NOT_FOUND;
                    __leave;
                }

                if (CreateDisposition == FILE_OPEN || CreateDisposition == FILE_OVERWRITE) {
                    __leave;
                }

                Status = ExnCreateNew(Ctx, Vcb, Current, Name, NameLength, CreateDisposition,
                                      CreateOptions, ShareAccess, TrailingBackslash, &Created);

                if (NT_SUCCESS(Status)) {
                    Opened = TRUE;
                } else if (Created != NULL) {
                    Current = Created;
                }
                __leave;
            }

            if (!NT_SUCCESS(Status)) {
                __leave;
            }

            /* A symbolic link: follow it, unless the caller wants the link */
            AsLink = FALSE;
            if (ExnEntryIsLink(Vcb, &Entry)) {
                if (LastComponent &&
                    ((CreateOptions & FILE_OPEN_REPARSE_POINT) ||
                     (IrpSp->Parameters.Create.SecurityContext->DesiredAccess & DELETE) ||
                     !ExnData.FollowLinks)) {
                    AsLink = TRUE;
                } else if (ExnData.FollowLinks) {
                    BOOLEAN Absolute;
                    if (++LinkDepth > EXN_MAX_LINK_DEPTH) {
                        Status = STATUS_OBJECT_PATH_NOT_FOUND;
                        __leave;
                    }
                    Status = ExnSpliceLink(Vcb, Entry.Inode, &Remaining, &LinkPath, &Absolute);
                    if (!NT_SUCCESS(Status)) {
                        __leave;
                    }
                    if (Absolute) {
                        ExnDropUnopened(Vcb, Current);
                        Current = Vcb->RootDcb;
                    }
                    continue;
                }
            }

            Child = ExnFindFcbByIno(Vcb, Entry.Inode, AsLink);

            if (Child == NULL) {
                Status = ExnCreateFcb(Vcb, Current, &Entry, AsLink, &Child);
                if (!NT_SUCCESS(Status)) {
                    __leave;
                }
            }

            if (LastComponent) {
                OpenName.ParentIno = Current->Ino;
                OpenName.Length = Entry.NameLength;
                RtlCopyMemory(OpenName.Name, Entry.Name, Entry.NameLength);
            }
            Parent = Current;
            Current = Child;
        }

        /* Current exists */
        if (CreateDisposition == FILE_CREATE) {
            Status = STATUS_OBJECT_NAME_COLLISION;
            __leave;
        }

        if (ExnIsDcb(Current)) {

            if (CreateOptions & FILE_NON_DIRECTORY_FILE) {
                Status = STATUS_FILE_IS_A_DIRECTORY;
                __leave;
            }
            if (CreateDisposition != FILE_OPEN && CreateDisposition != FILE_OPEN_IF) {
                Status = STATUS_OBJECT_NAME_COLLISION;
                __leave;
            }

        } else {

            if (CreateOptions & FILE_DIRECTORY_FILE) {
                Status = STATUS_NOT_A_DIRECTORY;
                __leave;
            }
            if (TrailingBackslash) {
                Status = STATUS_OBJECT_NAME_INVALID;
                __leave;
            }
        }

        if (Current->FcbState & FCB_STATE_DELETE_PENDING) {
            Status = STATUS_DELETE_PENDING;
            __leave;
        }

        DesiredAccess = ExnResolveAccess(IrpSp, Vcb, Current);
        ShareCheckAccess = DesiredAccess;

        Overwrite = (BOOLEAN)(CreateDisposition == FILE_SUPERSEDE ||
                              CreateDisposition == FILE_OVERWRITE ||
                              CreateDisposition == FILE_OVERWRITE_IF);

        if (Overwrite) {

            if (Current->FcbState & FCB_STATE_SYMLINK) {
                Status = STATUS_ACCESS_DENIED;
                __leave;
            }

            ShareCheckAccess |= FILE_WRITE_DATA;

            if (((Current->Attributes & FILE_ATTRIBUTE_HIDDEN) &&
                 !(IrpSp->Parameters.Create.FileAttributes & FILE_ATTRIBUTE_HIDDEN)) ||
                ((Current->Attributes & FILE_ATTRIBUTE_SYSTEM) &&
                 !(IrpSp->Parameters.Create.FileAttributes & FILE_ATTRIBUTE_SYSTEM))) {
                Status = STATUS_ACCESS_DENIED;
                __leave;
            }
        }

        if ((ShareCheckAccess & EXN_WRITE_ACCESS) || (CreateOptions & FILE_DELETE_ON_CLOSE)) {
            if (Vcb->VcbState & VCB_STATE_READ_ONLY) {
                Status = STATUS_MEDIA_WRITE_PROTECTED;
                __leave;
            }
        }

        if (ExnIsFcb(Current) && (Current->Attributes & FILE_ATTRIBUTE_READONLY)) {
            if (ShareCheckAccess & EXN_DATA_WRITE_ACCESS) {
                Status = STATUS_ACCESS_DENIED;
                __leave;
            }
            if (CreateOptions & FILE_DELETE_ON_CLOSE) {
                Status = STATUS_CANNOT_DELETE;
                __leave;
            }
        }

        if ((CreateOptions & FILE_DELETE_ON_CLOSE) && (Current->FcbState & FCB_STATE_ROOT)) {
            Status = STATUS_CANNOT_DELETE;
            __leave;
        }

        Status = ExnOpenFcb(Ctx, Vcb, Current, ShareCheckAccess, ShareAccess, CreateOptions,
                            OpenName.Length ? &OpenName : NULL);
        if (!NT_SUCCESS(Status)) {
            __leave;
        }

        if ((IrpSp->Flags & SL_OPEN_PAGING_FILE) && Current->Runs == NULL) {
            /* every block of a paging file is there, mapped in nonpaged pool */
            Status = ExnLoadRuns(Vcb, Current);
            if (!NT_SUCCESS(Status)) {
                ExnUndoOpen(Ctx, Vcb, Current);
                __leave;
            }
        }

        if (Overwrite) {

            Status = ExnOverwriteFile(Ctx, Vcb, Current, CreateDisposition);

            if (!NT_SUCCESS(Status)) {
                ExnUndoOpen(Ctx, Vcb, Current);
                __leave;
            }

            Ctx->Irp->IoStatus.Information =
                (CreateDisposition == FILE_SUPERSEDE) ? FILE_SUPERSEDED : FILE_OVERWRITTEN;
        }

        Opened = TRUE;

    } __finally {

        if (!Opened || AbnormalTermination()) {
            if (Current != NULL && Current != RelatedFcb) {
                ExnDropUnopened(Vcb, Current);
            }
        }

        if (LinkPath != NULL) {
            ExFreePool(LinkPath);
        }

        ExnRelease(&Vcb->Resource);
    }

    UNREFERENCED_PARAMETER(Parent);
    return Status;
}
