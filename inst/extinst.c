/*
 * EXTNT - extinst: installs or removes the EXTNT file system driver.
 *
 *   extinst                Install: copy extnt.sys (from the folder extinst.exe
 *                          is in) to System32\drivers and create the Extnt
 *                          service (system start, group "File system").
 *   extinst /BOOT          Boot start ("Boot file system"), for running
 *                          Windows NT from an ext volume (32-bit Windows;
 *                          the volume is then prepared with extboot).
 *   extinst /READONLY      Mount ext volumes read-only.
 *   extinst /NOLETTERS     Do not give Linux partitions drive letters.
 *   extinst /CASE          Case-sensitive names (exact case only).
 *   extinst /SHOWDOT       Do not hide names starting with a dot.
 *   extinst /UID:n /GID:n  Owner of the files EXTNT creates (default 0, root).
 *   extinst /U             Uninstall: disable the service, remove the driver.
 *
 * A restart applies the change. Runs on Windows NT 3.1 and later; on 64-bit
 * Windows use the 64-bit extinst and extnt.sys.
 */

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define SVC_EXTNT           "SYSTEM\\CurrentControlSet\\Services\\Extnt"

#define SERVICE_BOOT_START      0
#define SERVICE_SYSTEM_START    1
#define SERVICE_DISABLED        4
#define SERVICE_FILE_SYSTEM     2

static LONG SetDword(HKEY Key, const char *Name, DWORD Value)
{
    return RegSetValueExA(Key, Name, 0, REG_DWORD, (const BYTE *)&Value, sizeof(Value));
}

static LONG SetSz(HKEY Key, const char *Name, const char *Value)
{
    return RegSetValueExA(Key, Name, 0, REG_SZ, (const BYTE *)Value, (DWORD)(strlen(Value) + 1));
}

/*
 * Make "EXTNT" a known load-order group placed last, by appending it to
 * HKLM\SYSTEM\CurrentControlSet\Control\ServiceGroupOrder\List (a REG_MULTI_SZ).
 * The boot loader loads boot-start drivers group by group in this order, and
 * NT 3.1's loader will not load a boot-start driver whose group is not listed.
 * Placing EXTNT last means extnt registers after the FAT driver and so is
 * offered - and claims - the ext boot volume before FAT recognizes its view.
 */
static void AddGroupOrder(void)
{
    HKEY Key;
    DWORD Type = 0, Size = 0;
    char *List, *P;
    LONG R;

    R = RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                      "SYSTEM\\CurrentControlSet\\Control\\ServiceGroupOrder",
                      0, KEY_READ | KEY_WRITE, &Key);
    if (R != ERROR_SUCCESS) {
        return;
    }
    RegQueryValueExA(Key, "List", NULL, &Type, NULL, &Size);
    if (Type != REG_MULTI_SZ || Size == 0) {
        Size = 0;
    }
    List = (char *)malloc(Size + 32);
    if (List == NULL) {
        RegCloseKey(Key);
        return;
    }
    if (Size) {
        RegQueryValueExA(Key, "List", NULL, &Type, (BYTE *)List, &Size);
        if (Size >= 2 && List[Size - 1] == 0 && List[Size - 2] == 0) {
            Size--;     /* drop one of the two terminators; we re-add it */
        }
    } else {
        Size = 1; List[0] = 0;
    }
    for (P = List; *P; P += strlen(P) + 1) {
        if (_stricmp(P, "EXTNT") == 0) {    /* already present */
            RegCloseKey(Key);
            free(List);
            return;
        }
    }
    strcpy(List + Size, "EXTNT");
    Size += (DWORD)strlen("EXTNT") + 1;
    List[Size++] = 0;                       /* final terminator */
    RegSetValueExA(Key, "List", 0, REG_MULTI_SZ, (const BYTE *)List, Size);
    RegCloseKey(Key);
    free(List);
}

/* TRUE if the file is EXTNT's driver (it carries this string) */
static int IsExtnt(const char *Path)
{
    static const char Mark[] = "EXTNT file system driver";
    HANDLE H;
    DWORD Size, Got, I, N = sizeof(Mark) - 1;
    char *Buf;
    int Found = 0;

    H = CreateFileA(Path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (H == INVALID_HANDLE_VALUE) return 0;
    Size = GetFileSize(H, NULL);
    if (Size != 0xFFFFFFFF && Size < 4 * 1024 * 1024) {
        Buf = (char *)HeapAlloc(GetProcessHeap(), 0, Size + 1);
        if (Buf != NULL) {
            if (ReadFile(H, Buf, Size, &Got, NULL) && Got == Size) {
                for (I = 0; I + N <= Size && !Found; I++) {
                    if (Buf[I] == Mark[0] && memcmp(Buf + I, Mark, N) == 0) Found = 1;
                }
            }
            HeapFree(GetProcessHeap(), 0, Buf);
        }
    }
    CloseHandle(H);
    return Found;
}

static void DriversPath(char *Out)
{
    char Dir[MAX_PATH];
    UINT N = GetWindowsDirectoryA(Dir, sizeof(Dir));
    if (N == 0 || N >= sizeof(Dir)) {
        Out[0] = 0;
        return;
    }
    sprintf(Out, "%s\\System32\\drivers\\extnt.sys", Dir);
}

/* extnt.sys next to this program, else in the current folder */
static void SourcePath(char *Out)
{
    char *Slash;
    DWORD N = GetModuleFileNameA(NULL, Out, MAX_PATH);
    if (N == 0 || N >= MAX_PATH) {
        strcpy(Out, "extnt.sys");
        return;
    }
    Slash = strrchr(Out, '\\');
    if (Slash == NULL) {
        strcpy(Out, "extnt.sys");
        return;
    }
    strcpy(Slash + 1, "extnt.sys");
    if (GetFileAttributesA(Out) == 0xFFFFFFFF) strcpy(Out, "extnt.sys");
}

typedef struct {
    int Boot, ReadOnly, NoLetters, Case, ShowDot;
    long Uid, Gid;
} OPTIONS;

static int Install(OPTIONS *O)
{
    char Sys[MAX_PATH], Src[MAX_PATH];
    HKEY Key;
    DWORD Disp, Start;
    LONG R;

    DriversPath(Sys);
    SourcePath(Src);
    if (Sys[0] == 0) {
        printf("  cannot find the system directory.\n");
        return 1;
    }
    if (!IsExtnt(Src)) {
        printf("  error: %s is not EXTNT's driver.\n", Src);
        printf("  put extnt.sys next to extinst.exe and run it again.\n");
        return 1;
    }
    if (!CopyFileA(Src, Sys, FALSE)) {
        printf("  error: could not copy %s to %s (error %lu).\n", Src, Sys, GetLastError());
        return 1;
    }
    printf("  installed %s\n", Sys);

    R = RegCreateKeyExA(HKEY_LOCAL_MACHINE, SVC_EXTNT, 0, NULL, REG_OPTION_NON_VOLATILE,
                        KEY_ALL_ACCESS, NULL, &Key, &Disp);
    if (R != ERROR_SUCCESS) {
        printf("  error: cannot create the Extnt service key (error %ld).\n", R);
        return 1;
    }

    Start = O->Boot ? SERVICE_BOOT_START : SERVICE_SYSTEM_START;
    SetDword(Key, "Type", SERVICE_FILE_SYSTEM);
    SetDword(Key, "ErrorControl", 1);
    SetSz(Key, "ImagePath", "System32\\DRIVERS\\extnt.sys");
    SetDword(Key, "Start", Start);
    SetSz(Key, "Group", Start == SERVICE_BOOT_START ? "EXTNT" : "File system");
    SetDword(Key, "EnableWriteSupport", O->ReadOnly ? 0 : 1);
    SetDword(Key, "AssignDriveLetters", O->NoLetters ? 0 : 1);
    SetDword(Key, "CaseSensitive", O->Case ? 1 : 0);
    SetDword(Key, "HideDotFiles", O->ShowDot ? 0 : 1);
    if (O->Uid >= 0) SetDword(Key, "DefaultUid", (DWORD)O->Uid);
    if (O->Gid >= 0) SetDword(Key, "DefaultGid", (DWORD)O->Gid);
    RegFlushKey(Key);
    RegCloseKey(Key);

    if (Start == SERVICE_BOOT_START) {
        AddGroupOrder();
    }

    printf("  Extnt service: %s start, %s, %s names%s\n",
           Start == SERVICE_BOOT_START ? "boot" : "system",
           O->ReadOnly ? "read-only" : "read/write",
           O->Case ? "case-sensitive" : "case-preserving",
           O->NoLetters ? ", no drive letters" : "");
    printf("Done. Restart to load EXTNT.\n");
    return 0;
}

static int Uninstall(void)
{
    char Sys[MAX_PATH];
    HKEY Key;

    DriversPath(Sys);
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, SVC_EXTNT, 0, KEY_ALL_ACCESS, &Key) == ERROR_SUCCESS) {
        SetDword(Key, "Start", SERVICE_DISABLED);
        RegFlushKey(Key);
        RegCloseKey(Key);
        printf("  disabled the Extnt service\n");
    }
    /* the running driver keeps its file until the restart; remove it then */
    if (Sys[0] && IsExtnt(Sys) && !DeleteFileA(Sys)) {
        printf("  %s is in use; it stays until it is removed by hand.\n", Sys);
    }
    printf("Done. Restart to apply.\n");
    return 0;
}

/* A 32-bit installer on 64-bit Windows would write into SysWOW64 */
static int UnderWow64(void)
{
    typedef BOOL (WINAPI *ISWOW64)(HANDLE, PBOOL);
    HMODULE K = GetModuleHandleA("kernel32.dll");
    ISWOW64 F = K ? (ISWOW64)GetProcAddress(K, "IsWow64Process") : NULL;
    BOOL W = FALSE;
    return F != NULL && F(GetCurrentProcess(), &W) && W;
}

static int Usage(void)
{
    printf("usage: extinst [/BOOT] [/READONLY] [/NOLETTERS] [/CASE] [/SHOWDOT] [/UID:n] [/GID:n]\n"
           "       extinst /U\n");
    return 2;
}

int main(int argc, char **argv)
{
    OPTIONS O;
    int I, Remove = 0;

    memset(&O, 0, sizeof(O));
    O.Uid = O.Gid = -1;
    for (I = 1; I < argc; I++) {
        const char *A = argv[I];
        if (A[0] != '/' && A[0] != '-') return Usage();
        A++;
        if (!_stricmp(A, "U")) Remove = 1;
        else if (!_stricmp(A, "BOOT")) O.Boot = 1;
        else if (!_stricmp(A, "READONLY")) O.ReadOnly = 1;
        else if (!_stricmp(A, "NOLETTERS")) O.NoLetters = 1;
        else if (!_stricmp(A, "CASE")) O.Case = 1;
        else if (!_stricmp(A, "SHOWDOT")) O.ShowDot = 1;
        else if (!_strnicmp(A, "UID:", 4)) O.Uid = atol(A + 4);
        else if (!_strnicmp(A, "GID:", 4)) O.Gid = atol(A + 4);
        else return Usage();
    }

    printf("EXTNT installer\n");
#ifdef _WIN64
    if (O.Boot) {
        printf("  Starting Windows from an ext volume is not supported on 64-bit Windows\n"
               "  (its loader does not take EXTNT as a boot driver); installing for data volumes.\n");
        O.Boot = 0;
    }
#endif
    if (UnderWow64()) {
        printf("  this is the 32-bit installer: on 64-bit Windows run amd64\\extinst.exe\n");
        return 1;
    }
    return Remove ? Uninstall() : Install(&O);
}
