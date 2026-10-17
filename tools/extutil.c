/*
 * EXTNT - extutil: what Windows has no command for on an ext volume.
 *
 *   extutil stat FILE              mode, owner, group, links, inode, flags
 *   extutil chmod MODE FILE        permission bits (octal, e.g. 755)
 *   extutil chown UID[:GID] FILE   owner (and group)
 *   extutil symlink TARGET LINK    makes the symbolic link LINK -> TARGET
 *   extutil readlink LINK          shows where a symbolic link points
 *   extutil ea FILE [/A] [NAME=VALUE | NAME=]
 *                                  lists, sets or removes extended attributes
 *                                  (kept as Linux "user." xattrs)
 *   extutil ea FILE /TEST          exercises the EA interface (scratch file)
 *   extutil info D:                the volume: size, features, journal
 *   extutil sync D:                commits the journal now
 *
 * Runs on Windows NT 3.1 and later.
 */

#include <windows.h>
#include <winioctl.h>
#include "extctl.h"

typedef LONG NTSTATUS;
#define NT_SUCCESS(s)               ((NTSTATUS)(s) >= 0)

typedef struct _EX_UNICODE_STRING {
    USHORT Length;
    USHORT MaximumLength;
    PWSTR  Buffer;
} EX_UNICODE_STRING;

typedef struct _EX_OBJECT_ATTRIBUTES {
    ULONG Length;
    HANDLE RootDirectory;
    EX_UNICODE_STRING *ObjectName;
    ULONG Attributes;
    PVOID SecurityDescriptor;
    PVOID SecurityQualityOfService;
} EX_OBJECT_ATTRIBUTES;

typedef struct _EX_IO_STATUS_BLOCK {
    NTSTATUS Status;
    ULONG_PTR Information;
} EX_IO_STATUS_BLOCK;

typedef struct _EX_FULL_EA {
    ULONG NextEntryOffset;
    UCHAR Flags;
    UCHAR EaNameLength;
    USHORT EaValueLength;
    CHAR EaName[1];
} EX_FULL_EA;

typedef struct _EX_GET_EA {
    ULONG NextEntryOffset;
    UCHAR EaNameLength;
    CHAR EaName[1];
} EX_GET_EA;

#define EX_OBJ_CASE_INSENSITIVE     0x40
#define EX_FILE_OPEN                0x01
#define EX_FILE_OVERWRITE_IF        0x05
#define EX_FILE_SYNCHRONOUS_IO_NONALERT 0x20
#define EX_FILE_NON_DIRECTORY_FILE  0x40
#define EX_FILE_NEED_EA             0x80
#define EX_STATUS_NO_EAS_ON_FILE    ((NTSTATUS)0xC0000052L)
#define EX_STATUS_NO_MORE_EAS       ((NTSTATUS)0x80000012L)
#define EX_STATUS_BUFFER_OVERFLOW   ((NTSTATUS)0x80000005L)
#define EX_STATUS_BUFFER_TOO_SMALL  ((NTSTATUS)0xC0000023L)
#define EX_STATUS_NONEXISTENT_EA    ((NTSTATUS)0xC0000051L)

__declspec(dllimport) NTSTATUS NTAPI NtCreateFile(PHANDLE, ACCESS_MASK, EX_OBJECT_ATTRIBUTES *,
    EX_IO_STATUS_BLOCK *, PLARGE_INTEGER, ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);
__declspec(dllimport) NTSTATUS NTAPI NtClose(HANDLE);
__declspec(dllimport) NTSTATUS NTAPI NtQueryEaFile(HANDLE, EX_IO_STATUS_BLOCK *, PVOID, ULONG, BOOLEAN,
    PVOID, ULONG, PULONG, BOOLEAN);
__declspec(dllimport) NTSTATUS NTAPI NtSetEaFile(HANDLE, EX_IO_STATUS_BLOCK *, PVOID, ULONG);

int printf(const char *, ...);
int sprintf(char *, const char *, ...);

static UCHAR Buf[70000];
static int Failures;

/* \DosDevices\C:\full\path */
static NTSTATUS
OpenTarget (
    const char *Path,
    ULONG Disposition,
    PVOID Ea,
    ULONG EaLength,
    PHANDLE Handle
    )
{
    static WCHAR Full[600];
    static WCHAR Nt[700];
    WCHAR Wide[512];
    EX_UNICODE_STRING Name;
    EX_OBJECT_ATTRIBUTES Oa;
    EX_IO_STATUS_BLOCK Iosb;
    LPWSTR FilePart;
    DWORD Len;
    int i, j;

    MultiByteToWideChar(CP_ACP, 0, Path, -1, Wide, 512);
    Len = GetFullPathNameW(Wide, 600, Full, &FilePart);
    if (Len == 0 || Len >= 600) {
        return (NTSTATUS)0xC0000033L;
    }
    lstrcpyW(Nt, L"\\DosDevices\\");
    for (i = 0, j = lstrlenW(Nt); Full[i]; i++, j++) {
        Nt[j] = Full[i];
    }
    Nt[j] = 0;

    Name.Buffer = Nt;
    Name.Length = (USHORT)(j * sizeof(WCHAR));
    Name.MaximumLength = Name.Length + sizeof(WCHAR);

    Oa.Length = sizeof(Oa);
    Oa.RootDirectory = NULL;
    Oa.ObjectName = &Name;
    Oa.Attributes = EX_OBJ_CASE_INSENSITIVE;
    Oa.SecurityDescriptor = NULL;
    Oa.SecurityQualityOfService = NULL;

    return NtCreateFile(Handle, GENERIC_READ | GENERIC_WRITE | SYNCHRONIZE | 0x10 | 0x08,
                        &Oa, &Iosb, NULL, FILE_ATTRIBUTE_NORMAL,
                        FILE_SHARE_READ | FILE_SHARE_WRITE, Disposition,
                        EX_FILE_SYNCHRONOUS_IO_NONALERT, Ea, EaLength);
}

static void
ShowValue (
    const UCHAR *V,
    ULONG Length
    )
{
    ULONG i;
    BOOL Text = TRUE;

    /* OS/2 EAT_ASCII: FFFD, length, text */
    if (Length >= 4 && V[0] == 0xFD && V[1] == 0xFF && (ULONG)(V[2] | (V[3] << 8)) == Length - 4) {
        printf("ASCII \"");
        for (i = 4; i < Length; i++) printf("%c", V[i]);
        printf("\"");
        return;
    }
    for (i = 0; i < Length; i++) {
        if (V[i] < 0x20 || V[i] > 0x7E) Text = FALSE;
    }
    if (Text) {
        printf("\"");
        for (i = 0; i < Length; i++) printf("%c", V[i]);
        printf("\"");
    } else {
        for (i = 0; i < Length && i < 32; i++) printf("%02X ", V[i]);
        if (Length > 32) printf("...");
    }
}

/* Prints a FILE_FULL_EA_INFORMATION list; returns the number of records */
static int
ShowList (
    const UCHAR *L
    )
{
    const EX_FULL_EA *E;
    int Count = 0;

    for (;;) {
        E = (const EX_FULL_EA *)L;
        printf("  %s%-24s %5u  ", (E->Flags & EX_FILE_NEED_EA) ? "!" : " ", E->EaName, E->EaValueLength);
        ShowValue((const UCHAR *)E->EaName + E->EaNameLength + 1, E->EaValueLength);
        printf("\n");
        Count++;
        if (E->NextEntryOffset == 0) break;
        L += E->NextEntryOffset;
    }
    return Count;
}

static ULONG
MakeEa (
    UCHAR *Out,
    const char *Name,
    const UCHAR *Value,
    ULONG ValueLength,
    BOOL Ascii,
    UCHAR Flags
    )
{
    EX_FULL_EA *E = (EX_FULL_EA *)Out;
    ULONG N = lstrlenA(Name);
    UCHAR *V;

    E->NextEntryOffset = 0;
    E->Flags = Flags;
    E->EaNameLength = (UCHAR)N;
    E->EaValueLength = (USHORT)(ValueLength + (Ascii && ValueLength ? 4 : 0));
    lstrcpyA(E->EaName, Name);
    V = (UCHAR *)E->EaName + N + 1;
    if (Ascii && ValueLength) {
        V[0] = 0xFD; V[1] = 0xFF; V[2] = (UCHAR)ValueLength; V[3] = (UCHAR)(ValueLength >> 8);
        V += 4;
    }
    CopyMemory(V, Value, ValueLength);
    return 8 + N + 1 + E->EaValueLength;
}

static void
Check (
    int Ok,
    const char *What,
    NTSTATUS Status
    )
{
    printf("%s %s (status %08lX)\n", Ok ? "ok  " : "FAIL", What, (ULONG)Status);
    if (!Ok) Failures++;
}

static int
SelfTest (
    const char *Path
    )
{
    HANDLE H;
    EX_IO_STATUS_BLOCK Iosb;
    NTSTATUS S;
    UCHAR In[600];
    ULONG Len, L2, Off;
    EX_GET_EA *G;
    UCHAR Big[3000];
    int i;

    /* Create with two EAs, one of them NEED_EA */
    Len = MakeEa(In, "first.ea", (const UCHAR *)"one", 3, FALSE, 0);
    Len = (Len + 3) & ~3UL;
    ((EX_FULL_EA *)In)->NextEntryOffset = Len;
    Len += MakeEa(In + Len, "Second", (const UCHAR *)"two two", 7, TRUE, EX_FILE_NEED_EA);
    S = OpenTarget(Path, EX_FILE_OVERWRITE_IF, In, Len, &H);
    Check(NT_SUCCESS(S), "create with an EA buffer", S);
    if (!NT_SUCCESS(S)) return 1;

    S = NtQueryEaFile(H, &Iosb, Buf, sizeof(Buf), FALSE, NULL, 0, NULL, TRUE);
    Check(NT_SUCCESS(S) && ShowList(Buf) == 2, "query all", S);
    Check(lstrcmpA(((EX_FULL_EA *)Buf)->EaName, "FIRST.EA") == 0, "names in upper case", 0);

    S = NtQueryEaFile(H, &Iosb, Buf, sizeof(Buf), TRUE, NULL, 0, NULL, TRUE);
    Check(NT_SUCCESS(S) && ((EX_FULL_EA *)Buf)->NextEntryOffset == 0, "single entry (restart)", S);
    S = NtQueryEaFile(H, &Iosb, Buf, sizeof(Buf), TRUE, NULL, 0, NULL, FALSE);
    Check(NT_SUCCESS(S) && lstrcmpA(((EX_FULL_EA *)Buf)->EaName, "SECOND") == 0,
          "single entry continues", S);
    S = NtQueryEaFile(H, &Iosb, Buf, sizeof(Buf), TRUE, NULL, 0, NULL, FALSE);
    Check(S == EX_STATUS_NO_MORE_EAS, "then no more EAs", S);

    i = 2;
    S = NtQueryEaFile(H, &Iosb, Buf, sizeof(Buf), TRUE, NULL, 0, (PULONG)&i, FALSE);
    Check(NT_SUCCESS(S) && lstrcmpA(((EX_FULL_EA *)Buf)->EaName, "SECOND") == 0, "by index 2", S);
    i = 9;
    S = NtQueryEaFile(H, &Iosb, Buf, sizeof(Buf), TRUE, NULL, 0, (PULONG)&i, FALSE);
    Check(S == EX_STATUS_NONEXISTENT_EA, "index 9 does not exist", S);

    S = NtQueryEaFile(H, &Iosb, Buf, 12, FALSE, NULL, 0, NULL, TRUE);
    Check(S == EX_STATUS_BUFFER_TOO_SMALL, "tiny buffer", S);

    /* A list of names, one missing */
    G = (EX_GET_EA *)In;
    G->NextEntryOffset = 12;
    G->EaNameLength = 6; lstrcpyA(G->EaName, "second");
    G = (EX_GET_EA *)(In + 12);
    G->NextEntryOffset = 0;
    G->EaNameLength = 4; lstrcpyA(G->EaName, "NONE");
    S = NtQueryEaFile(H, &Iosb, Buf, sizeof(Buf), FALSE, In, 24, NULL, TRUE);
    Check(NT_SUCCESS(S) && ShowList(Buf) == 2 &&
          ((EX_FULL_EA *)(Buf + ((EX_FULL_EA *)Buf)->NextEntryOffset))->EaValueLength == 0,
          "query by name list", S);

    /* Replace one, remove one, add a large one (stored outside the fnode) */
    Len = MakeEa(In, "SECOND", (const UCHAR *)"", 0, FALSE, 0);
    S = NtSetEaFile(H, &Iosb, In, Len);
    Check(NT_SUCCESS(S), "remove SECOND", S);
    for (i = 0; i < (int)sizeof(Big); i++) Big[i] = (UCHAR)(i * 7);
    Len = MakeEa(Buf, "BIG.VALUE", Big, sizeof(Big), FALSE, 0);
    S = NtSetEaFile(H, &Iosb, Buf, Len);
    Check(NT_SUCCESS(S), "add a 3000 byte value (an xattr outside the inode)", S);
    Len = MakeEa(In, "first.ea", (const UCHAR *)"replaced", 8, FALSE, 0);
    S = NtSetEaFile(H, &Iosb, In, Len);
    Check(NT_SUCCESS(S), "replace first.ea", S);

    S = NtQueryEaFile(H, &Iosb, Buf, sizeof(Buf), FALSE, NULL, 0, NULL, TRUE);
    Check(NT_SUCCESS(S) && ShowList(Buf) == 2, "now two EAs", S);
    for (Off = 0, L2 = 0;;) {
        EX_FULL_EA *E = (EX_FULL_EA *)(Buf + Off);
        if (lstrcmpA(E->EaName, "BIG.VALUE") == 0 && E->EaValueLength == sizeof(Big)) {
            L2 = (memcmp((UCHAR *)E->EaName + E->EaNameLength + 1, Big, sizeof(Big)) == 0);
        }
        if (E->NextEntryOffset == 0) break;
        Off += E->NextEntryOffset;
    }
    Check(L2 != 0, "large value reads back", 0);

    /* Bad name */
    Len = MakeEa(In, "A*B", (const UCHAR *)"x", 1, FALSE, 0);
    S = NtSetEaFile(H, &Iosb, In, Len);
    Check(!NT_SUCCESS(S), "illegal name refused", S);

    NtClose(H);

    /* Overwrite with no EA buffer drops them */
    S = OpenTarget(Path, EX_FILE_OVERWRITE_IF, NULL, 0, &H);
    Check(NT_SUCCESS(S), "overwrite", S);
    S = NtQueryEaFile(H, &Iosb, Buf, sizeof(Buf), FALSE, NULL, 0, NULL, TRUE);
    Check(S == EX_STATUS_NO_EAS_ON_FILE, "no EAs after overwrite", S);
    NtClose(H);

    printf("%s: %d failure(s)\n", Failures ? "FAILED" : "PASSED", Failures);
    return Failures ? 1 : 0;
}


/* ------------------------------------------------------------------ */
/* POSIX information, links, the volume                               */
/* ------------------------------------------------------------------ */

static HANDLE
OpenPath(const char *Path, DWORD Access, BOOL AsLink)
{
    /* DELETE access opens a symbolic link itself instead of its target */
    return CreateFileA(Path, Access | (AsLink ? DELETE : 0), FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
}

static int
Control(HANDLE H, DWORD Code, PVOID In, DWORD InLen, PVOID Out, DWORD OutLen, DWORD *Got)
{
    DWORD N = 0;
    BOOL Ok = DeviceIoControl(H, Code, In, InLen, Out, OutLen, &N, NULL);
    if (Got) *Got = N;
    return Ok ? 0 : (int)GetLastError();
}

static void
ModeString(ULONG Mode, char *S)
{
    static const char T[] = "?pc?d?b?-?l?s???";
    static const char R[] = "rwxrwxrwx";
    int i;
    S[0] = T[(Mode >> 12) & 15];
    for (i = 0; i < 9; i++) S[1 + i] = (Mode & (0400 >> i)) ? R[i] : '-';
    if (Mode & 04000) S[3] = (Mode & 0100) ? 's' : 'S';
    if (Mode & 02000) S[6] = (Mode & 010) ? 's' : 'S';
    if (Mode & 01000) S[9] = (Mode & 01) ? 't' : 'T';
    S[10] = 0;
}

static int
CmdStat(const char *Path)
{
    EXN_POSIX_INFO P;
    char M[12];
    HANDLE H = OpenPath(Path, FILE_READ_ATTRIBUTES, TRUE);
    int E;
    if (H == INVALID_HANDLE_VALUE) H = OpenPath(Path, FILE_READ_ATTRIBUTES, FALSE);
    if (H == INVALID_HANDLE_VALUE) { printf("%s: cannot open (error %lu)\n", Path, GetLastError()); return 1; }
    E = Control(H, EXN_FSCTL_GET_POSIX, NULL, 0, &P, sizeof(P), NULL);
    CloseHandle(H);
    if (E) { printf("%s: not on an ext volume (error %d)\n", Path, E); return 1; }
    ModeString(P.Mode, M);
    printf("%s\n  mode %s (%lo)  owner %lu  group %lu  links %lu  inode %lu  flags %08lX\n",
           Path, M, P.Mode & 07777, P.Uid, P.Gid, P.Links, P.Inode, P.Flags);
    return 0;
}

static int
CmdSet(const char *Path, ULONG Valid, ULONG Mode, ULONG Uid, ULONG Gid)
{
    EXN_POSIX_INFO P;
    HANDLE H = OpenPath(Path, FILE_WRITE_ATTRIBUTES | FILE_READ_ATTRIBUTES, FALSE);
    int E;
    if (H == INVALID_HANDLE_VALUE) { printf("%s: cannot open (error %lu)\n", Path, GetLastError()); return 1; }
    ZeroMemory(&P, sizeof(P));
    P.Valid = Valid; P.Mode = Mode; P.Uid = Uid; P.Gid = Gid;
    E = Control(H, EXN_FSCTL_SET_POSIX, &P, sizeof(P), NULL, 0, NULL);
    CloseHandle(H);
    if (E) { printf("%s: cannot change (error %d)\n", Path, E); return 1; }
    return CmdStat(Path);
}

static ULONG
Octal(const char *S)
{
    ULONG V = 0;
    while (*S >= '0' && *S <= '7') V = V * 8 + (ULONG)(*S++ - '0');
    return V;
}

static ULONG
Decimal(const char **S)
{
    ULONG V = 0;
    while (**S >= '0' && **S <= '9') V = V * 10 + (ULONG)(*(*S)++ - '0');
    return V;
}

static int
CmdSymlink(const char *Target, const char *Link)
{
    char Dir[MAX_PATH];
    char *Slash, *Name;
    static UCHAR Req[4096];
    PEXN_LINK_REQUEST R = (PEXN_LINK_REQUEST)Req;
    WCHAR WName[300];
    int NameLen, TLen, E;
    HANDLE H;

    {
        int k;
        for (k = 0; k < MAX_PATH - 1 && Link[k]; k++) Dir[k] = Link[k];
        Dir[k] = 0;
    }
    Slash = Dir + lstrlenA(Dir);
    while (Slash > Dir && Slash[-1] != '\\' && Slash[-1] != '/' && Slash[-1] != ':') Slash--;
    Name = (char *)Link + (Slash - Dir);
    if (Slash == Dir) lstrcpyA(Dir, ".");
    else if (Slash[-1] == ':') { Slash[0] = '.'; Slash[1] = 0; }
    else Slash[-1] = 0;
    if (Dir[0] && Dir[lstrlenA(Dir) - 1] == ':') lstrcatA(Dir, "\\");

    NameLen = MultiByteToWideChar(CP_ACP, 0, Name, -1, WName, 300) - 1;
    /* the target is stored as given, in UTF-8; backslashes become slashes */
    {
        WCHAR WT[1024];
        int i, n = MultiByteToWideChar(CP_ACP, 0, Target, -1, WT, 1024) - 1;
        UCHAR *T = Req + FIELD_OFFSET(EXN_LINK_REQUEST, Name) + NameLen * 2;
        TLen = 0;
        for (i = 0; i < n; i++) {
            ULONG C = WT[i] == L'\\' ? L'/' : WT[i];
            if (C < 0x80) T[TLen++] = (UCHAR)C;
            else if (C < 0x800) { T[TLen++] = (UCHAR)(0xC0 | (C >> 6)); T[TLen++] = (UCHAR)(0x80 | (C & 0x3F)); }
            else { T[TLen++] = (UCHAR)(0xE0 | (C >> 12)); T[TLen++] = (UCHAR)(0x80 | ((C >> 6) & 0x3F)); T[TLen++] = (UCHAR)(0x80 | (C & 0x3F)); }
        }
    }
    if (NameLen <= 0 || TLen <= 0) { printf("bad name or target\n"); return 2; }
    R->NameLength = (USHORT)(NameLen * 2);
    R->TargetLength = (USHORT)TLen;
    CopyMemory(R->Name, WName, NameLen * 2);

    H = CreateFileA(Dir, FILE_READ_ATTRIBUTES | FILE_WRITE_DATA | FILE_ADD_FILE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (H == INVALID_HANDLE_VALUE) { printf("%s: cannot open the directory (error %lu)\n", Dir, GetLastError()); return 1; }
    E = Control(H, EXN_FSCTL_CREATE_LINK, Req, FIELD_OFFSET(EXN_LINK_REQUEST, Name) + NameLen * 2 + TLen, NULL, 0, NULL);
    CloseHandle(H);
    if (E) { printf("%s: cannot make the link (error %d)\n", Link, E); return 1; }
    printf("%s -> %s\n", Link, Target);
    return 0;
}

static int
CmdReadlink(const char *Link)
{
    static char Text[4100];
    DWORD Got = 0;
    HANDLE H = OpenPath(Link, FILE_READ_ATTRIBUTES, TRUE);
    int E;
    if (H == INVALID_HANDLE_VALUE) { printf("%s: cannot open (error %lu)\n", Link, GetLastError()); return 1; }
    E = Control(H, EXN_FSCTL_READ_LINK, NULL, 0, Text, sizeof(Text) - 1, &Got);
    CloseHandle(H);
    if (E) { printf("%s: not a symbolic link (error %d)\n", Link, E); return 1; }
    Text[Got] = 0;
    printf("%s -> %s\n", Link, Text);
    return 0;
}

static HANDLE
OpenVolume(const char *Drive)
{
    char Root[8];
    Root[0] = Drive[0]; Root[1] = ':'; Root[2] = '\\'; Root[3] = 0;
    return CreateFileA(Root, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                       FILE_FLAG_BACKUP_SEMANTICS, NULL);
}

static int
CmdInfo(const char *Drive)
{
    EXN_VOLUME_INFO V;
    HANDLE H = OpenVolume(Drive);
    int E, i;
    if (H == INVALID_HANDLE_VALUE) { printf("%s: cannot open (error %lu)\n", Drive, GetLastError()); return 1; }
    E = Control(H, EXN_FSCTL_QUERY_VOLUME, NULL, 0, &V, sizeof(V), NULL);
    CloseHandle(H);
    if (E) { printf("%c: is not an ext volume (error %d)\n", Drive[0], E); return 1; }
    printf("%c: label \"%s\"  uuid ", Drive[0], V.Label);
    for (i = 0; i < 16; i++) printf("%02x%s", V.Uuid[i], (i == 3 || i == 5 || i == 7 || i == 9) ? "-" : "");
    printf("\n  %lu-byte blocks: %lu MB, %lu MB free\n", V.BlockSize,
           (ULONG)((V.BlocksCount * V.BlockSize) >> 20), (ULONG)((V.FreeBlocks * V.BlockSize) >> 20));
    printf("  %lu inodes of %lu bytes (%lu free), %lu groups\n", V.InodesCount, V.InodeSize, V.FreeInodes, V.GroupCount);
    printf("  features: compat %08lX incompat %08lX ro_compat %08lX\n", V.FeatureCompat, V.FeatureIncompat, V.FeatureRoCompat);
    printf("  %s%s, journal: %s (%lu commits)\n", V.ReadOnly ? "read-only" : "read/write",
           V.ReadOnlyForced ? " (features EXTNT cannot write)" : "", V.HasJournal ? "yes" : "no", V.Commits);
    return 0;
}

static int
CmdSync(const char *Drive)
{
    HANDLE H = OpenVolume(Drive);
    int E;
    if (H == INVALID_HANDLE_VALUE) { printf("%s: cannot open (error %lu)\n", Drive, GetLastError()); return 1; }
    E = Control(H, EXN_FSCTL_COMMIT, NULL, 0, NULL, 0, NULL);
    CloseHandle(H);
    if (E) { printf("%c: cannot commit (error %d)\n", Drive[0], E); return 1; }
    printf("%c: committed\n", Drive[0]);
    return 0;
}

static int
CmdEa(int argc, char **argv)
{
    HANDLE H;
    EX_IO_STATUS_BLOCK Iosb;
    NTSTATUS S;
    BOOL Ascii = FALSE;
    int Arg = 1;
    char *Eq;
    ULONG Len;

    if (argc > 1 && lstrcmpiA(argv[1], "/TEST") == 0) return SelfTest(argv[0]);
    if (argc > Arg && lstrcmpiA(argv[Arg], "/A") == 0) { Ascii = TRUE; Arg++; }

    S = OpenTarget(argv[0], EX_FILE_OPEN, NULL, 0, &H);
    if (!NT_SUCCESS(S)) { printf("%s: cannot open (status %08lX)\n", argv[0], (ULONG)S); return 1; }
    if (argc > Arg) {
        Eq = argv[Arg];
        while (*Eq && *Eq != '=') Eq++;
        if (*Eq != '=') { printf("expected NAME=VALUE\n"); NtClose(H); return 2; }
        *Eq++ = 0;
        Len = MakeEa(Buf, argv[Arg], (const UCHAR *)Eq, lstrlenA(Eq), Ascii, 0);
        S = NtSetEaFile(H, &Iosb, Buf, Len);
        if (!NT_SUCCESS(S)) { printf("cannot set the EA (status %08lX)\n", (ULONG)S); NtClose(H); return 1; }
    }
    S = NtQueryEaFile(H, &Iosb, Buf, sizeof(Buf), FALSE, NULL, 0, NULL, TRUE);
    if (S == EX_STATUS_NO_EAS_ON_FILE) printf("%s: no extended attributes\n", argv[0]);
    else if (!NT_SUCCESS(S)) printf("%s: cannot read the EAs (status %08lX)\n", argv[0], (ULONG)S);
    else { printf("%s:\n", argv[0]); ShowList(Buf); }
    NtClose(H);
    return NT_SUCCESS(S) || S == EX_STATUS_NO_EAS_ON_FILE ? 0 : 1;
}

static int
Usage(void)
{
    printf("extutil stat FILE | chmod MODE FILE | chown UID[:GID] FILE\n"
           "        symlink TARGET LINK | readlink LINK\n"
           "        ea FILE [/A] [NAME=VALUE | NAME= | /TEST]\n"
           "        info D: | sync D:\n");
    return 2;
}

int
main(int argc, char **argv)
{
    const char *C;
    if (argc < 3) return Usage();
    C = argv[1];
    if (!lstrcmpiA(C, "stat")) return CmdStat(argv[2]);
    if (!lstrcmpiA(C, "chmod") && argc >= 4) return CmdSet(argv[3], EXN_POSIX_MODE, Octal(argv[2]), 0, 0);
    if (!lstrcmpiA(C, "chown") && argc >= 4) {
        const char *P = argv[2];
        ULONG Uid = Decimal(&P), Gid = 0, Valid = EXN_POSIX_UID;
        if (*P == ':' || *P == '.') { P++; Gid = Decimal(&P); Valid |= EXN_POSIX_GID; }
        return CmdSet(argv[3], Valid, 0, Uid, Gid);
    }
    if (!lstrcmpiA(C, "symlink") && argc >= 4) return CmdSymlink(argv[2], argv[3]);
    if (!lstrcmpiA(C, "readlink")) return CmdReadlink(argv[2]);
    if (!lstrcmpiA(C, "ea")) return CmdEa(argc - 2, argv + 2);
    if (!lstrcmpiA(C, "info")) return CmdInfo(argv[2]);
    if (!lstrcmpiA(C, "sync")) return CmdSync(argv[2]);
    return Usage();
}
