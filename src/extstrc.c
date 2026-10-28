/*
 * EXTNT - FCBs, CCBs, names, times, attributes and the glue to the engine
 *
 * A file has one FCB, found by its inode number, so every open of a file -
 * under any of its hard-link names - shares one set of section object
 * pointers. An FCB holds a reference on the directory it was opened in.
 *
 * Names: ext names are bytes, normally UTF-8. They map to NT names without
 * loss, both ways:
 *   - valid UTF-8 becomes the same characters (4-byte forms as surrogates);
 *   - a byte that is not valid UTF-8 becomes U+DC80..U+DCFF;
 *   - characters NT names cannot hold (0x01-0x1F " * : < > ? \ |, and a
 *     trailing dot or space) become U+F000 + the character (as Cygwin does).
 */

#include "ext.h"

/* ------------------------------------------------------------------ */
/* Times: ext keeps UTC seconds since 1970 (+ nanoseconds and epoch    */
/* bits in the inode's extra fields)                                   */
/* ------------------------------------------------------------------ */

LARGE_INTEGER
ExnToNtTime (
    ULONG Seconds,
    ULONG Extra
    )
{
    LARGE_INTEGER Time;
    Time.QuadPart = (LONGLONG)ExtGetTime(Seconds, Extra);
    return Time;
}

VOID
ExnFromNtTime (
    LARGE_INTEGER Time,
    PULONG Seconds,
    PULONG Extra
    )
{
    ExtSetTime(Seconds, Extra, (ULONGLONG)Time.QuadPart);
}

/* Is the 32-bit inode field at Offset covered by i_extra_isize? */
static BOOLEAN
ExnExtraField (
    PEXN_VCB Vcb,
    PEXT_INODE Inode,
    ULONG Offset
    )
{
    return (BOOLEAN)(Vcb->Vol.InodeSize > EXT_GOOD_OLD_INODE_SIZE &&
                     EXT_GOOD_OLD_INODE_SIZE + (ULONG)Inode->ExtraIsize >= Offset + 4);
}

/* ------------------------------------------------------------------ */
/* Names                                                               */
/* ------------------------------------------------------------------ */

static BOOLEAN
ExnNtSpecial (
    ULONG C
    )
{
    return (BOOLEAN)((C >= 0x01 && C <= 0x1F) || C == '"' || C == '*' || C == ':' || C == '<' ||
                     C == '>' || C == '?' || C == '\\' || C == '|');
}

BOOLEAN
ExnIsValidUnicodeName (
    PCUNICODE_STRING Name
    )
{
    USHORT Count = Name->Length / sizeof(WCHAR);
    USHORT i;
    WCHAR C;

    if (Count == 0 || Count > EXN_MAX_NAME || (Name->Length & 1) != 0) {
        return FALSE;
    }
    if ((Count == 1 && Name->Buffer[0] == L'.') ||
        (Count == 2 && Name->Buffer[0] == L'.' && Name->Buffer[1] == L'.')) {
        return FALSE;
    }
    for (i = 0; i < Count; i++) {
        C = Name->Buffer[i];
        if (C < 0x20 || C == L'"' || C == L'*' || C == L'/' || C == L':' || C == L'<' ||
            C == L'>' || C == L'?' || C == L'\\' || C == L'|') {
            return FALSE;
        }
    }
    return TRUE;
}

/*
 * NT name -> ext bytes. Fails for a name that would not come back the same
 * (a lone surrogate outside the escape range) or that is longer than 255
 * bytes once encoded.
 */
NTSTATUS
ExnUnicodeToExt (
    PCUNICODE_STRING Name,
    PUCHAR Out,
    PULONG OutLength
    )
{
    ULONG Count = Name->Length / sizeof(WCHAR), i, n = 0;
    const WCHAR *P = Name->Buffer;

    for (i = 0; i < Count; i++) {
        ULONG C = P[i];
        if (C >= 0xF001 && C <= 0xF07F && (ExnNtSpecial(C - 0xF000) ||
            ((C == 0xF02E || C == 0xF020) && i == Count - 1))) {
            C -= 0xF000;                               /* mapped NT-illegal character */
            if (n + 1 > EXN_MAX_NAME) return STATUS_OBJECT_NAME_INVALID;
            Out[n++] = (UCHAR)C;
            continue;
        }
        if (C >= 0xDC80 && C <= 0xDCFF) {                 /* escaped raw byte */
            if (n + 1 > EXN_MAX_NAME) return STATUS_OBJECT_NAME_INVALID;
            Out[n++] = (UCHAR)(C - 0xDC00);
            continue;
        }
        if (C >= 0xD800 && C <= 0xDBFF) {                 /* surrogate pair */
            ULONG L;
            if (i + 1 >= Count) return STATUS_OBJECT_NAME_INVALID;
            L = P[i + 1];
            if (L < 0xDC00 || L > 0xDFFF) return STATUS_OBJECT_NAME_INVALID;
            C = 0x10000 + ((C - 0xD800) << 10) + (L - 0xDC00);
            i++;
        } else if (C >= 0xDC00 && C <= 0xDFFF) {
            return STATUS_OBJECT_NAME_INVALID;
        }
        if (C == 0 || C == '/') return STATUS_OBJECT_NAME_INVALID;
        if (C < 0x80) {
            if (n + 1 > EXN_MAX_NAME) return STATUS_OBJECT_NAME_INVALID;
            Out[n++] = (UCHAR)C;
        } else if (C < 0x800) {
            if (n + 2 > EXN_MAX_NAME) return STATUS_OBJECT_NAME_INVALID;
            Out[n++] = (UCHAR)(0xC0 | (C >> 6));
            Out[n++] = (UCHAR)(0x80 | (C & 0x3F));
        } else if (C < 0x10000) {
            if (n + 3 > EXN_MAX_NAME) return STATUS_OBJECT_NAME_INVALID;
            Out[n++] = (UCHAR)(0xE0 | (C >> 12));
            Out[n++] = (UCHAR)(0x80 | ((C >> 6) & 0x3F));
            Out[n++] = (UCHAR)(0x80 | (C & 0x3F));
        } else {
            if (n + 4 > EXN_MAX_NAME) return STATUS_OBJECT_NAME_INVALID;
            Out[n++] = (UCHAR)(0xF0 | (C >> 18));
            Out[n++] = (UCHAR)(0x80 | ((C >> 12) & 0x3F));
            Out[n++] = (UCHAR)(0x80 | ((C >> 6) & 0x3F));
            Out[n++] = (UCHAR)(0x80 | (C & 0x3F));
        }
    }
    if (n == 0) return STATUS_OBJECT_NAME_INVALID;
    Out[n] = 0;
    *OutLength = n;
    return STATUS_SUCCESS;
}

/* One UTF-8 sequence at In[0..Left): its code point and length, or 0 */
static ULONG
ExnDecodeUtf8 (
    const UCHAR *In,
    ULONG Left,
    PULONG Cp
    )
{
    UCHAR B = In[0];
    ULONG C, Need, i, Min;
    if (B < 0x80) { *Cp = B; return 1; }
    if ((B & 0xE0) == 0xC0) { C = B & 0x1F; Need = 1; Min = 0x80; }
    else if ((B & 0xF0) == 0xE0) { C = B & 0x0F; Need = 2; Min = 0x800; }
    else if ((B & 0xF8) == 0xF0) { C = B & 0x07; Need = 3; Min = 0x10000; }
    else return 0;
    if (Need >= Left) return 0;
    for (i = 1; i <= Need; i++) {
        if ((In[i] & 0xC0) != 0x80) return 0;
        C = (C << 6) | (In[i] & 0x3F);
    }
    if (C < Min || C > 0x10FFFF || (C >= 0xD800 && C <= 0xDFFF)) return 0;
    /* a decoded character in our private escape ranges would not round-trip */
    if ((C >= 0xF001 && C <= 0xF07F) || (C >= 0xDC80 && C <= 0xDCFF)) return 0;
    *Cp = C;
    return Need + 1;
}

/* ext bytes -> NT name. Allocates Name->Buffer (paged pool) */
NTSTATUS
ExnExtToUnicode (
    const UCHAR *In,
    ULONG InLength,
    PUNICODE_STRING Name
    )
{
    ULONG i = 0, n = 0;
    PWSTR W;

    W = (PWSTR)ExAllocatePoolWithTag(PagedPool, (InLength + 1) * 2 * sizeof(WCHAR), EXN_TAG_NAME);
    if (W == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    while (i < InLength) {
        ULONG C, L = ExnDecodeUtf8(In + i, InLength - i, &C);
        if (L == 0) {
            W[n++] = (WCHAR)(0xDC00 | In[i]);           /* not UTF-8: escape the byte */
            i++;
            continue;
        }
        if (C < 0x80 && (ExnNtSpecial(C) || ((C == '.' || C == ' ') && i + 1 == InLength))) {
            C += 0xF000;
        }
        if (C >= 0x10000) {
            C -= 0x10000;
            W[n++] = (WCHAR)(0xD800 + (C >> 10));
            W[n++] = (WCHAR)(0xDC00 + (C & 0x3FF));
        } else {
            W[n++] = (WCHAR)C;
        }
        i += L;
    }
    Name->Buffer = W;
    Name->Length = (USHORT)(n * sizeof(WCHAR));
    Name->MaximumLength = (USHORT)((InLength + 1) * 2 * sizeof(WCHAR));
    return STATUS_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Attributes                                                          */
/* ------------------------------------------------------------------ */

/* user.DOSATTRIB as Samba writes it in its plain form: "0x" + hex */
ULONG
ExnLoadDosAttributes (
    PEXN_VCB Vcb,
    ULONG Ino,
    PEXT_INODE Inode
    )
{
    UCHAR V[64];
    ULONG Len = 0, i, A = 0;

    if (!NT_SUCCESS(ExtGetXattr(&Vcb->Vol, Ino, Inode, EXT_XATTR_INDEX_USER, "DOSATTRIB", 9,
                                V, sizeof(V) - 1, &Len)) || Len < 3) {
        return 0;
    }
    if (V[0] != '0' || (V[1] != 'x' && V[1] != 'X')) {
        return 0;               /* Samba's binary forms: not ours to parse */
    }
    for (i = 2; i < Len && V[i]; i++) {
        UCHAR c = V[i];
        ULONG d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else break;
        A = (A << 4) | d;
    }
    return A & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_ARCHIVE);
}

NTSTATUS
ExnStoreDosAttributes (
    PEXN_VCB Vcb,
    PEXN_FCB Fcb,
    ULONG Attributes
    )
{
    static const char Hex[] = "0123456789abcdef";
    char V[8];
    EXT_STATUS S;

    Attributes &= FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_ARCHIVE;
    if (Attributes == Fcb->DosAttributes) {
        return STATUS_SUCCESS;
    }
    if (Attributes == 0) {
        S = ExtSetXattr(&Vcb->Vol, Fcb->Ino, &Fcb->Inode, EXT_XATTR_INDEX_USER, "DOSATTRIB", 9, NULL, 0);
    } else {
        V[0] = '0'; V[1] = 'x';
        V[2] = Hex[(Attributes >> 4) & 0xF];
        V[3] = Hex[Attributes & 0xF];
        S = ExtSetXattr(&Vcb->Vol, Fcb->Ino, &Fcb->Inode, EXT_XATTR_INDEX_USER, "DOSATTRIB", 9,
                        (const UCHAR *)V, 4);
    }
    if (S == EXT_NOT_SUPPORTED) {
        S = EXT_OK;             /* revision 0 / ea_inode: keep them in memory only */
    }
    if (NT_SUCCESS(S)) {
        Fcb->DosAttributes = Attributes;
    }
    return S;
}

ULONG
ExnComputeAttributes (
    PEXN_VCB Vcb,
    PEXT_INODE Inode,
    const UCHAR *Name,
    ULONG NameLength,
    ULONG DosAttributes
    )
{
    ULONG A = DosAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_ARCHIVE);
    USHORT Type = Inode->Mode & EXT_S_IFMT;

    if (Type == EXT_S_IFDIR) {
        A |= FILE_ATTRIBUTE_DIRECTORY;
    } else if (!(Inode->Mode & 0200)) {
        A |= FILE_ATTRIBUTE_READONLY;          /* no owner write permission */
    }
    if (Type != EXT_S_IFDIR && Type != EXT_S_IFREG && Type != EXT_S_IFLNK) {
        A |= FILE_ATTRIBUTE_SYSTEM;            /* device, fifo, socket */
    }
    if (Inode->Flags & EXT_IF_IMMUTABLE) {
        A |= FILE_ATTRIBUTE_READONLY;
    }
    if (ExnData.HideDotFiles && NameLength > 1 && Name[0] == '.' &&
        !(NameLength == 2 && Name[1] == '.')) {
        A |= FILE_ATTRIBUTE_HIDDEN;
    }
    if (Vcb->VcbState & VCB_STATE_READ_ONLY) {
        /* nothing on a read-only volume can be written */
    }
    if (A == 0) {
        A = FILE_ATTRIBUTE_NORMAL;
    }
    return A;
}

/* ------------------------------------------------------------------ */
/* FCBs                                                                */
/* ------------------------------------------------------------------ */

static PEXN_FCB
ExnAllocateFcb (
    PEXN_VCB Vcb,
    CSHORT NodeType
    )
{
    PEXN_FCB Fcb;

    Fcb = (PEXN_FCB)ExAllocatePoolWithTag(NonPagedPool, sizeof(EXN_FCB), EXN_TAG_FCB);
    if (Fcb == NULL) {
        return NULL;
    }

    RtlZeroMemory(Fcb, sizeof(EXN_FCB));

    Fcb->Header.NodeTypeCode = NodeType;
    Fcb->Header.NodeByteSize = (CSHORT)sizeof(EXN_FCB);
    Fcb->Header.IsFastIoPossible = FastIoIsNotPossible;

    ExInitializeResourceLite(&Fcb->Resource);
    ExInitializeResourceLite(&Fcb->PagingIoResource);

    Fcb->Header.Resource = &Fcb->Resource;
    Fcb->Header.PagingIoResource = &Fcb->PagingIoResource;

    FsRtlInitializeFileLock(&Fcb->FileLock, NULL, NULL);

    Fcb->Vcb = Vcb;
    InitializeListHead(&Fcb->FcbLinks);

    return Fcb;
}

VOID
ExnDeleteFcb (
    PEXN_FCB Fcb
    )
{
    RemoveEntryList(&Fcb->FcbLinks);

    FsRtlUninitializeFileLock(&Fcb->FileLock);

    ExDeleteResourceLite(&Fcb->Resource);
    ExDeleteResourceLite(&Fcb->PagingIoResource);

    if (Fcb->Name.Buffer != NULL) {
        ExFreePool(Fcb->Name.Buffer);
    }
    if (Fcb->FullName.Buffer != NULL) {
        ExFreePool(Fcb->FullName.Buffer);
    }
    if (Fcb->Runs != NULL) {
        ExFreePool(Fcb->Runs);
    }
    ExnIndexFree(Fcb);

    ExFreePool(Fcb);
}

PEXN_FCB
ExnCreateVolumeFcb (
    PEXN_VCB Vcb
    )
{
    PEXN_FCB Fcb;

    Fcb = ExnAllocateFcb(Vcb, EXN_NTC_VFCB);
    if (Fcb == NULL) {
        return NULL;
    }

    Fcb->Header.AllocationSize.QuadPart = Vcb->PartitionBytes;
    Fcb->Header.FileSize.QuadPart = Vcb->PartitionBytes;
    Fcb->Header.ValidDataLength.QuadPart = Vcb->PartitionBytes;

    return Fcb;
}

/* The FCB's sizes, times and attributes from its inode copy */
VOID
ExnInodeToFcb (
    PEXN_VCB Vcb,
    PEXN_FCB Fcb
    )
{
    PEXT_INODE I = &Fcb->Inode;
    LONGLONG Size = (LONGLONG)ExtInodeSize(&Vcb->Vol, I);

    if (ExnIsDcb(Fcb)) {
        Size = 0;
    }
    Fcb->Header.FileSize.QuadPart = Size;
    Fcb->Header.ValidDataLength.QuadPart = Size;
    Fcb->Header.AllocationSize.QuadPart = ExnRoundUp(Size, Vcb->Vol.BlockSize);

    Fcb->LastWriteTime = ExnToNtTime(I->Mtime, ExnExtraField(Vcb, I, 0x88) ? I->MtimeExtra : 0);
    Fcb->LastAccessTime = ExnToNtTime(I->Atime, ExnExtraField(Vcb, I, 0x8C) ? I->AtimeExtra : 0);
    Fcb->ChangeTime = ExnToNtTime(I->Ctime, ExnExtraField(Vcb, I, 0x84) ? I->CtimeExtra : 0);
    if (ExnExtraField(Vcb, I, 0x90) && I->Crtime != 0) {
        Fcb->CreationTime = ExnToNtTime(I->Crtime, ExnExtraField(Vcb, I, 0x94) ? I->CrtimeExtra : 0);
    } else {
        /* no birth time on disk: the oldest time we have */
        Fcb->CreationTime = Fcb->LastWriteTime.QuadPart < Fcb->ChangeTime.QuadPart ?
                            Fcb->LastWriteTime : Fcb->ChangeTime;
    }
    Fcb->Attributes = ExnComputeAttributes(Vcb, I, Fcb->ExtName, Fcb->ExtNameLength, Fcb->DosAttributes);
}

/* Reads the inode into the FCB (engine not held by the caller) */
NTSTATUS
ExnLoadInode (
    PEXN_VCB Vcb,
    PEXN_FCB Fcb
    )
{
    NTSTATUS Status;

    ExnEngineEnter(Vcb);
    Status = ExtReadInode(&Vcb->Vol, Fcb->Ino, &Fcb->Inode);
    if (NT_SUCCESS(Status)) {
        Fcb->DosAttributes = ExnLoadDosAttributes(Vcb, Fcb->Ino, &Fcb->Inode);
    }
    Status = ExnEngineLeave(Vcb, Status);
    if (NT_SUCCESS(Status)) {
        if (Fcb->Inode.LinksCount == 0 && Fcb->Ino != EXT_ROOT_INO) {
            return STATUS_FILE_CORRUPT_ERROR;     /* a deleted inode under a live name */
        }
        ExnInodeToFcb(Vcb, Fcb);
    }
    return Status;
}

PEXN_FCB
ExnCreateRootDcb (
    PEXN_VCB Vcb
    )
{
    PEXN_FCB Dcb;

    Dcb = ExnAllocateFcb(Vcb, EXN_NTC_DCB);
    if (Dcb == NULL) {
        return NULL;
    }

    Dcb->FcbState = FCB_STATE_ROOT;
    Dcb->Ino = EXT_ROOT_INO;
    if (!NT_SUCCESS(ExnLoadInode(Vcb, Dcb))) {
        ExnDeleteFcb(Dcb);
        return NULL;
    }
    Dcb->Attributes = FILE_ATTRIBUTE_DIRECTORY;

    InsertTailList(&Vcb->FcbList, &Dcb->FcbLinks);
    return Dcb;
}

NTSTATUS
ExnCreateFcb (
    PEXN_VCB Vcb,
    PEXN_FCB ParentDcb,
    PEXT_ENTRY Entry,
    BOOLEAN AsLink,
    PEXN_FCB *NewFcb
    )
{
    PEXN_FCB Fcb;
    EXT_INODE Inode;
    BOOLEAN IsDirectory;
    NTSTATUS Status;

    *NewFcb = NULL;

    ExnEngineEnter(Vcb);
    Status = ExnEngineLeave(Vcb, ExtReadInode(&Vcb->Vol, Entry->Inode, &Inode));
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    IsDirectory = (BOOLEAN)((Inode.Mode & EXT_S_IFMT) == EXT_S_IFDIR);

    Fcb = ExnAllocateFcb(Vcb, (CSHORT)(IsDirectory ? EXN_NTC_DCB : EXN_NTC_FCB));
    if (Fcb == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Status = ExnExtToUnicode((const UCHAR *)Entry->Name, Entry->NameLength, &Fcb->Name);
    if (!NT_SUCCESS(Status)) {
        ExnDeleteFcb(Fcb);
        return Status;
    }
    Fcb->ExtNameLength = Entry->NameLength;
    RtlCopyMemory(Fcb->ExtName, Entry->Name, Entry->NameLength);
    Fcb->ExtName[Entry->NameLength] = 0;

    Fcb->ParentDcb = ParentDcb;
    Fcb->Ino = Entry->Inode;
    if (AsLink) {
        Fcb->FcbState |= FCB_STATE_SYMLINK;
    }

    Status = ExnLoadInode(Vcb, Fcb);
    if (!NT_SUCCESS(Status)) {
        ExnDeleteFcb(Fcb);
        return Status;
    }

    InsertTailList(&Vcb->FcbList, &Fcb->FcbLinks);
    ParentDcb->RefCount++;

    *NewFcb = Fcb;
    return STATUS_SUCCESS;
}

VOID
ExnDereferenceFcb (
    PEXN_VCB Vcb,
    PEXN_FCB Fcb
    )
{
    PEXN_FCB Parent;

    while (Fcb != NULL) {

        Fcb->RefCount--;

        if (Fcb->RefCount != 0 || Fcb == Vcb->RootDcb || Fcb == Vcb->VolumeFcb) {
            break;
        }

        Parent = Fcb->ParentDcb;
        ExnDeleteFcb(Fcb);
        Fcb = Parent;
    }
}

/* The live FCB of an inode; a symbolic link opened as itself has its own */
PEXN_FCB
ExnFindFcbByIno (
    PEXN_VCB Vcb,
    ULONG Ino,
    BOOLEAN AsLink
    )
{
    PLIST_ENTRY Entry;
    PEXN_FCB Fcb;

    for (Entry = Vcb->FcbList.Flink; Entry != &Vcb->FcbList; Entry = Entry->Flink) {
        Fcb = CONTAINING_RECORD(Entry, EXN_FCB, FcbLinks);
        if (Fcb->Ino == Ino && !(Fcb->FcbState & FCB_STATE_DELETED) &&
            (BOOLEAN)((Fcb->FcbState & FCB_STATE_SYMLINK) != 0) == AsLink) {
            return Fcb;
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* CCBs                                                                */
/* ------------------------------------------------------------------ */

PEXN_CCB
ExnCreateCcb (
    VOID
    )
{
    PEXN_CCB Ccb;

    Ccb = (PEXN_CCB)ExAllocatePoolWithTag(PagedPool, sizeof(EXN_CCB), EXN_TAG_CCB);
    if (Ccb == NULL) {
        return NULL;
    }

    RtlZeroMemory(Ccb, sizeof(EXN_CCB));
    Ccb->NodeTypeCode = EXN_NTC_CCB;
    Ccb->NodeByteSize = (CSHORT)sizeof(EXN_CCB);

    return Ccb;
}

VOID
ExnDeleteCcb (
    PEXN_CCB Ccb
    )
{
    if (Ccb->Pattern.Buffer != NULL) {
        ExFreePool(Ccb->Pattern.Buffer);
    }
    ExFreePool(Ccb);
}

/* ------------------------------------------------------------------ */
/* Full names                                                          */
/* ------------------------------------------------------------------ */

NTSTATUS
ExnBuildFullName (
    PEXN_FCB Fcb
    )
{
    PEXN_FCB Walk;
    ULONG Length = 0;
    PWSTR Buffer;
    PWSTR Write;

    if (Fcb->FullName.Buffer != NULL) {
        return STATUS_SUCCESS;
    }

    if (Fcb->ParentDcb == NULL) {
        Length = sizeof(WCHAR);
    } else {
        for (Walk = Fcb; Walk->ParentDcb != NULL; Walk = Walk->ParentDcb) {
            Length += sizeof(WCHAR) + Walk->Name.Length;
        }
    }

    if (Length > 0xFFFE) {
        return STATUS_NAME_TOO_LONG;
    }

    Buffer = (PWSTR)ExAllocatePoolWithTag(PagedPool, Length, EXN_TAG_NAME);
    if (Buffer == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    if (Fcb->ParentDcb == NULL) {
        Buffer[0] = L'\\';
    } else {
        Write = (PWSTR)((PUCHAR)Buffer + Length);
        for (Walk = Fcb; Walk->ParentDcb != NULL; Walk = Walk->ParentDcb) {
            Write = (PWSTR)((PUCHAR)Write - Walk->Name.Length);
            RtlCopyMemory(Write, Walk->Name.Buffer, Walk->Name.Length);
            Write--;
            *Write = L'\\';
        }
    }

    Fcb->FullName.Buffer = Buffer;
    Fcb->FullName.Length = (USHORT)Length;
    Fcb->FullName.MaximumLength = (USHORT)Length;

    return STATUS_SUCCESS;
}

BOOLEAN
ExnIsAncestor (
    PEXN_FCB Ancestor,
    PEXN_FCB Fcb
    )
{
    for (; Fcb != NULL; Fcb = Fcb->ParentDcb) {
        if (Fcb == Ancestor) {
            return TRUE;
        }
    }
    return FALSE;
}

VOID
ExnForgetNames (
    PEXN_VCB Vcb,
    PEXN_FCB Fcb
    )
{
    PLIST_ENTRY Entry;
    PEXN_FCB Walk;

    for (Entry = Vcb->FcbList.Flink; Entry != &Vcb->FcbList; Entry = Entry->Flink) {
        Walk = CONTAINING_RECORD(Entry, EXN_FCB, FcbLinks);
        if (Walk->FullName.Buffer != NULL && ExnIsAncestor(Fcb, Walk)) {
            ExFreePool(Walk->FullName.Buffer);
            RtlZeroMemory(&Walk->FullName, sizeof(UNICODE_STRING));
        }
    }
}

/* ------------------------------------------------------------------ */
/* Inode updates and allocation                                        */
/* ------------------------------------------------------------------ */

VOID
ExnForgetMapping (
    PEXN_FCB Fcb
    )
{
    Fcb->MapRun = 0;
}

/* Writes the FCB's size and times into its inode (engine not held) */
NTSTATUS
ExnUpdateInode (
    PEXN_VCB Vcb,
    PEXN_FCB Fcb
    )
{
    PEXT_INODE I = &Fcb->Inode;
    NTSTATUS Status;

    if (Fcb->FcbState & FCB_STATE_DELETED) {
        Fcb->FcbState &= ~FCB_STATE_INODE_DIRTY;
        return STATUS_SUCCESS;
    }
    if (Vcb->VcbState & (VCB_STATE_READ_ONLY | VCB_STATE_DISMOUNTED)) {
        return STATUS_SUCCESS;
    }

    ExnEngineEnter(Vcb);
    if (ExnIsFcb(Fcb) && !(Fcb->FcbState & FCB_STATE_SYMLINK)) {
        ExtInodeSetSize(&Vcb->Vol, I, (ULONGLONG)Fcb->Header.FileSize.QuadPart);
    }
    ExnFromNtTime(Fcb->LastWriteTime, &I->Mtime, ExnExtraField(Vcb, I, 0x88) ? &I->MtimeExtra : NULL);
    ExnFromNtTime(Fcb->LastAccessTime, &I->Atime, ExnExtraField(Vcb, I, 0x8C) ? &I->AtimeExtra : NULL);
    ExnFromNtTime(Fcb->ChangeTime, &I->Ctime, ExnExtraField(Vcb, I, 0x84) ? &I->CtimeExtra : NULL);
    if (ExnExtraField(Vcb, I, 0x90)) {
        ExnFromNtTime(Fcb->CreationTime, &I->Crtime, ExnExtraField(Vcb, I, 0x94) ? &I->CrtimeExtra : NULL);
    }
    Status = ExtWriteInode(&Vcb->Vol, Fcb->Ino, I);
    Status = ExnEngineLeave(Vcb, Status);

    if (NT_SUCCESS(Status)) {
        Fcb->FcbState &= ~FCB_STATE_INODE_DIRTY;
    }
    return Status;
}

/*
 * The allocation of a file: ext files are sparse, so growing only moves the
 * reported size (blocks come on first write); shrinking frees the blocks
 * past the new end. Called with the file exclusive.
 */
NTSTATUS
ExnSetAllocation (
    PEXN_VCB Vcb,
    PEXN_FCB Fcb,
    ULONGLONG Bytes
    )
{
    ULONGLONG Blocks = (Bytes + Vcb->Vol.BlockSize - 1) >> Vcb->Vol.BlockShift;
    ULONGLONG Have = (ULONGLONG)Fcb->Header.AllocationSize.QuadPart >> Vcb->Vol.BlockShift;
    NTSTATUS Status = STATUS_SUCCESS;

    if (Blocks < Have) {
        (VOID)ExAcquireResourceExclusiveLite(&Fcb->PagingIoResource, TRUE);
        ExnEngineEnter(Vcb);
        Status = ExtTruncateBlocks(&Vcb->Vol, Fcb->Ino, &Fcb->Inode, Blocks);
        if (NT_SUCCESS(Status)) {
            Status = ExtWriteInode(&Vcb->Vol, Fcb->Ino, &Fcb->Inode);
        }
        ExnForgetMapping(Fcb);          /* under the engine lock, like its readers */
        Status = ExnEngineLeave(Vcb, Status);
        ExnRelease(&Fcb->PagingIoResource);
        if (!NT_SUCCESS(Status)) {
            return Status;
        }
    }
    Fcb->Header.AllocationSize.QuadPart = (LONGLONG)(Blocks << Vcb->Vol.BlockShift);
    return Status;
}

/*
 * A paging file: every block allocated, and the map kept in nonpaged pool so
 * that paging I/O never enters the engine (which allocates paged memory and
 * takes locks a page fault could be waiting on).
 */
NTSTATUS
ExnLoadRuns (
    PEXN_VCB Vcb,
    PEXN_FCB Fcb
    )
{
    ULONGLONG Blocks = (ULONGLONG)(Fcb->Header.AllocationSize.QuadPart + Vcb->Vol.BlockSize - 1) >> Vcb->Vol.BlockShift;
    ULONGLONG Blk;
    ULONG Count = 0, Cap = 64;
    PEXN_RUN Runs, Bigger;
    NTSTATUS Status = STATUS_SUCCESS;

    Runs = (PEXN_RUN)ExAllocatePoolWithTag(NonPagedPool, Cap * sizeof(EXN_RUN), EXN_TAG_RUNS);
    if (Runs == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    ExnEngineEnter(Vcb);
    for (Blk = 0; Blk < Blocks && NT_SUCCESS(Status); ) {
        ULONGLONG Disk;
        ULONG Run;
        Status = ExtMapFileBlock(&Vcb->Vol, Fcb->Ino, &Fcb->Inode, Blk, &Disk, &Run);
        if (!NT_SUCCESS(Status)) break;
        if (Disk == 0) {
            BOOLEAN Fresh;
            Status = ExtGetFileBlock(&Vcb->Vol, Fcb->Ino, &Fcb->Inode, Blk, &Disk, &Fresh);
            if (!NT_SUCCESS(Status)) break;
            Run = 1;
        }
        if (Run > Blocks - Blk) Run = (ULONG)(Blocks - Blk);
        if (Count && Runs[Count - 1].DiskBlock + Runs[Count - 1].Length == Disk &&
            Runs[Count - 1].FileBlock + Runs[Count - 1].Length == Blk) {
            Runs[Count - 1].Length += Run;
        } else {
            if (Count == Cap) {
                Bigger = (PEXN_RUN)ExAllocatePoolWithTag(NonPagedPool, Cap * 2 * sizeof(EXN_RUN), EXN_TAG_RUNS);
                if (Bigger == NULL) { Status = STATUS_INSUFFICIENT_RESOURCES; break; }
                RtlCopyMemory(Bigger, Runs, Count * sizeof(EXN_RUN));
                ExFreePool(Runs);
                Runs = Bigger;
                Cap *= 2;
            }
            Runs[Count].FileBlock = Blk;
            Runs[Count].DiskBlock = Disk;
            Runs[Count].Length = Run;
            Count++;
        }
        Blk += Run;
    }
    if (NT_SUCCESS(Status)) {
        Status = ExtWriteInode(&Vcb->Vol, Fcb->Ino, &Fcb->Inode);
    }
    Status = ExnEngineLeave(Vcb, Status);

    if (!NT_SUCCESS(Status)) {
        ExFreePool(Runs);
        return Status;
    }
    (VOID)ExAcquireResourceExclusiveLite(&Fcb->PagingIoResource, TRUE);
    if (Fcb->Runs != NULL) {
        ExFreePool(Fcb->Runs);
    }
    Fcb->Runs = Runs;
    Fcb->RunCount = Count;
    ExnRelease(&Fcb->PagingIoResource);
    return STATUS_SUCCESS;
}
