/*
 * EXTNT - exttool: work with ext2/3/4 images from Linux through the shared
 * engine (the very code the driver runs), so the engine can be validated with
 * e2fsck and debugfs before it ever loads in NT.
 *
 *   exttool IMG info
 *   exttool IMG ls [DIR] | lsr [DIR] | stat PATH
 *   exttool IMG mkdir DIR | rmdir DIR | rm FILE | mv OLD NEW
 *   exttool IMG put LOCAL PATH | get PATH LOCAL | cat PATH
 *   exttool IMG ln TARGETPATH LINKPATH        (hard link)
 *   exttool IMG symlink TARGETTEXT LINKPATH
 *   exttool IMG readlink PATH
 *   exttool IMG truncate PATH SIZE
 *   exttool IMG setea PATH ns NAME VALUE | getea PATH ns NAME | lsea PATH
 *   exttool IMG copyin LOCALDIR DIR           (recursive)
 *   exttool IMG stress SEED OPS [MAXNAME]
 *   exttool IMG recover                        (replay the journal, nothing else)
 *
 * Options before IMG:  -c  commit the command's changes, then stop as if the
 *                          power failed (journal written, homes not): the
 *                          next mount (Linux, e2fsck or exttool) replays it.
 *   exttool IMG frag PATH1 PATH2 NBLOCKS      (two interleaved files: 1 extent per block)
 *   exttool IMG pwrite PATH OFFSET LOCAL       (write LOCAL at OFFSET, holes allowed)
 *   exttool IMG cmp LOCALDIR [DIR]            (compare a local tree with the image)
 *   exttool IMG check
 *
 * IMG is an image file or IMG@N to pick MBR slot N (a type-0x83 partition is
 * found automatically in a partitioned image).
 */

#include "extusr.h"
#include "extsup.h"
#include "extdisk.h"
#include "extimg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>

static EXT_IMAGE Img;
static EXT_VOL   Vol;

static void
Fail(const char *What, EXT_STATUS S)
{
    fprintf(stderr, "exttool: %s: status %08lX\n", What, (unsigned long)(ULONG)S);
    exit(2);
}

static int
OpenImage(const char *Spec, int ReadOnly)
{
    char Path[1024];
    const char *At = strrchr(Spec, '@');
    int Part = -2;
    UCHAR Mbr[512];

    strncpy(Path, Spec, sizeof(Path) - 1);
    Path[sizeof(Path) - 1] = 0;
    if (At && At[1] >= '0' && At[1] <= '3' && At[2] == 0) {
        Path[At - Spec] = 0;
        Part = At[1] - '0';
    }
    if (Part == -2) {
        if (ImgOpen(&Img, Path, -1, ReadOnly) != 0) return -1;
        if (ImgRead(&Img, 0, 1, Mbr) == 0 && Mbr[510] == 0x55 && Mbr[511] == 0xAA) {
            int I;
            for (I = 0; I < 4; I++)
                if (Mbr[446 + 16 * I + 4] == 0x83) { Part = I; break; }
        }
        ImgClose(&Img);
        if (Part == -2) Part = -1;
    }
    return ImgOpen(&Img, Path, Part, ReadOnly);
}

static int CrashAfterCommit;      /* -c: simulate power loss right after the commit */

static void
Mount(int ReadOnly)
{
    EXT_STATUS S;
    memset(&Vol, 0, sizeof(Vol));
    Vol.HostContext = &Img;
    S = ExtMount(&Vol, (BOOLEAN)ReadOnly);
    if (!NT_SUCCESS(S)) Fail("mount", S);
    if (ReadOnly) return;
    if (Vol.NeedsRecovery) {
        S = ExtJournalRecover(&Vol);
        if (!NT_SUCCESS(S)) Fail("recover", S);
    }
    /* from here on behave like the driver: metadata stays in the cache until
       a commit logs it to the journal and writes it home */
    Img.Defer = 1;
    S = ExtStartWriting(&Vol);
    if (!NT_SUCCESS(S)) Fail("start writing", S);
    S = ExtProcessOrphans(&Vol);
    if (!NT_SUCCESS(S)) Fail("orphans", S);
}

/* one transaction */
static void
Commit(void)
{
    EXT_STATUS S;
    if (Vol.ReadOnly) return;
    S = ExtFlushSuper(&Vol);
    if (NT_SUCCESS(S)) S = ExtTxnCommit(&Vol);
    if (!NT_SUCCESS(S)) Fail("commit", S);
}

static void
Unmount(void)
{
    if (!Vol.ReadOnly) {
        EXT_STATUS S;
        if (CrashAfterCommit) {
            Vol.CrashAfterCommit = TRUE;
            Commit();
            ImgDiscard(&Img);       /* the power goes out: nothing else reaches the disk */
            ImgClose(&Img);
            return;
        }
        S = ExtStopWriting(&Vol);
        if (!NT_SUCCESS(S)) Fail("stop writing", S);
    }
    ExtUnmountFree(&Vol);
    ImgClose(&Img);
}

/* Resolve PATH to an inode number. If WantParent, return the parent inode and
   copy the last component into Last; *Child gets the child inode or 0. */
static ULONG
Resolve(const char *Path, char *Last, int WantParent, PEXT_INODE DirOut, ULONG *Child)
{
    char Buf[2048], *Tok, *Next, *P;
    ULONG Dir = EXT_ROOT_INO;
    EXT_INODE DirInode;
    EXT_ENTRY E;
    EXT_STATUS S;

    strncpy(Buf, Path, sizeof(Buf) - 1);
    Buf[sizeof(Buf) - 1] = 0;
    for (P = Buf; *P; P++) if (*P == '\\') *P = '/';
    if (Last) Last[0] = 0;
    if (Child) *Child = 0;

    S = ExtReadInode(&Vol, Dir, &DirInode);
    if (!NT_SUCCESS(S)) Fail("read root", S);

    P = Buf;
    while (*P == '/') P++;
    Tok = P;
    if (!*Tok) {        /* the root itself */
        if (DirOut) *DirOut = DirInode;
        if (Child) *Child = Dir;
        return Dir;
    }
    for (;;) {
        Next = strchr(Tok, '/');
        if (Next) { *Next++ = 0; while (*Next == '/') Next++; if (!*Next) Next = NULL; }
        if (!Next && WantParent) {
            if (Last) strcpy(Last, Tok);
            if (DirOut) *DirOut = DirInode;
            S = ExtLookup(&Vol, Dir, &DirInode, Tok, (ULONG)strlen(Tok), &E);
            if (Child && NT_SUCCESS(S)) *Child = E.Inode;
            return Dir;
        }
        S = ExtLookup(&Vol, Dir, &DirInode, Tok, (ULONG)strlen(Tok), &E);
        if (!NT_SUCCESS(S)) { fprintf(stderr, "exttool: %s: not found\n", Tok); exit(2); }
        Dir = E.Inode;
        S = ExtReadInode(&Vol, Dir, &DirInode);
        if (!NT_SUCCESS(S)) Fail("read inode", S);
        if (!Next) {
            if (Last) strcpy(Last, Tok);
            if (DirOut) *DirOut = DirInode;
            if (Child) *Child = Dir;
            return Dir;
        }
        Tok = Next;
    }
}

static const char *
TypeName(USHORT Mode)
{
    switch (Mode & EXT_S_IFMT) {
    case EXT_S_IFREG:  return "file";
    case EXT_S_IFDIR:  return "dir ";
    case EXT_S_IFLNK:  return "link";
    case EXT_S_IFCHR:  return "chr ";
    case EXT_S_IFBLK:  return "blk ";
    case EXT_S_IFIFO:  return "fifo";
    case EXT_S_IFSOCK: return "sock";
    default:           return "??? ";
    }
}

static void
CmdInfo(void)
{
    ULONGLONG FreeB; ULONG FreeI;
    printf("EXT volume \"%s\"\n", Vol.VolumeName);
    printf("  block size      %lu\n", (unsigned long)Vol.BlockSize);
    printf("  blocks          %llu (free %llu)\n",
           (unsigned long long)Vol.BlocksCount, (unsigned long long)Vol.FreeBlocksCount);
    printf("  inodes          %lu (free %lu)\n",
           (unsigned long)Vol.InodesCount, (unsigned long)Vol.FreeInodesCount);
    printf("  inode size      %u\n", Vol.InodeSize);
    printf("  groups          %lu  desc size %u\n", (unsigned long)Vol.GroupCount, Vol.DescSize);
    printf("  features  %s%s%s%s%s%s%s%s\n",
           Vol.HasExtents ? "extents " : "",
           Vol.Has64Bit ? "64bit " : "",
           Vol.HasMetaCsum ? "metadata_csum " : "",
           Vol.HasGdtCsum ? "gdt_csum " : "",
           Vol.HasFlexBg ? "flex_bg " : "",
           Vol.HasJournal ? "has_journal " : "",
           Vol.HasHtree ? "htree " : "",
           Vol.HasInlineData ? "inline_data " : "");
    if (NT_SUCCESS(ExtCountFree(&Vol, &FreeB, &FreeI)))
        printf("  recomputed free blocks %llu inodes %lu\n",
               (unsigned long long)FreeB, (unsigned long)FreeI);
}

static void
ListDir(ULONG Ino, PEXT_INODE Inode, const char *Prefix, int Recurse)
{
    EXT_ENTRY E;
    ULONGLONG Cookie = 0;
    EXT_STATUS S;

    for (;;) {
        EXT_INODE Child;
        S = ExtEnum(&Vol, Ino, Inode, Cookie, &E);
        if (S == EXT_NO_MORE_ENTRIES) break;
        if (!NT_SUCCESS(S)) Fail("enum", S);
        Cookie = E.Cookie;
        if (!NT_SUCCESS(ExtReadInode(&Vol, E.Inode, &Child))) continue;
        printf("  %s %8llu  %s%.*s\n", TypeName(Child.Mode),
               (unsigned long long)ExtInodeSize(&Vol, &Child),
               Prefix, (int)E.NameLength, E.Name);
        if (Recurse && (Child.Mode & EXT_S_IFMT) == EXT_S_IFDIR) {
            char Sub[1024];
            snprintf(Sub, sizeof(Sub), "%s%.*s/", Prefix, (int)E.NameLength, E.Name);
            ListDir(E.Inode, &Child, Sub, 1);
        }
    }
}

static void
CmdLs(const char *Path, int Recurse)
{
    EXT_INODE Inode;
    ULONG Ino = Resolve(Path ? Path : "/", NULL, 0, &Inode, NULL);
    ListDir(Ino, &Inode, "", Recurse);
}

static void
CmdPut(const char *Local, const char *Path)
{
    FILE *F = fopen(Local, "rb");
    char Last[300];
    EXT_INODE Dir, NewInode;
    ULONG DirIno, NewIno, Child = 0;
    EXT_STATUS S;
    static UCHAR Chunk[65536];
    ULONGLONG Off = 0;
    size_t n;

    if (!F) { perror(Local); exit(2); }
    DirIno = Resolve(Path, Last, 1, &Dir, &Child);
    if (Child) { ExtUnlink(&Vol, DirIno, &Dir, Last, (ULONG)strlen(Last)); ExtReadInode(&Vol, DirIno, &Dir); }
    S = ExtCreate(&Vol, DirIno, &Dir, Last, (ULONG)strlen(Last), EXT_S_IFREG | 0644, NULL, &NewIno, &NewInode);
    if (!NT_SUCCESS(S)) Fail("create", S);
    while ((n = fread(Chunk, 1, sizeof(Chunk), F)) > 0) {
        ULONG w;
        S = ExtWriteFile(&Vol, NewIno, &NewInode, Off, (ULONG)n, Chunk, &w);
        if (!NT_SUCCESS(S)) Fail("write", S);
        Off += w;
    }
    fclose(F);
    S = ExtWriteInode(&Vol, NewIno, &NewInode);
    if (!NT_SUCCESS(S)) Fail("write inode", S);
}

static void
CmdGet(const char *Path, const char *Local)
{
    EXT_INODE Inode;
    ULONG Ino = Resolve(Path, NULL, 0, &Inode, NULL);
    FILE *F = Local ? fopen(Local, "wb") : stdout;
    ULONGLONG Off = 0, Size = ExtInodeSize(&Vol, &Inode);
    static UCHAR Chunk[65536];

    if (!F) { perror(Local); exit(2); }
    while (Off < Size) {
        ULONG want = (ULONG)((Size - Off > sizeof(Chunk)) ? sizeof(Chunk) : (Size - Off));
        ULONG got;
        EXT_STATUS S = ExtReadFile(&Vol, Ino, &Inode, Off, want, Chunk, &got);
        if (!NT_SUCCESS(S)) Fail("read", S);
        if (got == 0) break;
        fwrite(Chunk, 1, got, F);
        Off += got;
    }
    if (Local) fclose(F);
}

static void
CmdMkdir(const char *Path)
{
    char Last[300];
    EXT_INODE Dir;
    ULONG DirIno = Resolve(Path, Last, 1, &Dir, NULL);
    EXT_STATUS S = ExtCreate(&Vol, DirIno, &Dir, Last, (ULONG)strlen(Last),
                             EXT_S_IFDIR | 0755, NULL, NULL, NULL);
    if (!NT_SUCCESS(S)) Fail("mkdir", S);
}

static void
CmdRm(const char *Path)
{
    char Last[300];
    EXT_INODE Dir;
    ULONG DirIno = Resolve(Path, Last, 1, &Dir, NULL);
    EXT_STATUS S = ExtUnlink(&Vol, DirIno, &Dir, Last, (ULONG)strlen(Last));
    if (!NT_SUCCESS(S)) Fail("rm", S);
}

static ULONG RmOps;

/* empties directory Ino (recursively); commits now and then */
static void
RmTree(ULONG Ino)
{
    EXT_INODE D;
    EXT_ENTRY E;
    EXT_STATUS S;
    for (;;) {
        ULONGLONG Cookie = 0;
        int Found = 0;
        ExtReadInode(&Vol, Ino, &D);
        while (NT_SUCCESS(ExtEnum(&Vol, Ino, &D, Cookie, &E))) {
            Cookie = E.Cookie;
            if ((E.NameLength == 1 && E.Name[0] == '.') ||
                (E.NameLength == 2 && E.Name[0] == '.' && E.Name[1] == '.')) continue;
            Found = 1;
            break;
        }
        if (!Found) return;
        {
            EXT_INODE C;
            if (NT_SUCCESS(ExtReadInode(&Vol, E.Inode, &C)) && (C.Mode & EXT_S_IFMT) == EXT_S_IFDIR)
                RmTree(E.Inode);
        }
        ExtReadInode(&Vol, Ino, &D);
        S = ExtUnlink(&Vol, Ino, &D, E.Name, E.NameLength);
        if (!NT_SUCCESS(S)) Fail("rmr", S);
        if ((++RmOps % 50) == 0) Commit();
    }
}

static void
CmdRmr(const char *Path)
{
    char Last[300];
    EXT_INODE Dir;
    ULONG Child = 0;
    ULONG DirIno = Resolve(Path, Last, 1, &Dir, &Child);
    EXT_STATUS S;
    if (Child == 0) Fail("rmr: not found", EXT_NOT_FOUND);
    RmTree(Child);
    ExtReadInode(&Vol, DirIno, &Dir);
    S = ExtUnlink(&Vol, DirIno, &Dir, Last, (ULONG)strlen(Last));
    if (!NT_SUCCESS(S)) Fail("rmr", S);
}

static void
CmdMv(const char *Old, const char *New)
{
    char OldLast[300], NewLast[300];
    EXT_INODE OldDir, NewDir;
    ULONG OldDirIno = Resolve(Old, OldLast, 1, &OldDir, NULL);
    ULONG NewDirIno = Resolve(New, NewLast, 1, &NewDir, NULL);
    EXT_STATUS S;
    if (OldDirIno == NewDirIno) ExtReadInode(&Vol, NewDirIno, &NewDir);
    S = ExtRename(&Vol, OldDirIno, &OldDir, OldLast, (ULONG)strlen(OldLast),
                  NewDirIno, &NewDir, NewLast, (ULONG)strlen(NewLast));
    if (!NT_SUCCESS(S)) Fail("mv", S);
}

static void
CmdLn(const char *Target, const char *Link)
{
    char Last[300];
    EXT_INODE Dir, TInode;
    ULONG DirIno, TIno;
    EXT_STATUS S;
    TIno = Resolve(Target, NULL, 0, &TInode, NULL);
    DirIno = Resolve(Link, Last, 1, &Dir, NULL);
    S = ExtLink(&Vol, DirIno, &Dir, Last, (ULONG)strlen(Last), TIno, &TInode);
    if (!NT_SUCCESS(S)) Fail("ln", S);
}

static void
CmdSymlink(const char *Text, const char *Link)
{
    char Last[300];
    EXT_INODE Dir;
    ULONG DirIno = Resolve(Link, Last, 1, &Dir, NULL);
    EXT_STATUS S = ExtCreate(&Vol, DirIno, &Dir, Last, (ULONG)strlen(Last),
                             EXT_S_IFLNK | 0777, Text, NULL, NULL);
    if (!NT_SUCCESS(S)) Fail("symlink", S);
}

static void
CmdReadlink(const char *Path)
{
    EXT_INODE Inode;
    ULONG Ino = Resolve(Path, NULL, 0, &Inode, NULL);
    char Buf[4096]; ULONG Len;
    EXT_STATUS S = ExtReadLink(&Vol, Ino, &Inode, Buf, sizeof(Buf), &Len);
    if (!NT_SUCCESS(S)) Fail("readlink", S);
    printf("%s\n", Buf);
}

static void
CmdTruncate(const char *Path, const char *SizeStr)
{
    EXT_INODE Inode;
    ULONG Ino = Resolve(Path, NULL, 0, &Inode, NULL);
    ULONGLONG Size = strtoull(SizeStr, NULL, 0);
    EXT_STATUS S = ExtSetFileSize(&Vol, Ino, &Inode, Size);
    if (!NT_SUCCESS(S)) Fail("truncate", S);
}

static void
EaReport(PVOID C, UCHAR Ns, const char *Name, ULONG NameLen, const UCHAR *Val, ULONG ValLen)
{
    (void)C;
    printf("  ns=%u %.*s = ", Ns, (int)NameLen, Name);
    if (Val) { ULONG i; for (i = 0; i < ValLen && i < 64; i++) putchar(Val[i] >= 32 && Val[i] < 127 ? Val[i] : '.'); }
    printf("  (%lu bytes)\n", (unsigned long)ValLen);
}

static void
CmdLsea(const char *Path)
{
    EXT_INODE Inode;
    ULONG Ino = Resolve(Path, NULL, 0, &Inode, NULL);
    EXT_STATUS S = ExtListXattr(&Vol, Ino, &Inode, EaReport, NULL);
    if (!NT_SUCCESS(S)) Fail("lsea", S);
}

static void
CmdSetea(const char *Path, const char *Ns, const char *Name, const char *Value)
{
    EXT_INODE Inode;
    ULONG Ino = Resolve(Path, NULL, 0, &Inode, NULL);
    UCHAR NsIdx = (UCHAR)(strcmp(Ns, "trusted") == 0 ? EXT_XATTR_INDEX_TRUSTED :
                          strcmp(Ns, "security") == 0 ? EXT_XATTR_INDEX_SECURITY :
                          strcmp(Ns, "system") == 0 ? EXT_XATTR_INDEX_SYSTEM :
                          EXT_XATTR_INDEX_USER);
    EXT_STATUS S = ExtSetXattr(&Vol, Ino, &Inode, NsIdx, Name, (ULONG)strlen(Name),
                               Value ? (const UCHAR *)Value : NULL,
                               Value ? (ULONG)strlen(Value) : 0);
    if (!NT_SUCCESS(S)) Fail("setea", S);
}

static void
CopyIn(const char *LocalDir, ULONG DirIno)
{
    DIR *D = opendir(LocalDir);
    struct dirent *De;
    EXT_INODE Dir;
    if (!D) { perror(LocalDir); exit(2); }
    ExtReadInode(&Vol, DirIno, &Dir);
    while ((De = readdir(D)) != NULL) {
        char Lp[2048];
        struct stat St;
        if (!strcmp(De->d_name, ".") || !strcmp(De->d_name, "..")) continue;
        snprintf(Lp, sizeof(Lp), "%s/%s", LocalDir, De->d_name);
        if (stat(Lp, &St) != 0) continue;
        ExtReadInode(&Vol, DirIno, &Dir);
        if (S_ISDIR(St.st_mode)) {
            ULONG SubIno; EXT_INODE Sub;
            EXT_STATUS S = ExtCreate(&Vol, DirIno, &Dir, De->d_name, (ULONG)strlen(De->d_name),
                                     EXT_S_IFDIR | 0755, NULL, &SubIno, &Sub);
            if (NT_SUCCESS(S)) CopyIn(Lp, SubIno);
        } else if (S_ISREG(St.st_mode)) {
            FILE *F = fopen(Lp, "rb");
            ULONG NewIno; EXT_INODE New; ULONGLONG Off = 0; size_t n;
            static UCHAR Chunk[65536];
            EXT_STATUS S;
            if (!F) continue;
            S = ExtCreate(&Vol, DirIno, &Dir, De->d_name, (ULONG)strlen(De->d_name),
                          EXT_S_IFREG | 0644, NULL, &NewIno, &New);
            if (NT_SUCCESS(S)) {
                while ((n = fread(Chunk, 1, sizeof(Chunk), F)) > 0) {
                    ULONG w; ExtWriteFile(&Vol, NewIno, &New, Off, (ULONG)n, Chunk, &w); Off += w;
                }
                ExtWriteInode(&Vol, NewIno, &New);
            }
            fclose(F);
        }
    }
    closedir(D);
}

/* A deterministic pseudo-random workload: create/grow/delete files and dirs */
static ULONG RndState;
static ULONG Rnd(void) { RndState = RndState * 1103515245 + 12345; return (RndState >> 16) & 0x7FFF; }

static void
CmdStress(ULONG Seed, ULONG Ops, ULONG MaxName)
{
    ULONG i;
    char Name[300];
    EXT_INODE Root;
    RndState = Seed;
    if (MaxName < 3) MaxName = 8;
    if (MaxName > 200) MaxName = 200;

    for (i = 0; i < Ops; i++) {
        ULONG op = Rnd() % 10;
        if (i % 7 == 0) Commit();       /* many small transactions */
        ULONG id = Rnd() % 64;
        ULONG DirIno = EXT_ROOT_INO;
        snprintf(Name, sizeof(Name), "f%03lu", (unsigned long)id);
        ExtReadInode(&Vol, DirIno, &Root);
        if (op < 5) {           /* create/overwrite a file with random content */
            EXT_ENTRY E;
            ULONG NewIno; EXT_INODE New;
            ULONG sz = Rnd() * 7 + 1;
            static UCHAR Buf[262144];
            ULONG j, w;
            if (sz > sizeof(Buf)) sz = sizeof(Buf);
            if (NT_SUCCESS(ExtLookup(&Vol, DirIno, &Root, Name, (ULONG)strlen(Name), &E))) {
                ExtUnlink(&Vol, DirIno, &Root, Name, (ULONG)strlen(Name));
                ExtReadInode(&Vol, DirIno, &Root);
            }
            if (NT_SUCCESS(ExtCreate(&Vol, DirIno, &Root, Name, (ULONG)strlen(Name),
                                     EXT_S_IFREG | 0644, NULL, &NewIno, &New))) {
                for (j = 0; j < sz; j++) Buf[j] = (UCHAR)(id + j);
                ExtWriteFile(&Vol, NewIno, &New, 0, sz, Buf, &w);
                ExtWriteInode(&Vol, NewIno, &New);
            }
        } else if (op < 7) {    /* delete a file */
            EXT_ENTRY E;
            if (NT_SUCCESS(ExtLookup(&Vol, DirIno, &Root, Name, (ULONG)strlen(Name), &E)))
                ExtUnlink(&Vol, DirIno, &Root, Name, (ULONG)strlen(Name));
        } else if (op < 9) {    /* mkdir */
            snprintf(Name, sizeof(Name), "d%03lu", (unsigned long)id);
            ExtCreate(&Vol, DirIno, &Root, Name, (ULONG)strlen(Name), EXT_S_IFDIR | 0755, NULL, NULL, NULL);
        } else {                /* rmdir */
            EXT_ENTRY E;
            snprintf(Name, sizeof(Name), "d%03lu", (unsigned long)id);
            if (NT_SUCCESS(ExtLookup(&Vol, DirIno, &Root, Name, (ULONG)strlen(Name), &E)))
                ExtUnlink(&Vol, DirIno, &Root, Name, (ULONG)strlen(Name));
        }
    }
    printf("stress: %lu ops done\n", (unsigned long)Ops);
}


/* Alternates whole-block writes between two files so neither gets two
   physically adjacent blocks: every block becomes its own extent */
static void
CmdFrag(const char *P1, const char *P2, ULONG Blocks)
{
    char L1[300], L2[300];
    EXT_INODE D1, D2, I1, I2;
    ULONG Dir1 = Resolve(P1, L1, 1, &D1, NULL), Ino1, Ino2, Dir2, b, w;
    UCHAR *Buf = (UCHAR *)malloc(Vol.BlockSize);
    EXT_STATUS S = ExtCreate(&Vol, Dir1, &D1, L1, (ULONG)strlen(L1), EXT_S_IFREG | 0644, NULL, &Ino1, &I1);
    if (!NT_SUCCESS(S)) Fail("create1", S);
    Dir2 = Resolve(P2, L2, 1, &D2, NULL);
    S = ExtCreate(&Vol, Dir2, &D2, L2, (ULONG)strlen(L2), EXT_S_IFREG | 0644, NULL, &Ino2, &I2);
    if (!NT_SUCCESS(S)) Fail("create2", S);
    for (b = 0; b < Blocks; b++) {
        memset(Buf, (int)(b & 0xFF), Vol.BlockSize);
        *(ULONG *)Buf = b;
        S = ExtWriteFile(&Vol, Ino1, &I1, (ULONGLONG)b * Vol.BlockSize, Vol.BlockSize, Buf, &w);
        if (!NT_SUCCESS(S)) Fail("write1", S);
        *(ULONG *)Buf = ~b;
        S = ExtWriteFile(&Vol, Ino2, &I2, (ULONGLONG)b * Vol.BlockSize, Vol.BlockSize, Buf, &w);
        if (!NT_SUCCESS(S)) Fail("write2", S);
    }
    ExtWriteInode(&Vol, Ino1, &I1);
    ExtWriteInode(&Vol, Ino2, &I2);
    free(Buf);
}

/* Writes a local file's contents at OFFSET inside PATH (creating PATH) */
static void
CmdPwrite(const char *Path, const char *OffStr, const char *Local)
{
    char Last[300];
    EXT_INODE Dir, Inode;
    ULONG DirIno, Ino = 0, w;
    ULONGLONG Off = strtoull(OffStr, NULL, 0);
    FILE *F = fopen(Local, "rb");
    static UCHAR Chunk[65536];
    size_t n;
    EXT_STATUS S;
    if (!F) { perror(Local); exit(2); }
    DirIno = Resolve(Path, Last, 1, &Dir, &Ino);
    if (Ino) ExtReadInode(&Vol, Ino, &Inode);
    else {
        S = ExtCreate(&Vol, DirIno, &Dir, Last, (ULONG)strlen(Last), EXT_S_IFREG | 0644, NULL, &Ino, &Inode);
        if (!NT_SUCCESS(S)) Fail("create", S);
    }
    while ((n = fread(Chunk, 1, sizeof(Chunk), F)) > 0) {
        S = ExtWriteFile(&Vol, Ino, &Inode, Off, (ULONG)n, Chunk, &w);
        if (!NT_SUCCESS(S)) Fail("write", S);
        Off += w;
    }
    fclose(F);
    S = ExtWriteInode(&Vol, Ino, &Inode);
    if (!NT_SUCCESS(S)) Fail("write inode", S);
}

/* Compares a local tree with a directory in the image: names, types, sizes,
   contents and symlink targets, in both directions. Returns mismatches. */
static ULONG
CmpTree(const char *LocalDir, ULONG DirIno, const char *Show)
{
    DIR *D = opendir(LocalDir);
    struct dirent *De;
    EXT_INODE Dir;
    ULONG Bad = 0, LocalCount = 0, ImgCount = 0;
    ULONGLONG Cookie = 0;
    EXT_ENTRY E;
    static UCHAR A[65536], B[65536];

    if (!D) { perror(LocalDir); return 1; }
    ExtReadInode(&Vol, DirIno, &Dir);
    while ((De = readdir(D)) != NULL) {
        char Lp[2048], Sp[2048];
        struct stat St;
        EXT_INODE Ch;
        if (!strcmp(De->d_name, ".") || !strcmp(De->d_name, "..")) continue;
        if (!strcmp(De->d_name, "lost+found")) continue;
        LocalCount++;
        snprintf(Lp, sizeof(Lp), "%s/%s", LocalDir, De->d_name);
        snprintf(Sp, sizeof(Sp), "%s/%s", Show, De->d_name);
        if (lstat(Lp, &St) != 0) continue;
        if (!NT_SUCCESS(ExtLookup(&Vol, DirIno, &Dir, De->d_name, (ULONG)strlen(De->d_name), &E))) {
            printf("missing: %s\n", Sp); Bad++; continue;
        }
        ExtReadInode(&Vol, E.Inode, &Ch);
        if (S_ISDIR(St.st_mode)) {
            if ((Ch.Mode & EXT_S_IFMT) != EXT_S_IFDIR) { printf("not a dir: %s\n", Sp); Bad++; continue; }
            Bad += CmpTree(Lp, E.Inode, Sp);
        } else if (S_ISLNK(St.st_mode)) {
            char T1[4096], T2[4096]; ULONG Len; ssize_t n = readlink(Lp, T1, sizeof(T1) - 1);
            T1[n < 0 ? 0 : n] = 0;
            if (!NT_SUCCESS(ExtReadLink(&Vol, E.Inode, &Ch, T2, sizeof(T2), &Len)) || strcmp(T1, T2)) {
                printf("symlink differs: %s\n", Sp); Bad++;
            }
        } else if (S_ISREG(St.st_mode)) {
            FILE *F = fopen(Lp, "rb");
            ULONGLONG Off = 0;
            if (ExtInodeSize(&Vol, &Ch) != (ULONGLONG)St.st_size) { printf("size differs: %s\n", Sp); Bad++; }
            else if (F) {
                for (;;) {
                    size_t n = fread(A, 1, sizeof(A), F);
                    ULONG got;
                    if (n == 0) break;
                    if (!NT_SUCCESS(ExtReadFile(&Vol, E.Inode, &Ch, Off, (ULONG)n, B, &got)) || got != n ||
                        memcmp(A, B, n)) { printf("content differs: %s @%llu\n", Sp, (unsigned long long)Off); Bad++; break; }
                    Off += n;
                }
            }
            if (F) fclose(F);
        }
    }
    closedir(D);
    for (;;) {
        EXT_STATUS S = ExtEnum(&Vol, DirIno, &Dir, Cookie, &E);
        if (S == EXT_NO_MORE_ENTRIES) break;
        if (!NT_SUCCESS(S)) { printf("enum failed in %s: %08lX\n", Show, (unsigned long)(ULONG)S); Bad++; break; }
        Cookie = E.Cookie;
        if (E.NameLength == 10 && !memcmp(E.Name, "lost+found", 10)) continue;
        ImgCount++;
    }
    if (ImgCount != LocalCount) { printf("entry count differs in %s: local %lu image %lu\n", Show,
                                         (unsigned long)LocalCount, (unsigned long)ImgCount); Bad++; }
    return Bad;
}

static void
CmdCheck(void)
{
    ULONGLONG FreeB; ULONG FreeI;
    EXT_STATUS S = ExtCountFree(&Vol, &FreeB, &FreeI);
    if (!NT_SUCCESS(S)) Fail("count", S);
    printf("sb free blocks %llu inodes %lu / recomputed %llu %lu : %s\n",
           (unsigned long long)Vol.FreeBlocksCount, (unsigned long)Vol.FreeInodesCount,
           (unsigned long long)FreeB, (unsigned long)FreeI,
           (FreeB == Vol.FreeBlocksCount && FreeI == Vol.FreeInodesCount) ? "OK" : "MISMATCH");
}

int
main(int argc, char **argv)
{
    const char *Spec, *Cmd;
    int ro;

    while (argc > 1 && argv[1][0] == '-' && argv[1][1]) {
        if (!strcmp(argv[1], "-c")) CrashAfterCommit = 1;
        else { fprintf(stderr, "exttool: unknown option %s\n", argv[1]); return 1; }
        argv++; argc--;
    }
    if (argc < 3) {
        fprintf(stderr, "usage: exttool [-c] IMG COMMAND [args]\n");
        return 1;
    }
    Spec = argv[1];
    Cmd = argv[2];
    ro = (!strcmp(Cmd, "info") || !strcmp(Cmd, "ls") || !strcmp(Cmd, "lsr") ||
          !strcmp(Cmd, "stat") || !strcmp(Cmd, "cat") || !strcmp(Cmd, "get") ||
          !strcmp(Cmd, "readlink") || !strcmp(Cmd, "lsea") || !strcmp(Cmd, "getea") ||
          !strcmp(Cmd, "check") || !strcmp(Cmd, "cmp") || !strcmp(Cmd, "dxhash"));

    if (OpenImage(Spec, ro) != 0) { fprintf(stderr, "exttool: cannot open %s\n", Spec); return 2; }
    Mount(ro);

    if (!strcmp(Cmd, "info")) CmdInfo();
    else if (!strcmp(Cmd, "ls")) CmdLs(argc > 3 ? argv[3] : "/", 0);
    else if (!strcmp(Cmd, "lsr")) CmdLs(argc > 3 ? argv[3] : "/", 1);
    else if (!strcmp(Cmd, "put") && argc >= 5) CmdPut(argv[3], argv[4]);
    else if (!strcmp(Cmd, "get") && argc >= 4) CmdGet(argv[3], argc > 4 ? argv[4] : NULL);
    else if (!strcmp(Cmd, "cat") && argc >= 4) CmdGet(argv[3], NULL);
    else if (!strcmp(Cmd, "mkdir") && argc >= 4) CmdMkdir(argv[3]);
    else if ((!strcmp(Cmd, "rmdir") || !strcmp(Cmd, "rm")) && argc >= 4) CmdRm(argv[3]);
    else if (!strcmp(Cmd, "rmr") && argc >= 4) CmdRmr(argv[3]);
    else if (!strcmp(Cmd, "mv") && argc >= 5) CmdMv(argv[3], argv[4]);
    else if (!strcmp(Cmd, "ln") && argc >= 5) CmdLn(argv[3], argv[4]);
    else if (!strcmp(Cmd, "symlink") && argc >= 5) CmdSymlink(argv[3], argv[4]);
    else if (!strcmp(Cmd, "readlink") && argc >= 4) CmdReadlink(argv[3]);
    else if (!strcmp(Cmd, "truncate") && argc >= 5) CmdTruncate(argv[3], argv[4]);
    else if (!strcmp(Cmd, "lsea") && argc >= 4) CmdLsea(argv[3]);
    else if (!strcmp(Cmd, "setea") && argc >= 6) CmdSetea(argv[3], argv[4], argv[5], argc > 6 ? argv[6] : NULL);
    else if (!strcmp(Cmd, "copyin") && argc >= 5) {
        char Last[300]; EXT_INODE Dir; ULONG DirIno;
        if (strcmp(argv[4], "/")) CmdMkdir(argv[4]);
        DirIno = Resolve(argv[4], Last, 0, &Dir, NULL);
        CopyIn(argv[3], DirIno);
    }
    else if (!strcmp(Cmd, "stress") && argc >= 5)
        CmdStress((ULONG)strtoul(argv[3], NULL, 0), (ULONG)strtoul(argv[4], NULL, 0),
                  argc > 5 ? (ULONG)strtoul(argv[5], NULL, 0) : 8);
    else if (!strcmp(Cmd, "recover")) { /* done at mount */ printf("recovered\n"); }
    else if (!strcmp(Cmd, "frag") && argc >= 6) CmdFrag(argv[3], argv[4], (ULONG)strtoul(argv[5], NULL, 0));
    else if (!strcmp(Cmd, "pwrite") && argc >= 6) CmdPwrite(argv[3], argv[4], argv[5]);
    else if (!strcmp(Cmd, "omkdir") && argc >= 4) {
        char Last[300]; EXT_INODE Dir; ULONG P = Resolve(argv[3], Last, 1, &Dir, NULL);
        EXT_STATUS S = ExtOldCreate(&Vol, P, Last, (ULONG)strlen(Last), (USHORT)(0x4000|0755), NULL);
        printf("omkdir %s: %s (%lX)\n", argv[3], NT_SUCCESS(S)?"OK":"FAIL", (unsigned long)S);
    }
    else if (!strcmp(Cmd, "oput") && argc >= 5) {
        char Last[300]; EXT_INODE Dir; ULONG P = Resolve(argv[4], Last, 1, &Dir, NULL), nino=0;
        FILE *lf = fopen(argv[3], "rb"); long n; unsigned char *b; EXT_STATUS S;
        if (!lf) { fprintf(stderr, "cannot open %s\n", argv[3]); return 1; }
        fseek(lf, 0, SEEK_END); n = ftell(lf); fseek(lf, 0, SEEK_SET);
        b = malloc(n > 0 ? n : 1); fread(b, 1, n, lf); fclose(lf);
        S = ExtOldCreate(&Vol, P, Last, (ULONG)strlen(Last), (USHORT)(0x8000|0644), &nino);
        if (NT_SUCCESS(S) && n > 0) S = ExtOldWriteData(&Vol, nino, 0, b, (ULONG)n);
        free(b);
        printf("oput %s -> %s (ino %lu): %s (%lX)\n", argv[3], argv[4], (unsigned long)nino, NT_SUCCESS(S)?"OK":"FAIL", (unsigned long)S);
    }
    else if (!strcmp(Cmd, "owrite") && argc >= 5) {
        char Last[300]; EXT_INODE Dir; ULONG P = Resolve(argv[3], Last, 1, &Dir, NULL), nino=0;
        EXT_STATUS S = ExtOldCreate(&Vol, P, Last, (ULONG)strlen(Last), (USHORT)(0x8000|0644), &nino);
        if (NT_SUCCESS(S)) S = ExtOldWriteData(&Vol, nino, 0, (const unsigned char*)argv[4], (ULONG)strlen(argv[4]));
        printf("owrite %s (ino %lu): %s (%lX)\n", argv[3], (unsigned long)nino, NT_SUCCESS(S)?"OK":"FAIL", (unsigned long)S);
    }
    else if (!strcmp(Cmd, "orm") && argc >= 4) {
        char Last[300]; EXT_INODE Dir; ULONG P = Resolve(argv[3], Last, 1, &Dir, NULL);
        EXT_STATUS S = ExtOldUnlink(&Vol, P, Last, (ULONG)strlen(Last));
        printf("orm %s: %s (%lX)\n", argv[3], NT_SUCCESS(S)?"OK":"FAIL", (unsigned long)S);
    }
    else if (!strcmp(Cmd, "check")) CmdCheck();
    else if (!strcmp(Cmd, "dxhash") && argc >= 5) {       /* dxhash VERSION NAME... (volume seed) */
        int k;
        for (k = 4; k < argc; k++) {
            ULONG Minor, H = ExtNameHash(&Vol, (UCHAR)atoi(argv[3]), argv[k], (ULONG)strlen(argv[k]), &Minor);
            printf("%s %08lx %08lx\n", argv[k], (unsigned long)H, (unsigned long)Minor);
        }
    }
    else if (!strcmp(Cmd, "cmp") && argc >= 4) {
        EXT_INODE Dir; ULONG DirIno = Resolve(argc > 4 ? argv[4] : "/", NULL, 0, &Dir, NULL);
        ULONG Bad = CmpTree(argv[3], DirIno, "");
        printf("cmp: %lu difference(s)\n", (unsigned long)Bad);
        Unmount();
        return Bad ? 1 : 0;
    }
    else { fprintf(stderr, "exttool: unknown or malformed command '%s'\n", Cmd); Unmount(); return 1; }

    Unmount();
    return 0;
}
