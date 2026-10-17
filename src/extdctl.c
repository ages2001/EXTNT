/*
 * EXTNT - IRP_MJ_DIRECTORY_CONTROL, and name lookups
 *
 * An enumeration walks the directory's entries in their on-disk order; a
 * handle remembers the byte position after the last entry it returned (the
 * engine's cookie). Entries added or removed in between are seen or not as
 * their position falls. "." and ".." come first (not in the root), as NT
 * expects.
 *
 * Lookups try the exact name (through the htree index when the directory
 * has one), then, unless the caller asked for case sensitivity, any name
 * equal to it without regard to case: small directories are scanned; a big
 * one gets an in-memory index of its upcased names, built on the first such
 * miss and kept up to date as names come and go.
 */

#include "ext.h"

/* ------------------------------------------------------------------ */
/* Case-insensitive name index                                         */
/* ------------------------------------------------------------------ */

#define EXN_INDEX_MIN_BLOCKS    4       /* smaller directories are just scanned */

typedef struct _EXN_NAME_NODE {
    struct _EXN_NAME_NODE *Next;
    ULONG       Hash;
    ULONG       Ino;
    UCHAR       FileType;
    UCHAR       Length;
    UCHAR       Name[1];
} EXN_NAME_NODE, *PEXN_NAME_NODE;

struct _EXN_NAME_INDEX {
    ULONG           Buckets;            /* a power of two */
    ULONG           Count;
    PEXN_NAME_NODE  Table[1];
};

/* FNV-1a over the upcased UTF-16 name */
static ULONG
ExnNameHash (
    PCUNICODE_STRING Name
    )
{
    ULONG H = 2166136261u, i;
    for (i = 0; i < Name->Length / sizeof(WCHAR); i++) {
        WCHAR C = RtlUpcaseUnicodeChar(Name->Buffer[i]);
        H = (H ^ (C & 0xFF)) * 16777619u;
        H = (H ^ (C >> 8)) * 16777619u;
    }
    return H;
}

VOID
ExnIndexFree (
    PEXN_FCB Dir
    )
{
    PEXN_NAME_INDEX X = Dir->NameIndex;
    ULONG i;
    if (X == NULL) {
        return;
    }
    for (i = 0; i < X->Buckets; i++) {
        while (X->Table[i] != NULL) {
            PEXN_NAME_NODE N = X->Table[i];
            X->Table[i] = N->Next;
            ExFreePool(N);
        }
    }
    ExFreePool(X);
    Dir->NameIndex = NULL;
}

static VOID
ExnIndexInsert (
    PEXN_NAME_INDEX X,
    const UCHAR *Name,
    ULONG Length,
    ULONG Ino,
    UCHAR FileType,
    ULONG Hash
    )
{
    PEXN_NAME_NODE N = (PEXN_NAME_NODE)ExAllocatePoolWithTag(PagedPool,
                            FIELD_OFFSET(EXN_NAME_NODE, Name) + Length + 1, EXN_TAG_INDEX);
    if (N == NULL) {
        return;
    }
    N->Hash = Hash;
    N->Ino = Ino;
    N->FileType = FileType;
    N->Length = (UCHAR)Length;
    RtlCopyMemory(N->Name, Name, Length);
    N->Name[Length] = 0;
    N->Next = X->Table[Hash & (X->Buckets - 1)];
    X->Table[Hash & (X->Buckets - 1)] = N;
    X->Count++;
}

VOID
ExnIndexAdd (
    PEXN_FCB Dir,
    const UCHAR *Name,
    ULONG Length,
    ULONG Ino
    )
{
    UNICODE_STRING U;
    if (Dir->NameIndex == NULL || !NT_SUCCESS(ExnExtToUnicode(Name, Length, &U))) {
        return;
    }
    ExnIndexInsert(Dir->NameIndex, Name, Length, Ino, EXT_FT_UNKNOWN, ExnNameHash(&U));
    ExFreePool(U.Buffer);
}

VOID
ExnIndexRemove (
    PEXN_FCB Dir,
    const UCHAR *Name,
    ULONG Length
    )
{
    UNICODE_STRING U;
    PEXN_NAME_NODE *P;
    ULONG Hash;
    if (Dir->NameIndex == NULL || !NT_SUCCESS(ExnExtToUnicode(Name, Length, &U))) {
        return;
    }
    Hash = ExnNameHash(&U);
    ExFreePool(U.Buffer);
    for (P = &Dir->NameIndex->Table[Hash & (Dir->NameIndex->Buckets - 1)]; *P != NULL; P = &(*P)->Next) {
        if ((*P)->Length == Length && RtlCompareMemory((*P)->Name, Name, Length) == Length) {
            PEXN_NAME_NODE N = *P;
            *P = N->Next;
            ExFreePool(N);
            Dir->NameIndex->Count--;
            return;
        }
    }
}

/* Reads every name of the directory into a new index (engine not held) */
static NTSTATUS
ExnIndexBuild (
    PEXN_VCB Vcb,
    PEXN_FCB Dir
    )
{
    ULONGLONG Blocks = ExtInodeSize(&Vcb->Vol, &Dir->Inode) >> Vcb->Vol.BlockShift;
    ULONG Buckets = 256, Size;
    PEXN_NAME_INDEX X;
    PEXT_ENTRY E;
    ULONGLONG Cookie = 0;
    NTSTATUS Status = STATUS_SUCCESS;

    while ((ULONGLONG)Buckets < Blocks * 16 && Buckets < 65536) {
        Buckets <<= 1;
    }
    Size = FIELD_OFFSET(EXN_NAME_INDEX, Table) + Buckets * sizeof(PEXN_NAME_NODE);
    X = (PEXN_NAME_INDEX)ExAllocatePoolWithTag(PagedPool, Size, EXN_TAG_INDEX);
    E = (PEXT_ENTRY)ExAllocatePoolWithTag(PagedPool, sizeof(EXT_ENTRY), EXN_TAG_BUFFER);
    if (X == NULL || E == NULL) {
        if (X) ExFreePool(X);
        if (E) ExFreePool(E);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(X, Size);
    X->Buckets = Buckets;

    for (;;) {
        UNICODE_STRING U;
        ExnEngineEnter(Vcb);
        Status = ExnEngineLeave(Vcb, ExtEnum(&Vcb->Vol, Dir->Ino, &Dir->Inode, Cookie, E));
        if (Status == STATUS_NO_MORE_ENTRIES) {
            Status = STATUS_SUCCESS;
            break;
        }
        if (!NT_SUCCESS(Status)) {
            break;
        }
        Cookie = E->Cookie;
        if (NT_SUCCESS(ExnExtToUnicode((const UCHAR *)E->Name, E->NameLength, &U))) {
            ExnIndexInsert(X, (const UCHAR *)E->Name, E->NameLength, E->Inode, E->FileType, ExnNameHash(&U));
            ExFreePool(U.Buffer);
        }
    }
    ExFreePool(E);
    if (!NT_SUCCESS(Status)) {
        Dir->NameIndex = X;
        ExnIndexFree(Dir);
        return Status;
    }
    Dir->NameIndex = X;
    return STATUS_SUCCESS;
}

/*
 * Looks Name up in directory Dir: the exact name first, then (unless
 * CaseSensitive) a name equal to it without regard to case. Entry gets the
 * name as stored. STATUS_OBJECT_NAME_NOT_FOUND when there is none.
 */
NTSTATUS
ExnLookupName (
    PEXN_VCB Vcb,
    PEXN_FCB Dir,
    PCUNICODE_STRING Name,
    BOOLEAN CaseSensitive,
    PEXT_ENTRY Entry
    )
{
    UCHAR Bytes[EXN_MAX_NAME + 1];
    ULONG Length;
    NTSTATUS Status;

    Status = ExnUnicodeToExt(Name, Bytes, &Length);
    if (!NT_SUCCESS(Status)) {
        return STATUS_OBJECT_NAME_INVALID;
    }

    ExnEngineEnter(Vcb);
    Status = ExnEngineLeave(Vcb, ExtLookup(&Vcb->Vol, Dir->Ino, &Dir->Inode, (const char *)Bytes, Length, Entry));
    if (Status != EXT_NOT_FOUND) {
        return Status;
    }
    if (CaseSensitive) {
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }

    if ((ExtInodeSize(&Vcb->Vol, &Dir->Inode) >> Vcb->Vol.BlockShift) < EXN_INDEX_MIN_BLOCKS) {

        /* small: scan */
        ULONGLONG Cookie = 0;
        for (;;) {
            UNICODE_STRING U;
            BOOLEAN Equal;
            ExnEngineEnter(Vcb);
            Status = ExnEngineLeave(Vcb, ExtEnum(&Vcb->Vol, Dir->Ino, &Dir->Inode, Cookie, Entry));
            if (Status == STATUS_NO_MORE_ENTRIES) {
                return STATUS_OBJECT_NAME_NOT_FOUND;
            }
            if (!NT_SUCCESS(Status)) {
                return Status;
            }
            Cookie = Entry->Cookie;
            if (!NT_SUCCESS(ExnExtToUnicode((const UCHAR *)Entry->Name, Entry->NameLength, &U))) {
                continue;
            }
            Equal = RtlEqualUnicodeString(&U, Name, TRUE);
            ExFreePool(U.Buffer);
            if (Equal) {
                return STATUS_SUCCESS;
            }
        }
    }

    if (Dir->NameIndex == NULL) {
        Status = ExnIndexBuild(Vcb, Dir);
        if (!NT_SUCCESS(Status)) {
            return Status;
        }
    }
    {
        ULONG Hash = ExnNameHash(Name);
        PEXN_NAME_NODE N;
        for (N = Dir->NameIndex->Table[Hash & (Dir->NameIndex->Buckets - 1)]; N != NULL; N = N->Next) {
            UNICODE_STRING U;
            BOOLEAN Equal;
            if (N->Hash != Hash || !NT_SUCCESS(ExnExtToUnicode(N->Name, N->Length, &U))) {
                continue;
            }
            Equal = RtlEqualUnicodeString(&U, Name, TRUE);
            ExFreePool(U.Buffer);
            if (Equal) {
                /* confirm on the disk (and get the stored file type) */
                ExnEngineEnter(Vcb);
                Status = ExnEngineLeave(Vcb, ExtLookup(&Vcb->Vol, Dir->Ino, &Dir->Inode,
                                                       (const char *)N->Name, N->Length, Entry));
                return (Status == EXT_NOT_FOUND) ? STATUS_OBJECT_NAME_NOT_FOUND : Status;
            }
        }
    }
    return STATUS_OBJECT_NAME_NOT_FOUND;
}

/* ------------------------------------------------------------------ */
/* Symbolic links in listings                                          */
/* ------------------------------------------------------------------ */

/*
 * The inode a symbolic link (in directory DirIno) ends at, walking its
 * target with exact names; FALSE when it does not resolve. Engine held.
 */
static BOOLEAN
ExnResolveLink (
    PEXN_VCB Vcb,
    ULONG DirIno,
    ULONG LinkIno,
    PULONG TargetIno,
    PEXT_INODE Target
    )
{
    UCHAR *Path;
    ULONG Len = 0, Hops = 0, Ino = LinkIno, Dir = DirIno;
    EXT_INODE Inode, DirInode;
    BOOLEAN Ok = FALSE;

    Path = (UCHAR *)ExAllocatePoolWithTag(PagedPool, 4096, EXN_TAG_NAME);
    if (Path == NULL) {
        return FALSE;
    }
    if (!NT_SUCCESS(ExtReadInode(&Vcb->Vol, Ino, &Inode))) {
        goto out;
    }
    while ((Inode.Mode & EXT_S_IFMT) == EXT_S_IFLNK) {
        ULONG i = 0, Start;
        if (++Hops > EXN_MAX_LINK_DEPTH ||
            !NT_SUCCESS(ExtReadLink(&Vcb->Vol, Ino, &Inode, (char *)Path, 4096, &Len)) || Len == 0) {
            goto out;
        }
        if (Path[0] == '/') {
            Dir = EXT_ROOT_INO;
        }
        Ino = Dir;
        while (i < Len) {
            EXT_ENTRY E;
            while (i < Len && Path[i] == '/') i++;
            for (Start = i; i < Len && Path[i] != '/'; i++) { ; }
            if (i == Start) break;
            if (!NT_SUCCESS(ExtReadInode(&Vcb->Vol, Ino, &DirInode)) ||
                (DirInode.Mode & EXT_S_IFMT) != EXT_S_IFDIR) {
                goto out;
            }
            if (i - Start == 1 && Path[Start] == '.') {
                continue;
            }
            if (i - Start == 2 && Path[Start] == '.' && Path[Start + 1] == '.') {
                if (!NT_SUCCESS(ExtGetParent(&Vcb->Vol, Ino, &DirInode, &Ino))) goto out;
                continue;
            }
            if (!NT_SUCCESS(ExtLookup(&Vcb->Vol, Ino, &DirInode, (const char *)Path + Start, i - Start, &E))) {
                goto out;
            }
            Dir = Ino;
            Ino = E.Inode;
            if (!NT_SUCCESS(ExtReadInode(&Vcb->Vol, Ino, &Inode))) goto out;
            if ((Inode.Mode & EXT_S_IFMT) == EXT_S_IFLNK && i < Len) {
                break;                  /* a link inside the path: resolve it first */
            }
        }
        if (!NT_SUCCESS(ExtReadInode(&Vcb->Vol, Ino, &Inode))) goto out;
        if ((Inode.Mode & EXT_S_IFMT) == EXT_S_IFLNK && i < Len) {
            goto out;                   /* nested mid-path links: not followed in listings */
        }
    }
    *TargetIno = Ino;
    *Target = Inode;
    Ok = TRUE;
out:
    ExFreePool(Path);
    return Ok;
}

/* ------------------------------------------------------------------ */
/* IRP_MN_QUERY_DIRECTORY                                              */
/* ------------------------------------------------------------------ */

typedef struct _EXN_LIST_ENTRY {
    LARGE_INTEGER   CreationTime;
    LARGE_INTEGER   LastAccessTime;
    LARGE_INTEGER   LastWriteTime;
    LARGE_INTEGER   ChangeTime;
    LARGE_INTEGER   EndOfFile;
    LARGE_INTEGER   AllocationSize;
    ULONG           Attributes;
    ULONG           EaSize;
    ULONG           FileIndex;
    UNICODE_STRING  Name;
} EXN_LIST_ENTRY, *PEXN_LIST_ENTRY;

static VOID
ExnListFromDcb (
    PEXN_FCB Dcb,
    PEXN_LIST_ENTRY List,
    PWSTR Name,
    USHORT NameLength
    )
{
    RtlZeroMemory(List, sizeof(EXN_LIST_ENTRY));

    if (Dcb != NULL) {
        List->CreationTime = Dcb->CreationTime;
        List->LastAccessTime = Dcb->LastAccessTime;
        List->LastWriteTime = Dcb->LastWriteTime;
        List->ChangeTime = Dcb->ChangeTime;
    }

    List->Attributes = FILE_ATTRIBUTE_DIRECTORY;
    List->Name.Buffer = Name;
    List->Name.Length = NameLength;
    List->Name.MaximumLength = NameLength;
}

static VOID
ExnListFromInode (
    PEXN_VCB Vcb,
    PEXT_INODE I,
    PEXN_LIST_ENTRY List
    )
{
    LONGLONG Size = (LONGLONG)ExtInodeSize(&Vcb->Vol, I);
    BOOLEAN Big = (BOOLEAN)(Vcb->Vol.InodeSize > EXT_GOOD_OLD_INODE_SIZE);
    ULONG Extra = Big ? I->ExtraIsize : 0;

    List->LastWriteTime = ExnToNtTime(I->Mtime, Extra >= 12 ? I->MtimeExtra : 0);
    List->LastAccessTime = ExnToNtTime(I->Atime, Extra >= 16 ? I->AtimeExtra : 0);
    List->ChangeTime = ExnToNtTime(I->Ctime, Extra >= 8 ? I->CtimeExtra : 0);
    if (Extra >= 20 && I->Crtime != 0) {
        List->CreationTime = ExnToNtTime(I->Crtime, Extra >= 24 ? I->CrtimeExtra : 0);
    } else {
        List->CreationTime = List->LastWriteTime.QuadPart < List->ChangeTime.QuadPart ?
                             List->LastWriteTime : List->ChangeTime;
    }
    if ((I->Mode & EXT_S_IFMT) != EXT_S_IFDIR) {
        List->EndOfFile.QuadPart = Size;
        List->AllocationSize.QuadPart = (LONGLONG)ExtInodeBlocks(&Vcb->Vol, I) << EXN_SECTOR_SHIFT;
    }
}

static VOID
ExnListFromEntry (
    PEXN_VCB Vcb,
    PEXN_FCB Dir,
    PEXT_ENTRY Entry,
    PEXN_LIST_ENTRY List,
    BOOLEAN WantEaSize
    )
{
    PEXN_FCB Fcb;
    EXT_INODE Inode;
    ULONG Ino = Entry->Inode, Dos = 0;

    RtlZeroMemory(List, FIELD_OFFSET(EXN_LIST_ENTRY, Name));
    List->FileIndex = Entry->Inode;

    /* An open file's inode may lag behind its FCB */
    Fcb = ExnFindFcbByIno(Vcb, Ino, FALSE);
    if (Fcb != NULL && !(Fcb->FcbState & FCB_STATE_SYMLINK)) {
        List->CreationTime = Fcb->CreationTime;
        List->LastAccessTime = Fcb->LastAccessTime;
        List->LastWriteTime = Fcb->LastWriteTime;
        List->ChangeTime = Fcb->ChangeTime;
        List->Attributes = ExnComputeAttributes(Vcb, &Fcb->Inode, (const UCHAR *)Entry->Name,
                                                Entry->NameLength, Fcb->DosAttributes);
        if (ExnIsFcb(Fcb)) {
            List->EndOfFile = Fcb->Header.FileSize;
            List->AllocationSize.QuadPart = (LONGLONG)ExtInodeBlocks(&Vcb->Vol, &Fcb->Inode) << EXN_SECTOR_SHIFT;
        }
        if (WantEaSize) {
            List->EaSize = ExnNtEaSize(Vcb, Fcb);
        }
        return;
    }

    ExnEngineEnter(Vcb);
    if (!NT_SUCCESS(ExtReadInode(&Vcb->Vol, Ino, &Inode))) {
        (VOID)ExnEngineLeave(Vcb, STATUS_SUCCESS);
        List->Attributes = FILE_ATTRIBUTE_SYSTEM;
        return;
    }
    if ((Inode.Mode & EXT_S_IFMT) == EXT_S_IFLNK && ExnData.FollowLinks) {
        /* shown as what it points to (a link to a directory is a directory) */
        ULONG TIno;
        EXT_INODE Target;
        if (ExnResolveLink(Vcb, Dir->Ino, Ino, &TIno, &Target)) {
            Ino = TIno;
            Inode = Target;
        }
    }
    Dos = ExnLoadDosAttributes(Vcb, Ino, &Inode);
    (VOID)ExnEngineLeave(Vcb, STATUS_SUCCESS);

    ExnListFromInode(Vcb, &Inode, List);
    List->Attributes = ExnComputeAttributes(Vcb, &Inode, (const UCHAR *)Entry->Name, Entry->NameLength, Dos);
    if (WantEaSize && (Inode.FileAclLo != 0 || Vcb->Vol.InodeSize > EXT_GOOD_OLD_INODE_SIZE)) {
        EXN_FCB Temp;
        RtlZeroMemory(&Temp, FIELD_OFFSET(EXN_FCB, Name));
        Temp.Ino = Ino;
        Temp.Inode = Inode;
        List->EaSize = ExnNtEaSize(Vcb, &Temp);
    }
}

static ULONG
ExnListBaseLength (
    FILE_INFORMATION_CLASS Class
    )
{
    switch (Class) {
    case FileDirectoryInformation:
        return FIELD_OFFSET(FILE_DIRECTORY_INFORMATION, FileName);
    case FileFullDirectoryInformation:
        return FIELD_OFFSET(FILE_FULL_DIR_INFORMATION, FileName);
    case FileBothDirectoryInformation:
        return FIELD_OFFSET(FILE_BOTH_DIR_INFORMATION, FileName);
    case FileNamesInformation:
        return FIELD_OFFSET(FILE_NAMES_INFORMATION, FileName);
    default:
        return 0;
    }
}

static ULONG
ExnFillListEntry (
    FILE_INFORMATION_CLASS Class,
    PUCHAR Buffer,
    ULONG Room,
    PEXN_LIST_ENTRY List
    )
{
    PFILE_DIRECTORY_INFORMATION Dir;
    PFILE_FULL_DIR_INFORMATION Full;
    PFILE_BOTH_DIR_INFORMATION Both;
    PFILE_NAMES_INFORMATION Names;
    ULONG Base = ExnListBaseLength(Class);
    ULONG Copy;

    RtlZeroMemory(Buffer, Base);

    Copy = List->Name.Length;
    if (Copy > Room - Base) {
        Copy = Room - Base;
    }

    switch (Class) {

    case FileDirectoryInformation:
        Dir = (PFILE_DIRECTORY_INFORMATION)Buffer;
        Dir->FileIndex = List->FileIndex;
        Dir->CreationTime = List->CreationTime;
        Dir->LastAccessTime = List->LastAccessTime;
        Dir->LastWriteTime = List->LastWriteTime;
        Dir->ChangeTime = List->ChangeTime;
        Dir->EndOfFile = List->EndOfFile;
        Dir->AllocationSize = List->AllocationSize;
        Dir->FileAttributes = List->Attributes;
        Dir->FileNameLength = List->Name.Length;
        RtlCopyMemory(Dir->FileName, List->Name.Buffer, Copy);
        break;

    case FileFullDirectoryInformation:
        Full = (PFILE_FULL_DIR_INFORMATION)Buffer;
        Full->FileIndex = List->FileIndex;
        Full->CreationTime = List->CreationTime;
        Full->LastAccessTime = List->LastAccessTime;
        Full->LastWriteTime = List->LastWriteTime;
        Full->ChangeTime = List->ChangeTime;
        Full->EndOfFile = List->EndOfFile;
        Full->AllocationSize = List->AllocationSize;
        Full->FileAttributes = List->Attributes;
        Full->EaSize = List->EaSize;
        Full->FileNameLength = List->Name.Length;
        RtlCopyMemory(Full->FileName, List->Name.Buffer, Copy);
        break;

    case FileBothDirectoryInformation:
        Both = (PFILE_BOTH_DIR_INFORMATION)Buffer;
        Both->FileIndex = List->FileIndex;
        Both->CreationTime = List->CreationTime;
        Both->LastAccessTime = List->LastAccessTime;
        Both->LastWriteTime = List->LastWriteTime;
        Both->ChangeTime = List->ChangeTime;
        Both->EndOfFile = List->EndOfFile;
        Both->AllocationSize = List->AllocationSize;
        Both->FileAttributes = List->Attributes;
        Both->EaSize = List->EaSize;
        Both->FileNameLength = List->Name.Length;
        Both->ShortNameLength = 0;
        RtlCopyMemory(Both->FileName, List->Name.Buffer, Copy);
        break;

    case FileNamesInformation:
        Names = (PFILE_NAMES_INFORMATION)Buffer;
        Names->FileIndex = List->FileIndex;
        Names->FileNameLength = List->Name.Length;
        RtlCopyMemory(Names->FileName, List->Name.Buffer, Copy);
        break;

    default:
        break;
    }

    return Copy;
}

static BOOLEAN
ExnListMatches (
    PEXN_CCB Ccb,
    PUNICODE_STRING Name
    )
{
    if (Ccb->Flags & CCB_FLAG_MATCH_ALL) {
        return TRUE;
    }

    if (Ccb->Flags & CCB_FLAG_WILDCARD) {
        return FsRtlIsNameInExpression(&Ccb->Pattern, Name, TRUE, NULL);
    }

    return RtlEqualUnicodeString(&Ccb->Pattern, Name, TRUE);
}

static NTSTATUS
ExnSetPattern (
    PEXN_CCB Ccb,
    PUNICODE_STRING FileName
    )
{
    UNICODE_STRING Pattern;
    NTSTATUS Status;

    if (Ccb->Pattern.Buffer != NULL) {
        ExFreePool(Ccb->Pattern.Buffer);
        RtlZeroMemory(&Ccb->Pattern, sizeof(UNICODE_STRING));
    }

    Ccb->Flags &= ~(CCB_FLAG_WILDCARD | CCB_FLAG_MATCH_ALL);

    if (FileName == NULL || FileName->Length == 0 ||
        (FileName->Length == sizeof(WCHAR) && FileName->Buffer[0] == L'*')) {

        Ccb->Flags |= CCB_FLAG_MATCH_ALL | CCB_FLAG_PATTERN_SET;
        return STATUS_SUCCESS;
    }

    Pattern.Buffer = (PWSTR)ExAllocatePoolWithTag(PagedPool, FileName->Length, EXN_TAG_NAME);
    if (Pattern.Buffer == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Pattern.Length = 0;
    Pattern.MaximumLength = FileName->Length;

    Status = RtlUpcaseUnicodeString(&Pattern, FileName, FALSE);
    if (!NT_SUCCESS(Status)) {
        ExFreePool(Pattern.Buffer);
        return Status;
    }

    Ccb->Pattern = Pattern;

    if (FsRtlDoesNameContainWildCards(&Ccb->Pattern)) {
        Ccb->Flags |= CCB_FLAG_WILDCARD;
    }

    Ccb->Flags |= CCB_FLAG_PATTERN_SET;
    return STATUS_SUCCESS;
}

static NTSTATUS
ExnQueryDirectory (
    PEXN_IRP_CONTEXT Ctx,
    PEXN_FCB Dcb,
    PEXN_CCB Ccb
    )
{
    PEXN_VCB Vcb = Dcb->Vcb;
    PIRP Irp = Ctx->Irp;
    PIO_STACK_LOCATION IrpSp = Ctx->IrpSp;
    FILE_INFORMATION_CLASS Class;
    PUNICODE_STRING FileName;
    ULONG Length;
    PUCHAR Buffer;
    PUCHAR Out = NULL;
    ULONG Base;
    ULONG EntryLength;
    ULONG Room;
    ULONG Copied;
    ULONG NextOffset = 0;
    ULONG LastOffset = 0;
    ULONG Returned = 0;
    ULONG Information = 0;
    ULONGLONG NextCookie = 0;
    BOOLEAN InitialQuery = FALSE;
    BOOLEAN IsRoot = (BOOLEAN)(Dcb->ParentDcb == NULL);
    BOOLEAN WantEaSize;
    BOOLEAN FromEntry;
    BOOLEAN Matched;
    EXN_LIST_ENTRY List;
    PEXT_ENTRY Entry = NULL;
    UNICODE_STRING Name;
    NTSTATUS Status = STATUS_SUCCESS;

    RtlZeroMemory(&Name, sizeof(Name));

    Class    = ExnXSp(IrpSp)->Parameters.QueryDirectory.FileInformationClass;
    FileName = (PUNICODE_STRING)ExnXSp(IrpSp)->Parameters.QueryDirectory.FileName;
    Length   = ExnXSp(IrpSp)->Parameters.QueryDirectory.Length;

    Base = ExnListBaseLength(Class);
    if (Base == 0) {
        return STATUS_INVALID_INFO_CLASS;
    }

    if (Length < Base) {
        return STATUS_INFO_LENGTH_MISMATCH;
    }

    WantEaSize = (BOOLEAN)(Class == FileFullDirectoryInformation ||
                           Class == FileBothDirectoryInformation);

    Buffer = (PUCHAR)ExnMapUserBuffer(Irp);
    if (Buffer == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    /* Built here and copied out at the end: the caller's buffer may fault */
    Out = (PUCHAR)ExAllocatePoolWithTag(PagedPool, Length, EXN_TAG_BUFFER);
    Entry = (PEXT_ENTRY)ExAllocatePoolWithTag(PagedPool, sizeof(EXT_ENTRY), EXN_TAG_BUFFER);
    if (Out == NULL || Entry == NULL) {
        if (Out != NULL) ExFreePool(Out);
        if (Entry != NULL) ExFreePool(Entry);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    (VOID)ExAcquireResourceSharedLite(&Vcb->Resource, TRUE);
    (VOID)ExAcquireResourceExclusiveLite(&Dcb->Resource, TRUE);

    __try {

        Status = ExnVerifyVcb(Ctx, Vcb);
        if (!NT_SUCCESS(Status)) {
            __leave;
        }

        if (Dcb->FcbState & FCB_STATE_DELETED) {
            Status = STATUS_FILE_DELETED;
            __leave;
        }

        if (!(Ccb->Flags & CCB_FLAG_PATTERN_SET) ||
            ((IrpSp->Flags & SL_RESTART_SCAN) && FileName != NULL && FileName->Length != 0)) {

            /* The caller's name lives in its buffer */
            UNICODE_STRING Captured;
            RtlZeroMemory(&Captured, sizeof(Captured));
            Status = STATUS_SUCCESS;

            if (FileName != NULL && FileName->Length != 0) {
                Captured.Buffer = (PWSTR)ExAllocatePoolWithTag(PagedPool, FileName->Length, EXN_TAG_NAME);
                if (Captured.Buffer == NULL) {
                    Status = STATUS_INSUFFICIENT_RESOURCES;
                    __leave;
                }
                Status = ExnSafeCopy(Captured.Buffer, FileName->Buffer, FileName->Length);
                if (NT_SUCCESS(Status)) {
                    Captured.Length = Captured.MaximumLength = FileName->Length;
                }
            }

            if (NT_SUCCESS(Status)) {
                Status = ExnSetPattern(Ccb, Captured.Length ? &Captured : NULL);
            }
            if (Captured.Buffer != NULL) {
                ExFreePool(Captured.Buffer);
            }
            if (!NT_SUCCESS(Status)) {
                __leave;
            }

            InitialQuery = TRUE;
            Ccb->QueryState = 0;
            Ccb->Cookie = 0;
        }

        if (IrpSp->Flags & SL_RESTART_SCAN) {
            Ccb->QueryState = 0;
            Ccb->Cookie = 0;
        }

        if (IsRoot && Ccb->QueryState < 2) {
            Ccb->QueryState = 2;
        }

        for (;;) {

            FromEntry = FALSE;

            if (Ccb->QueryState == 0) {

                ExnListFromDcb(Dcb, &List, L".", sizeof(WCHAR));

            } else if (Ccb->QueryState == 1) {

                ExnListFromDcb(Dcb->ParentDcb, &List, L"..", 2 * sizeof(WCHAR));

            } else {

                ExnEngineEnter(Vcb);
                Status = ExnEngineLeave(Vcb, ExtEnum(&Vcb->Vol, Dcb->Ino, &Dcb->Inode, Ccb->Cookie, Entry));

                if (Status == STATUS_NO_MORE_ENTRIES) {
                    Status = STATUS_SUCCESS;
                    break;
                }
                if (!NT_SUCCESS(Status)) {
                    break;
                }
                NextCookie = Entry->Cookie;

                if (Name.Buffer != NULL) {
                    ExFreePool(Name.Buffer);
                    Name.Buffer = NULL;
                }
                Status = ExnExtToUnicode((const UCHAR *)Entry->Name, Entry->NameLength, &Name);
                if (!NT_SUCCESS(Status)) {
                    break;
                }

                List.Name = Name;
                FromEntry = TRUE;
            }

            Matched = ExnListMatches(Ccb, &List.Name);
            if (!Matched) {
                goto Consumed;
            }

            if (FromEntry) {
                ExnListFromEntry(Vcb, Dcb, Entry, &List, WantEaSize);
            }

            EntryLength = Base + List.Name.Length;
            Room = Length - NextOffset;

            if (NextOffset + Base > Length || (Returned != 0 && EntryLength > Room)) {
                /* Not consumed: the next call starts here */
                if (Returned == 0) {
                    Status = STATUS_BUFFER_OVERFLOW;
                }
                break;
            }

            if (Returned != 0) {
                *(PULONG)(Out + LastOffset) = NextOffset - LastOffset;
            }

            Copied = ExnFillListEntry(Class, Out + NextOffset, Room, &List);

            Returned++;
            LastOffset = NextOffset;
            Information = NextOffset + Base + Copied;

            if (Copied < List.Name.Length) {
                /* Partly returned; NT moves on all the same */
                Status = STATUS_BUFFER_OVERFLOW;
            }

        Consumed:
            if (Ccb->QueryState < 2) {
                Ccb->QueryState++;
            } else {
                Ccb->Cookie = NextCookie;
            }

            if (!Matched) {
                continue;
            }

            if (Status == STATUS_BUFFER_OVERFLOW) {
                break;
            }

            if (IrpSp->Flags & SL_RETURN_SINGLE_ENTRY) {
                break;
            }

            NextOffset = (Information + 7) & ~7UL;

            if (NextOffset >= Length) {
                break;
            }
        }

        if (Returned == 0 && NT_SUCCESS(Status)) {
            Status = InitialQuery ? STATUS_NO_SUCH_FILE : STATUS_NO_MORE_FILES;
        }

        if (Returned != 0) {
            NTSTATUS Copy = ExnSafeCopy(Buffer, Out, Information);
            if (!NT_SUCCESS(Copy)) {
                Status = Copy;
                Information = 0;
            }
        }

        Irp->IoStatus.Information = Information;

    } __finally {

        if (Name.Buffer != NULL) {
            ExFreePool(Name.Buffer);
        }
        ExFreePool(Entry);
        ExFreePool(Out);
        ExnRelease(&Dcb->Resource);
        ExnRelease(&Vcb->Resource);
    }

    return Status;
}

/* ------------------------------------------------------------------ */
/* IRP_MN_NOTIFY_CHANGE_DIRECTORY                                      */
/* ------------------------------------------------------------------ */

#ifdef EXN_NT31

/*
 * NT 3.1 does not export the FsRtlNotifyFull API, so a directory-change
 * notify request cannot be handed to the FsRtl notify package. Completing it
 * at once makes the caller (File Manager) re-read the directory and re-arm
 * the notify in a tight loop. Instead the request is held pending on the
 * volume's list and completed when its handle is cleaned up or it is
 * cancelled. (No live notification; a manual refresh works.)
 */
static VOID NTAPI
ExnCancelNotifyIrp (
    PDEVICE_OBJECT DeviceObject,
    PIRP Irp
    )
{
    UNREFERENCED_PARAMETER(DeviceObject);

    RemoveEntryList(&Irp->Tail.Overlay.ListEntry);
    IoReleaseCancelSpinLock(Irp->CancelIrql);

    Irp->IoStatus.Status = STATUS_CANCELLED;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
}

VOID
ExnNotifyCleanupList (
    PEXN_VCB Vcb,
    PEXN_CCB Ccb
    )
{
    KIRQL Irql;
    LIST_ENTRY Done;
    PLIST_ENTRY Entry, Next;
    PIRP Nirp;
    PIO_STACK_LOCATION Nsp;

    InitializeListHead(&Done);

    IoAcquireCancelSpinLock(&Irql);
    for (Entry = Vcb->DirNotifyList.Flink; Entry != &Vcb->DirNotifyList; Entry = Next) {
        Next = Entry->Flink;
        Nirp = CONTAINING_RECORD(Entry, IRP, Tail.Overlay.ListEntry);
        Nsp = IoGetCurrentIrpStackLocation(Nirp);

        if (Ccb == NULL || Nsp->FileObject->FsContext2 == Ccb) {
            Nirp->CancelRoutine = NULL;
            RemoveEntryList(Entry);
            InsertTailList(&Done, Entry);
        }
    }
    IoReleaseCancelSpinLock(Irql);

    while (!IsListEmpty(&Done)) {
        Entry = RemoveHeadList(&Done);
        Nirp = CONTAINING_RECORD(Entry, IRP, Tail.Overlay.ListEntry);
        Nirp->IoStatus.Status = STATUS_NOTIFY_CLEANUP;
        Nirp->IoStatus.Information = 0;
        IoCompleteRequest(Nirp, IO_NO_INCREMENT);
    }
}

#else

VOID
ExnNotifyCleanupList (
    PEXN_VCB Vcb,
    PEXN_CCB Ccb
    )
{
    FsRtlNotifyCleanup(Vcb->NotifySync, &Vcb->DirNotifyList, Ccb);
}

#endif /* EXN_NT31 */

static NTSTATUS
ExnNotifyChangeDirectory (
    PEXN_IRP_CONTEXT Ctx,
    PEXN_FCB Dcb,
    PEXN_CCB Ccb
    )
{
    PEXN_VCB Vcb = Dcb->Vcb;
    PIO_STACK_LOCATION IrpSp = Ctx->IrpSp;
    NTSTATUS Status;
#ifdef EXN_NT31
    KIRQL Irql;
    BOOLEAN Cancelled = FALSE;
#endif

    (VOID)ExAcquireResourceExclusiveLite(&Vcb->Resource, TRUE);

    __try {

        Status = ExnVerifyVcb(Ctx, Vcb);
        if (!NT_SUCCESS(Status)) {
            __leave;
        }

#ifdef EXN_NT31
        UNREFERENCED_PARAMETER(IrpSp);
        UNREFERENCED_PARAMETER(Ccb);

        IoMarkIrpPending(Ctx->Irp);
        IoAcquireCancelSpinLock(&Irql);
        if (Ctx->Irp->Cancel) {
            IoReleaseCancelSpinLock(Irql);
            Cancelled = TRUE;
        } else {
            InsertTailList(&Vcb->DirNotifyList, &Ctx->Irp->Tail.Overlay.ListEntry);
            Ctx->Irp->CancelRoutine = ExnCancelNotifyIrp;
            IoReleaseCancelSpinLock(Irql);
        }
        Ctx->Flags |= EXN_CTX_NO_COMPLETE;
        Status = STATUS_PENDING;
#else
        Status = ExnBuildFullName(Dcb);
        if (!NT_SUCCESS(Status)) {
            __leave;
        }

        FsRtlNotifyFullChangeDirectory(Vcb->NotifySync,
                                       &Vcb->DirNotifyList,
                                       Ccb,
                                       (PSTRING)&Dcb->FullName,
                                       (BOOLEAN)((IrpSp->Flags & SL_WATCH_TREE) != 0),
                                       FALSE,
                                       ExnXSp(IrpSp)->Parameters.NotifyDirectory.CompletionFilter,
                                       Ctx->Irp,
                                       NULL,
                                       NULL);

        Ctx->Flags |= EXN_CTX_NO_COMPLETE;
        Status = STATUS_PENDING;
#endif

    } __finally {

        ExnRelease(&Vcb->Resource);
    }

#ifdef EXN_NT31
    if (Cancelled) {
        Ctx->Irp->IoStatus.Status = STATUS_CANCELLED;
        Ctx->Irp->IoStatus.Information = 0;
        IoCompleteRequest(Ctx->Irp, IO_NO_INCREMENT);
    }
#endif

    return Status;
}

VOID
ExnNotifyChange (
    PEXN_VCB Vcb,
    PEXN_FCB Fcb,
    ULONG Filter,
    ULONG Action
    )
{
#ifdef EXN_NT31
    UNREFERENCED_PARAMETER(Vcb);
    UNREFERENCED_PARAMETER(Fcb);
    UNREFERENCED_PARAMETER(Filter);
    UNREFERENCED_PARAMETER(Action);
#else
    if (Fcb->ParentDcb == NULL || !NT_SUCCESS(ExnBuildFullName(Fcb))) {
        return;
    }

    FsRtlNotifyFullReportChange(Vcb->NotifySync,
                                &Vcb->DirNotifyList,
                                (PSTRING)&Fcb->FullName,
                                (USHORT)(Fcb->FullName.Length - Fcb->Name.Length),
                                NULL,
                                NULL,
                                Filter,
                                Action,
                                NULL);
#endif
}

NTSTATUS
ExnCommonDirectoryControl (
    PEXN_IRP_CONTEXT Ctx
    )
{
    PFILE_OBJECT FileObject = Ctx->IrpSp->FileObject;
    PEXN_FCB Dcb = (PEXN_FCB)FileObject->FsContext;
    PEXN_CCB Ccb = (PEXN_CCB)FileObject->FsContext2;

    if (Dcb == NULL || Ccb == NULL || !ExnIsDcb(Dcb)) {
        return STATUS_INVALID_PARAMETER;
    }

    switch (Ctx->IrpSp->MinorFunction) {

    case IRP_MN_QUERY_DIRECTORY:
        return ExnQueryDirectory(Ctx, Dcb, Ccb);

    case IRP_MN_NOTIFY_CHANGE_DIRECTORY:
        return ExnNotifyChangeDirectory(Ctx, Dcb, Ccb);
    }

    return STATUS_INVALID_DEVICE_REQUEST;
}
