/*
 * EXTNT - extended attributes: IRP_MJ_QUERY_EA, IRP_MJ_SET_EA and the EAs
 * given at create
 *
 * NT EAs are kept as ext xattrs in the user namespace ("user." + the EA
 * name); Samba's user.DOSATTRIB, which holds the DOS attributes, is not
 * shown as an EA. Internally a file's EAs are handled as an OS/2 FEA list
 * (flags, name length, value length, name, 0, value), built from the xattrs
 * when read and applied back as the difference when written. EA names are
 * compared without regard to case (as NT does) and new ones are stored in
 * upper case.
 */

#include "ext.h"

#ifndef FILE_NEED_EA
#define FILE_NEED_EA                0x00000080
#endif

#define ExnLongAlign(x)            (((x) + 3) & ~3UL)

/* Size of one record as NT returns it */
#define ExnNtEaRecordSize(NameLength, ValueLength) \
    ExnLongAlign(FIELD_OFFSET(FILE_FULL_EA_INFORMATION, EaName) + (NameLength) + 1 + (ValueLength))

#define EXN_MAX_EA_LIST     65536
#ifndef EXN_UNKNOWN
#define EXN_UNKNOWN         0xFFFFFFFFUL
#endif

#include <pshpack1.h>
typedef struct _EXN_EA {
    UCHAR       Flags;
    UCHAR       NameLength;
    UCHAR       ValueLengthLo;
    UCHAR       ValueLengthHi;
    /* name, 0, value */
} EXN_EA, *PEXN_EA;
#include <poppack.h>

#define EXN_EA_NEEDEA               0x80
#define ExnEaValueLength(E)         ((ULONG)(E)->ValueLengthLo | ((ULONG)(E)->ValueLengthHi << 8))
#define ExnEaSize(E)                (sizeof(EXN_EA) + (E)->NameLength + 1 + ExnEaValueLength(E))

static UCHAR
ExnEaUpper (
    UCHAR C
    )
{
    return (UCHAR)((C >= 'a' && C <= 'z') ? C - 32 : C);
}

static BOOLEAN
ExnIsHiddenXattr (
    const char *Name,
    ULONG Length
    )
{
    return (BOOLEAN)(Length == 9 && RtlCompareMemory(Name, "DOSATTRIB", 9) == 9);
}

/* ------------------------------------------------------------------ */
/* The file's list                                                     */
/* ------------------------------------------------------------------ */

typedef struct {
    PUCHAR      List;
    ULONG       Length;
    ULONG       Cap;
    NTSTATUS    Status;
} EXN_EA_BUILD;

static VOID
ExnEaBuildCb (
    PVOID Ctx,
    UCHAR Index,
    const char *Name,
    ULONG NameLength,
    const UCHAR *Value,
    ULONG ValueLength
    )
{
    EXN_EA_BUILD *B = (EXN_EA_BUILD *)Ctx;
    ULONG Need = sizeof(EXN_EA) + NameLength + 1 + ValueLength;
    PEXN_EA E;

    if (Index != EXT_XATTR_INDEX_USER || ExnIsHiddenXattr(Name, NameLength) || !NT_SUCCESS(B->Status) ||
        NameLength == 0 || NameLength > 255 || ValueLength > 0xFFFF || Value == NULL) {
        return;
    }
    if (B->Length + Need > B->Cap) {
        ULONG NewCap = (B->Cap ? B->Cap * 2 : 1024);
        PUCHAR N;
        while (NewCap < B->Length + Need) NewCap *= 2;
        N = (PUCHAR)ExtHostAlloc(NewCap);
        if (N == NULL) { B->Status = STATUS_INSUFFICIENT_RESOURCES; return; }
        if (B->List) { RtlCopyMemory(N, B->List, B->Length); ExtHostFree(B->List); }
        B->List = N;
        B->Cap = NewCap;
    }
    E = (PEXN_EA)(B->List + B->Length);
    E->Flags = 0;
    E->NameLength = (UCHAR)NameLength;
    E->ValueLengthLo = (UCHAR)ValueLength;
    E->ValueLengthHi = (UCHAR)(ValueLength >> 8);
    RtlCopyMemory(E + 1, Name, NameLength);
    ((PUCHAR)(E + 1))[NameLength] = 0;
    RtlCopyMemory((PUCHAR)(E + 1) + NameLength + 1, Value, ValueLength);
    B->Length += Need;
}

/* The file's EAs as an FEA list (NULL when it has none); free with ExtHostFree */
static NTSTATUS
ExnReadEaList (
    PEXN_VCB Vcb,
    PEXN_FCB Fcb,
    PUCHAR *List,
    PULONG Length
    )
{
    EXN_EA_BUILD B;
    NTSTATUS Status;

    RtlZeroMemory(&B, sizeof(B));
    ExnEngineEnter(Vcb);
    Status = ExtListXattr(&Vcb->Vol, Fcb->Ino, &Fcb->Inode, ExnEaBuildCb, &B);
    Status = ExnEngineLeave(Vcb, Status);
    if (NT_SUCCESS(Status)) {
        Status = B.Status;
    }
    if (!NT_SUCCESS(Status)) {
        if (B.List) ExtHostFree(B.List);
        return Status;
    }
    *List = B.List;
    *Length = B.Length;
    return STATUS_SUCCESS;
}

/* Makes the file's user.* xattrs (DOSATTRIB aside) what List says */
static NTSTATUS
ExnWriteEaList (
    PEXN_VCB Vcb,
    PEXN_FCB Fcb,
    const UCHAR *List,
    ULONG Length
    )
{
    PUCHAR Old = NULL;
    ULONG OldLength = 0, Off, Pos;
    NTSTATUS Status;

    Status = ExnReadEaList(Vcb, Fcb, &Old, &OldLength);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    ExnEngineEnter(Vcb);

    /* gone or renamed (by case) */
    for (Off = 0; NT_SUCCESS(Status) && Off + sizeof(EXN_EA) <= OldLength; Off += ExnEaSize((PEXN_EA)(Old + Off))) {
        PEXN_EA O = (PEXN_EA)(Old + Off);
        BOOLEAN Keep = FALSE;
        for (Pos = 0; Pos + sizeof(EXN_EA) <= Length; Pos += ExnEaSize((const EXN_EA *)(List + Pos))) {
            const EXN_EA *N = (const EXN_EA *)(List + Pos);
            if (N->NameLength == O->NameLength &&
                RtlCompareMemory(N + 1, O + 1, O->NameLength) == O->NameLength) {
                Keep = TRUE;
                break;
            }
        }
        if (!Keep) {
            Status = ExtSetXattr(&Vcb->Vol, Fcb->Ino, &Fcb->Inode, EXT_XATTR_INDEX_USER,
                                 (const char *)(O + 1), O->NameLength, NULL, 0);
        }
    }

    /* new or changed */
    for (Pos = 0; NT_SUCCESS(Status) && Pos + sizeof(EXN_EA) <= Length; Pos += ExnEaSize((const EXN_EA *)(List + Pos))) {
        const EXN_EA *N = (const EXN_EA *)(List + Pos);
        const UCHAR *NV = (const UCHAR *)(N + 1) + N->NameLength + 1;
        BOOLEAN Same = FALSE;
        for (Off = 0; Off + sizeof(EXN_EA) <= OldLength; Off += ExnEaSize((PEXN_EA)(Old + Off))) {
            PEXN_EA O = (PEXN_EA)(Old + Off);
            if (O->NameLength == N->NameLength &&
                RtlCompareMemory(N + 1, O + 1, O->NameLength) == O->NameLength) {
                Same = (BOOLEAN)(ExnEaValueLength(O) == ExnEaValueLength(N) &&
                                 RtlCompareMemory((PUCHAR)(O + 1) + O->NameLength + 1, NV,
                                                  ExnEaValueLength(N)) == ExnEaValueLength(N));
                break;
            }
        }
        if (!Same) {
            Status = ExtSetXattr(&Vcb->Vol, Fcb->Ino, &Fcb->Inode, EXT_XATTR_INDEX_USER,
                                 (const char *)(N + 1), N->NameLength, NV, ExnEaValueLength(N));
        }
    }

    Status = ExnEngineLeave(Vcb, Status);
    if (Old != NULL) {
        ExtHostFree(Old);
    }
    return Status;
}

static ULONG
ExnNtSizeOfList (
    const UCHAR *List,
    ULONG Length
    )
{
    ULONG Off = 0, Size = 0;
    const EXN_EA *E;

    while (Off + sizeof(EXN_EA) <= Length) {
        E = (const EXN_EA *)(List + Off);
        Size += ExnNtEaRecordSize(E->NameLength, ExnEaValueLength(E));
        Off += ExnEaSize(E);
    }
    return Size;
}

/*
 * Bytes an NtQueryEaFile of all of the file's EAs needs: what
 * FILE_EA_INFORMATION and the directory listings report.
 */
ULONG
ExnNtEaSize (
    PEXN_VCB Vcb,
    PEXN_FCB Fcb
    )
{
    PUCHAR List = NULL;
    ULONG Length = 0;
    ULONG Size = 0;

    if (ExnIsVfcb(Fcb) || (Fcb->FcbState & FCB_STATE_DELETED)) {
        return 0;
    }
    if (NT_SUCCESS(ExnReadEaList(Vcb, Fcb, &List, &Length)) && List != NULL) {
        Size = ExnNtSizeOfList(List, Length);
    }
    if (List != NULL) {
        ExtHostFree(List);
    }
    return Size;
}

/* Finds Name (upper case) in an EXN list; returns its offset or ~0 */
static ULONG
ExnFindEa (
    PEXN_VCB Vcb,
    const UCHAR *List,
    ULONG Length,
    const UCHAR *Name,
    ULONG NameLength
    )
{
    ULONG Off = 0, i;
    const EXN_EA *E;
    const UCHAR *N;

    while (Off + sizeof(EXN_EA) <= Length) {
        E = (const EXN_EA *)(List + Off);
        if (E->NameLength == NameLength) {
            N = (const UCHAR *)(E + 1);
            for (i = 0; i < NameLength; i++) {
                if (ExnEaUpper(N[i]) != ExnEaUpper(Name[i])) {
                    break;
                }
            }
            if (i == NameLength) {
                return Off;
            }
        }
        Off += ExnEaSize(E);
    }
    return EXN_UNKNOWN;
}

/* ------------------------------------------------------------------ */
/* Checking what the caller gives                                      */
/* ------------------------------------------------------------------ */

/*
 * A FILE_FULL_EA_INFORMATION list is well formed: every record inside the
 * buffer, names 0-terminated, links long-aligned and forward.
 */
NTSTATUS
ExnCheckEaBuffer (
    PFILE_FULL_EA_INFORMATION Buffer,
    ULONG Length,
    PULONG ErrorOffset
    )
{
    ULONG Off = 0;
    ULONG Size;
    PFILE_FULL_EA_INFORMATION Ea;

    for (;;) {

        *ErrorOffset = Off;

        if (Off > Length || Length - Off < FIELD_OFFSET(FILE_FULL_EA_INFORMATION, EaName) + 1) {
            return STATUS_EA_LIST_INCONSISTENT;
        }

        Ea = (PFILE_FULL_EA_INFORMATION)((PUCHAR)Buffer + Off);
        Size = FIELD_OFFSET(FILE_FULL_EA_INFORMATION, EaName) + Ea->EaNameLength + 1 + Ea->EaValueLength;

        if (Ea->EaNameLength == 0 || Size > Length - Off || Ea->EaName[Ea->EaNameLength] != 0) {
            return STATUS_EA_LIST_INCONSISTENT;
        }

        if (Ea->NextEntryOffset == 0) {
            return STATUS_SUCCESS;
        }

        if (Ea->NextEntryOffset < Size || (Ea->NextEntryOffset & 3) != 0 ||
            Ea->NextEntryOffset > Length - Off) {
            return STATUS_EA_LIST_INCONSISTENT;
        }

        Off += Ea->NextEntryOffset;
    }
}

static BOOLEAN
ExnIsValidEaName (
    const UCHAR *Name,
    ULONG Length
    )
{
    ULONG i;
    UCHAR C;

    if (Length == 0 || Length > 255) {
        return FALSE;
    }
    for (i = 0; i < Length; i++) {
        C = Name[i];
        if (C < 0x20 || C == '"' || C == '*' || C == '+' || C == ',' || C == '/' || C == ':' ||
            C == ';' || C == '<' || C == '=' || C == '>' || C == '?' || C == '[' || C == '\\' ||
            C == ']' || C == '|') {
            return FALSE;
        }
    }
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* Setting                                                             */
/* ------------------------------------------------------------------ */

/*
 * Applies an NT EA buffer (already checked) to a file: with Replace, the
 * buffer becomes the file's whole list; without, its records are merged
 * in (a zero-length value removes that EA). The caller holds the file
 * exclusive.
 */
NTSTATUS
ExnSetEaBuffer (
    PEXN_VCB Vcb,
    PEXN_FCB Fcb,
    PFILE_FULL_EA_INFORMATION Buffer,
    ULONG Length,
    BOOLEAN Replace
    )
{
    PUCHAR Old = NULL;
    ULONG OldLength = 0;
    PUCHAR New = NULL;
    ULONG NewLength = 0;
    ULONG Room;
    ULONG Off, Pos, Size, i;
    UCHAR Name[256];
    PFILE_FULL_EA_INFORMATION Ea;
    PEXN_EA E;
        NTSTATUS Status;

    if (Buffer == NULL) {
        Length = 0;
    }

    if (Replace && Length == 0) {
        /* Nothing given: take away whatever is there */
        Status = ExnReadEaList(Vcb, Fcb, &Old, &OldLength);
        if (!NT_SUCCESS(Status)) {
            return Status;
        }
        if (Old == NULL) {
            return STATUS_SUCCESS;
        }
        ExtHostFree(Old);
        Old = NULL;
        OldLength = 0;
    } else if (!Replace) {
        Status = ExnReadEaList(Vcb, Fcb, &Old, &OldLength);
        if (!NT_SUCCESS(Status)) {
            return Status;
        }
    }

    /* The result is at most the old list plus every new record */
    Room = OldLength;
    if (Length != 0) {
        Off = 0;
        for (;;) {
            Ea = (PFILE_FULL_EA_INFORMATION)((PUCHAR)Buffer + Off);
            Room += sizeof(EXN_EA) + Ea->EaNameLength + 1 + Ea->EaValueLength;
            if (Ea->NextEntryOffset == 0) {
                break;
            }
            Off += Ea->NextEntryOffset;
        }
    }

    New = (PUCHAR)ExAllocatePoolWithTag(PagedPool, Room ? Room : 1, EXN_TAG_EA);
    if (New == NULL) {
        if (Old != NULL) ExtHostFree(Old);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    if (OldLength != 0) {
        RtlCopyMemory(New, Old, OldLength);
        NewLength = OldLength;
    }
    if (Old != NULL) {
        ExtHostFree(Old);
    }

    Status = STATUS_SUCCESS;
    Off = 0;
    while (Length != 0) {

        Ea = (PFILE_FULL_EA_INFORMATION)((PUCHAR)Buffer + Off);

        for (i = 0; i < Ea->EaNameLength; i++) {
            Name[i] = ExnEaUpper((UCHAR)Ea->EaName[i]);
        }

        if (!ExnIsValidEaName(Name, Ea->EaNameLength)) {
            Status = STATUS_INVALID_EA_NAME;
            break;
        }

        /* An EA of the same name goes first */
        Pos = ExnFindEa(Vcb, New, NewLength, Name, Ea->EaNameLength);
        if (Pos != EXN_UNKNOWN) {
            Size = ExnEaSize((PEXN_EA)(New + Pos));
            RtlMoveMemory(New + Pos, New + Pos + Size, NewLength - Pos - Size);
            NewLength -= Size;
        }

        if (Ea->EaValueLength != 0) {
            E = (PEXN_EA)(New + NewLength);
            E->Flags = 0;           /* xattrs carry no flags: FILE_NEED_EA is not kept */
            E->NameLength = Ea->EaNameLength;
            E->ValueLengthLo = (UCHAR)Ea->EaValueLength;
            E->ValueLengthHi = (UCHAR)(Ea->EaValueLength >> 8);
            RtlCopyMemory(E + 1, Name, Ea->EaNameLength);
            ((PUCHAR)(E + 1))[Ea->EaNameLength] = 0;
            RtlCopyMemory((PUCHAR)(E + 1) + Ea->EaNameLength + 1,
                          (PUCHAR)Ea->EaName + Ea->EaNameLength + 1, Ea->EaValueLength);
            NewLength += ExnEaSize(E);
        }

        if (Ea->NextEntryOffset == 0) {
            break;
        }
        Off += Ea->NextEntryOffset;
    }

    if (NT_SUCCESS(Status) && NewLength > EXN_MAX_EA_LIST) {
        Status = STATUS_EA_TOO_LARGE;
    }

    if (NT_SUCCESS(Status)) {
        Status = ExnWriteEaList(Vcb, Fcb, New, NewLength);
    }

    ExFreePool(New);

    if (NT_SUCCESS(Status)) {
        KeQuerySystemTime(&Fcb->ChangeTime);
    }
    return Status;
}

NTSTATUS
ExnCommonSetEa (
    PEXN_IRP_CONTEXT Ctx
    )
{
    PIRP Irp = Ctx->Irp;
    PIO_STACK_LOCATION IrpSp = Ctx->IrpSp;
    PFILE_OBJECT FileObject = IrpSp->FileObject;
    PEXN_FCB Fcb = (PEXN_FCB)FileObject->FsContext;
    PEXN_VCB Vcb = Ctx->Vcb;
    ULONG Length = IrpSp->Parameters.SetEa.Length;
    PVOID User;
    PFILE_FULL_EA_INFORMATION Buffer = NULL;
    ULONG ErrorOffset = 0;
    NTSTATUS Status = STATUS_SUCCESS;

    if (Fcb == NULL || ExnIsVfcb(Fcb)) {
        return STATUS_INVALID_PARAMETER;
    }

    if (Length == 0) {
        return STATUS_SUCCESS;
    }
    if (Length > 2 * EXN_MAX_EA_LIST) {
        return STATUS_EA_TOO_LARGE;
    }

    User = (Irp->AssociatedIrp.SystemBuffer != NULL) ? Irp->AssociatedIrp.SystemBuffer
                                                     : ExnMapUserBuffer(Irp);
    if (User == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    Buffer = (PFILE_FULL_EA_INFORMATION)ExAllocatePoolWithTag(PagedPool, Length, EXN_TAG_EA);
    if (Buffer == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Status = ExnSafeCopy(Buffer, User, Length);

    if (!NT_SUCCESS(Status)) {
        ExFreePool(Buffer);
        return Status;
    }

    Status = ExnCheckEaBuffer(Buffer, Length, &ErrorOffset);
    if (!NT_SUCCESS(Status)) {
        ExFreePool(Buffer);
        Irp->IoStatus.Information = ErrorOffset;
        return Status;
    }

    (VOID)ExAcquireResourceExclusiveLite(&Vcb->Resource, TRUE);
    (VOID)ExAcquireResourceExclusiveLite(&Fcb->Resource, TRUE);

    __try {

        Status = ExnVerifyWritable(Ctx, Vcb);
        if (!NT_SUCCESS(Status)) {
            __leave;
        }

        if (Fcb->FcbState & FCB_STATE_DELETED) {
            Status = STATUS_FILE_DELETED;
            __leave;
        }

        Status = ExnSetEaBuffer(Vcb, Fcb, Buffer, Length, FALSE);

        if (NT_SUCCESS(Status)) {
            FileObject->Flags |= FO_FILE_MODIFIED;
            ExnNotifyChange(Vcb, Fcb, FILE_NOTIFY_CHANGE_EA, FILE_ACTION_MODIFIED);
        }

    } __finally {

        ExnRelease(&Fcb->Resource);
        ExnRelease(&Vcb->Resource);
        ExFreePool(Buffer);
    }

    return Status;
}

/* ------------------------------------------------------------------ */
/* Querying                                                            */
/* ------------------------------------------------------------------ */

/* Appends one NT record at Out+*Used; FALSE if it does not fit */
static BOOLEAN
ExnPutNtEa (
    PUCHAR Out,
    ULONG Room,
    PULONG Used,
    PULONG Last,
    BOOLEAN First,
    UCHAR Flags,
    const UCHAR *Name,
    ULONG NameLength,
    const UCHAR *Value,
    ULONG ValueLength
    )
{
    ULONG Start = ExnLongAlign(*Used);
    ULONG Size = FIELD_OFFSET(FILE_FULL_EA_INFORMATION, EaName) + NameLength + 1 + ValueLength;
    PFILE_FULL_EA_INFORMATION Ea;

    if (Start > Room || Size > Room - Start) {
        return FALSE;
    }

    if (!First) {
        ((PFILE_FULL_EA_INFORMATION)(Out + *Last))->NextEntryOffset = Start - *Last;
    }

    Ea = (PFILE_FULL_EA_INFORMATION)(Out + Start);
    Ea->NextEntryOffset = 0;
    Ea->Flags = Flags;
    Ea->EaNameLength = (UCHAR)NameLength;
    Ea->EaValueLength = (USHORT)ValueLength;
    RtlCopyMemory(Ea->EaName, Name, NameLength);
    Ea->EaName[NameLength] = 0;
    if (ValueLength != 0) {
        RtlCopyMemory((PUCHAR)Ea->EaName + NameLength + 1, Value, ValueLength);
    }

    *Last = Start;
    *Used = Start + Size;
    return TRUE;
}

/* Records of the user's FILE_GET_EA_INFORMATION list, in its order */
static NTSTATUS
ExnQueryEaList (
    PEXN_VCB Vcb,
    const UCHAR *List,
    ULONG ListLength,
    PFILE_GET_EA_INFORMATION Get,
    ULONG GetLength,
    BOOLEAN Single,
    PUCHAR Out,
    ULONG Room,
    PULONG Used
    )
{
    ULONG Off = 0, Pos, Last = 0, i;
    BOOLEAN First = TRUE;
    PFILE_GET_EA_INFORMATION G;
    const EXN_EA *E;
    UCHAR Name[256];

    *Used = 0;

    for (;;) {

        if (Off > GetLength || GetLength - Off < FIELD_OFFSET(FILE_GET_EA_INFORMATION, EaName) + 1) {
            return STATUS_EA_LIST_INCONSISTENT;
        }
        G = (PFILE_GET_EA_INFORMATION)((PUCHAR)Get + Off);
        if (G->EaNameLength == 0 ||
            FIELD_OFFSET(FILE_GET_EA_INFORMATION, EaName) + G->EaNameLength + 1 > GetLength - Off) {
            return STATUS_EA_LIST_INCONSISTENT;
        }

        for (i = 0; i < G->EaNameLength; i++) {
            Name[i] = ExnEaUpper((UCHAR)G->EaName[i]);
        }
        if (!ExnIsValidEaName(Name, G->EaNameLength)) {
            return STATUS_INVALID_EA_NAME;
        }

        Pos = ExnFindEa(Vcb, List, ListLength, Name, G->EaNameLength);

        if (Pos == EXN_UNKNOWN) {
            /* Not there: the name with an empty value */
            if (!ExnPutNtEa(Out, Room, Used, &Last, First, 0, Name, G->EaNameLength, NULL, 0)) {
                *Used = 0;
                return STATUS_BUFFER_OVERFLOW;
            }
        } else {
            E = (const EXN_EA *)(List + Pos);
            if (!ExnPutNtEa(Out, Room, Used, &Last, First,
                             (UCHAR)((E->Flags & EXN_EA_NEEDEA) ? FILE_NEED_EA : 0),
                             (const UCHAR *)(E + 1), E->NameLength,
                             (const UCHAR *)(E + 1) + E->NameLength + 1, ExnEaValueLength(E))) {
                *Used = 0;
                return STATUS_BUFFER_OVERFLOW;
            }
        }
        First = FALSE;

        if (Single || G->NextEntryOffset == 0) {
            return STATUS_SUCCESS;
        }
        Off += G->NextEntryOffset;
    }
}

NTSTATUS
ExnCommonQueryEa (
    PEXN_IRP_CONTEXT Ctx
    )
{
    PIRP Irp = Ctx->Irp;
    PIO_STACK_LOCATION IrpSp = Ctx->IrpSp;
    PFILE_OBJECT FileObject = IrpSp->FileObject;
    PEXN_FCB Fcb = (PEXN_FCB)FileObject->FsContext;
    PEXN_CCB Ccb = (PEXN_CCB)FileObject->FsContext2;
    PEXN_VCB Vcb = Ctx->Vcb;
    ULONG Length = IrpSp->Parameters.QueryEa.Length;
    PVOID GetList = IrpSp->Parameters.QueryEa.EaList;
    ULONG GetLength = IrpSp->Parameters.QueryEa.EaListLength;
    ULONG EaIndex = IrpSp->Parameters.QueryEa.EaIndex;
    BOOLEAN Single = (BOOLEAN)((IrpSp->Flags & SL_RETURN_SINGLE_ENTRY) != 0);
    PUCHAR List = NULL;
    ULONG ListLength = 0;
    PUCHAR Out = NULL;
    PUCHAR Get = NULL;
    PUCHAR User;
    ULONG Used = 0, Last = 0, Count, Index, Off, Start;
    BOOLEAN First = TRUE;
    const EXN_EA *E;
    NTSTATUS Status = STATUS_SUCCESS;

    if (Fcb == NULL || Ccb == NULL || ExnIsVfcb(Fcb)) {
        return STATUS_INVALID_PARAMETER;
    }

    User = (PUCHAR)ExnMapUserBuffer(Irp);
    if (User == NULL && Length != 0) {
        return STATUS_INVALID_PARAMETER;
    }

    Out = (PUCHAR)ExAllocatePoolWithTag(PagedPool, Length ? Length : 1, EXN_TAG_EA);
    if (Out == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    if (GetList != NULL && GetLength != 0) {
        if (GetLength > 2 * EXN_MAX_EA_LIST) {
            ExFreePool(Out);
            return STATUS_EA_LIST_INCONSISTENT;
        }
        Get = (PUCHAR)ExAllocatePoolWithTag(PagedPool, GetLength, EXN_TAG_EA);
        if (Get == NULL) {
            ExFreePool(Out);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        Status = ExnSafeCopy(Get, GetList, GetLength);
        if (!NT_SUCCESS(Status)) {
            ExFreePool(Get);
            ExFreePool(Out);
            return Status;
        }
    }

    (VOID)ExAcquireResourceSharedLite(&Vcb->Resource, TRUE);
    (VOID)ExAcquireResourceExclusiveLite(&Fcb->Resource, TRUE);

    __try {

        Status = ExnVerifyVcb(Ctx, Vcb);
        if (!NT_SUCCESS(Status)) {
            __leave;
        }

        if (Fcb->FcbState & FCB_STATE_DELETED) {
            Status = STATUS_FILE_DELETED;
            __leave;
        }

        Status = ExnReadEaList(Vcb, Fcb, &List, &ListLength);
        if (!NT_SUCCESS(Status)) {
            __leave;
        }

        if (Get != NULL) {
            Status = ExnQueryEaList(Vcb, List, ListLength, (PFILE_GET_EA_INFORMATION)Get, GetLength,
                                     Single, Out, Length, &Used);
            __leave;
        }

        /* Count the records */
        Count = 0;
        for (Off = 0; Off + sizeof(EXN_EA) <= ListLength; Off += ExnEaSize((const EXN_EA *)(List + Off))) {
            Count++;
        }

        if (Count == 0) {
            Status = STATUS_NO_EAS_ON_FILE;
            __leave;
        }

        if (IrpSp->Flags & SL_INDEX_SPECIFIED) {
            /* 1 is the first */
            if (EaIndex == 0 || EaIndex > Count + 1) {
                Status = STATUS_NONEXISTENT_EA_ENTRY;
                __leave;
            }
            Start = EaIndex - 1;
        } else if (IrpSp->Flags & SL_RESTART_SCAN) {
            Start = 0;
        } else {
            Start = Ccb->EaIndex;
        }

        if (Start >= Count) {
            Status = STATUS_NO_MORE_EAS;
            __leave;
        }

        Index = 0;
        for (Off = 0; Off + sizeof(EXN_EA) <= ListLength; Off += ExnEaSize(E), Index++) {

            E = (const EXN_EA *)(List + Off);
            if (Index < Start) {
                continue;
            }

            if (!ExnPutNtEa(Out, Length, &Used, &Last, First,
                             (UCHAR)((E->Flags & EXN_EA_NEEDEA) ? FILE_NEED_EA : 0),
                             (const UCHAR *)(E + 1), E->NameLength,
                             (const UCHAR *)(E + 1) + E->NameLength + 1, ExnEaValueLength(E))) {
                Status = First ? STATUS_BUFFER_TOO_SMALL : STATUS_BUFFER_OVERFLOW;
                break;
            }

            First = FALSE;
            Ccb->EaIndex = Index + 1;

            if (Single) {
                break;
            }
        }

    } __finally {

        if (List != NULL) {
            ExtHostFree(List);
        }
        ExnRelease(&Fcb->Resource);
        ExnRelease(&Vcb->Resource);
    }

    if ((NT_SUCCESS(Status) || Status == STATUS_BUFFER_OVERFLOW) && Used != 0) {
        NTSTATUS Copy = ExnSafeCopy(User, Out, Used);
        if (!NT_SUCCESS(Copy)) {
            Status = Copy;
            Used = 0;
        }
    }

    Irp->IoStatus.Information = (NT_SUCCESS(Status) || Status == STATUS_BUFFER_OVERFLOW) ? Used : 0;

    if (Get != NULL) {
        ExFreePool(Get);
    }
    ExFreePool(Out);

    return Status;
}
