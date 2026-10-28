/*
 * EXTNT - extfmt: formats a volume (or an image file) as ext2, ext3 or ext4.
 *
 *   extfmt D: [options]
 *   extfmt \\.\PhysicalDrive1 /P:n [options]      partition n (1-4) of a disk
 *   extfmt IMAGE [options]                        an image file (a disk image:
 *                                                 its first Linux partition)
 *
 * The partition type becomes 83h (Linux). Runs on Windows NT 3.1 and later;
 * the same source builds for Linux, for images.
 */

#include "extusr.h"
#include "extsup.h"
#include "extimg.h"
#include "extmkfs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <winioctl.h>
#else
#include <sys/stat.h>
#include <strings.h>
#define _stricmp strcasecmp
#endif

static int
Usage(void)
{
    printf("Formats a volume as ext2, ext3 or ext4 (Linux).\n\n"
           "extfmt drive: [options]\n"
           "extfmt \\\\.\\PhysicalDriveN /P:n [options]\n"
           "extfmt image-file [options]\n\n"
           "  /T:2|3|4     ext2, ext3 (journal) or ext4 (default)\n"
           "  /V:label     volume label (up to 16 bytes)\n"
           "  /B:size      block size: 1024, 2048, 4096 ... 65536\n"
           "  /C:size      cluster size (ext4 bigalloc): 2 to 32768 blocks, up to 1G,\n"
           "               e.g. 64K or 1M; files take whole clusters\n"
           "  /I:size      inode size: 128, 256 (default), 512 ...\n"
           "  /N:bytes     bytes per inode (default by volume size)\n"
           "  /J:MB        journal size (ext3/ext4)\n"
           "  /R:percent   blocks reserved for root (default 5)\n"
           "  /O:list      features to add or (with ^) remove, comma separated:\n"
           "               has_journal ext_attr dir_index filetype extent 64bit\n"
           "               flex_bg sparse_super large_file huge_file dir_nlink\n"
           "               extra_isize metadata_csum uninit_bg\n"
           "  /REV0        the original ext2 format (no features)\n"
           "  /LAZY        ext4: leave inode tables unwritten (faster on big volumes)\n"
           "  /BOOT        reserve the boot area, to start Windows NT from the volume\n"
           "               (then copy the system and run extboot)\n"
           "  /P:n         partition n (1-4) of a disk or disk image\n"
           "  /Y           do not ask for confirmation\n"
           "  /Q           quiet\n");
    return 2;
}

static const struct { const char *Name; int Set; ULONG Bit; } Features[] = {
    { "has_journal", 0, EXT_FC_HAS_JOURNAL }, { "ext_attr", 0, EXT_FC_EXT_ATTR },
    { "dir_index", 0, EXT_FC_DIR_INDEX }, { "filetype", 1, EXT_FI_FILETYPE },
    { "extent", 1, EXT_FI_EXTENTS }, { "extents", 1, EXT_FI_EXTENTS },
    { "64bit", 1, EXT_FI_64BIT }, { "flex_bg", 1, EXT_FI_FLEX_BG },
    { "sparse_super", 2, EXT_RO_SPARSE_SUPER }, { "large_file", 2, EXT_RO_LARGE_FILE },
    { "huge_file", 2, EXT_RO_HUGE_FILE }, { "dir_nlink", 2, EXT_RO_DIR_NLINK },
    { "extra_isize", 2, EXT_RO_EXTRA_ISIZE }, { "metadata_csum", 2, EXT_RO_METADATA_CSUM },
    { "uninit_bg", 2, EXT_RO_GDT_CSUM }, { "gdt_csum", 2, EXT_RO_GDT_CSUM },
};

/* 4096, 64K, 1M, 1G */
static ULONG
SizeArg(const char *V)
{
    char *End;
    ULONGLONG N = strtoul(V, &End, 0);
    if (*End == 'k' || *End == 'K') N <<= 10;
    else if (*End == 'm' || *End == 'M') N <<= 20;
    else if (*End == 'g' || *End == 'G') N <<= 30;
    return N > 0xFFFFFFFFULL ? 0xFFFFFFFFUL : (ULONG)N;
}

static int
ParseFeatures(PEXT_MKFS P, const char *List)
{
    char Name[40];
    while (*List) {
        int Off = 0, n = 0;
        unsigned k;
        if (*List == '^' || *List == '-') { Off = 1; List++; }
        else if (*List == '+') List++;
        while (*List && *List != ',' && n < 39) Name[n++] = *List++;
        Name[n] = 0;
        if (*List == ',') List++;
        if (n == 0) continue;
        for (k = 0; k < sizeof(Features) / sizeof(Features[0]); k++) {
            if (!_stricmp(Name, Features[k].Name)) break;
        }
        if (k == sizeof(Features) / sizeof(Features[0])) {
            printf("Unknown feature: %s\n", Name);
            return -1;
        }
        if (Off) {
            P->FeaturesOff[Features[k].Set] |= Features[k].Bit;
            P->FeaturesOn[Features[k].Set] &= ~Features[k].Bit;
        } else {
            P->FeaturesOn[Features[k].Set] |= Features[k].Bit;
            P->FeaturesOff[Features[k].Set] &= ~Features[k].Bit;
        }
    }
    return 0;
}

/* The MBR slot of a disk image's first Linux (83h) partition, or the one asked for */
static int
FindPartition(PEXT_IMAGE Img, int Slot)
{
    UCHAR Mbr[512];
    int I;
    if (ImgRead(Img, 0, 1, Mbr) != 0 || Mbr[510] != 0x55 || Mbr[511] != 0xAA) return -1;
    /* a boot sector of a file system is not a partition table */
    if (!memcmp(Mbr + 3, "NTFS", 4) || !memcmp(Mbr + 0x36, "FAT", 3) || !memcmp(Mbr + 0x52, "FAT", 3)) return -1;
    for (I = 0; I < 4; I++) {
        UCHAR *E = Mbr + 446 + 16 * I;
        if ((Slot >= 0 && I != Slot) || (Slot < 0 && E[4] != 0x83)) continue;
        if (E[4] == 0 || E[4] == 0x05 || E[4] == 0x0F) return -1;
        Img->Offset = (ULONGLONG)(E[8] | (E[9] << 8) | ((ULONG)E[10] << 16) | ((ULONG)E[11] << 24)) * 512;
        Img->PartitionStart = (ULONG)(Img->Offset / 512);
        Img->Sectors = E[12] | (E[13] << 8) | ((ULONG)E[14] << 16) | ((ULONG)E[15] << 24);
        return I;
    }
    return -1;
}

static int
SetMbrType(PEXT_IMAGE Img, int Slot, UCHAR Type)
{
    UCHAR Mbr[512];
    ULONGLONG Off = Img->Offset, Sec = Img->Sectors;
    int R;
    Img->Offset = 0; Img->Sectors = 0;
    R = ImgRead(Img, 0, 1, Mbr);
    if (R == 0) {
        Mbr[446 + 16 * Slot + 4] = Type;
        R = ImgWrite(Img, 0, 1, Mbr);
    }
    Img->Offset = Off; Img->Sectors = Sec;
    return R;
}

int
main(int argc, char **argv)
{
    EXT_IMAGE Img;
    EXT_MKFS P;
    const char *Target = NULL;
    char Err[200];
    int Yes = 0, IsDrive, I, Locked = 0, Slot = -1, Found = -1;

    memset(&P, 0, sizeof(P));
    P.ReservedPercent = EXT_MKFS_DEFAULT;
    for (I = 1; I < argc; I++) {
        const char *A = argv[I];
        if ((A[0] == '/' || A[0] == '-') && A[1] && (A[2] == ':' || !Target || A[0] == '/')) {
            char C = (char)((A[1] >= 'a' && A[1] <= 'z') ? A[1] - 32 : A[1]);
            const char *V = (A[2] == ':') ? A + 3 : NULL;
            if (!_stricmp(A + 1, "Y")) Yes = 1;
            else if (!_stricmp(A + 1, "Q")) P.Quiet = 1;
            else if (!_stricmp(A + 1, "REV0")) P.Revision0 = 1;
            else if (!_stricmp(A + 1, "LAZY")) P.Lazy = 1;
            else if (!_stricmp(A + 1, "BOOT")) P.Boot = 1;
            else if (V && C == 'T') P.Type = atoi(V);
            else if (V && C == 'V') {
                if (strlen(V) > 16) { printf("The label is longer than 16 bytes.\n"); return 2; }
                strcpy(P.Label, V);
            }
            else if (V && C == 'B') P.BlockSize = SizeArg(V);
            else if (V && C == 'C') P.ClusterSize = SizeArg(V);
            else if (V && C == 'I') P.InodeSize = (ULONG)strtoul(V, NULL, 0);
            else if (V && C == 'N') P.InodeRatio = (ULONG)strtoul(V, NULL, 0);
            else if (V && C == 'J') P.JournalBlocks = (ULONG)strtoul(V, NULL, 0);   /* MB for now */
            else if (V && C == 'R') P.ReservedPercent = (ULONG)strtoul(V, NULL, 0);
            else if (V && C == 'O') { if (ParseFeatures(&P, V) != 0) return 2; }
            else if (V && C == 'P') Slot = atoi(V) - 1;
            else return Usage();
        } else if (Target == NULL) {
            Target = A;
        } else {
            return Usage();
        }
    }
    if (Target == NULL) return Usage();
    if (P.Type && (P.Type < 2 || P.Type > 4)) return Usage();

    IsDrive = (Target[0] && Target[1] == ':' && Target[2] == 0);
#ifdef _WIN32
    if (IsDrive) {
        if (ImgOpenDrive(&Img, Target[0], 0) != 0) {
            printf("Cannot open drive %s (error %lu).\n", Target, (unsigned long)GetLastError());
            return 1;
        }
        if (Img.Sectors == 0) {
            printf("Cannot tell the size of drive %s.\n", Target);
            return 1;
        }
    } else
#endif
    {
        if (IsDrive) { printf("Drives can be formatted on Windows only.\n"); return 1; }
        if (ImgOpen(&Img, Target, -1, 0) != 0) {
            printf("Cannot open %s.\n", Target);
            return 1;
        }
        Found = FindPartition(&Img, Slot);
        if (Slot >= 0 && Found < 0) {
            printf("%s has no partition %d.\n", Target, Slot + 1);
            return 1;
        }
        if (Img.Sectors == 0) {
#ifdef _WIN32
            DWORD High = 0, Low = GetFileSize(Img.Handle, &High);
            Img.Sectors = (((ULONGLONG)High << 32) | Low) / 512;
            if (Img.Sectors == 0) {
                /* a physical disk: its geometry */
                DISK_GEOMETRY Dg; DWORD N;
                if (DeviceIoControl(Img.Handle, IOCTL_DISK_GET_DRIVE_GEOMETRY, NULL, 0, &Dg, sizeof(Dg), &N, NULL))
                    Img.Sectors = (ULONGLONG)Dg.Cylinders.QuadPart * Dg.TracksPerCylinder * Dg.SectorsPerTrack;
            }
#else
            struct stat St;
            if (fstat(Img.Fd, &St) == 0) Img.Sectors = (ULONGLONG)St.st_size / 512;
#endif
        }
    }
    if (Img.Sectors < 128) {
        printf("The volume is too small.\n");
        return 1;
    }

    if (!Yes) {
        char Answer[8];
        printf("WARNING: ALL DATA ON %s%s WILL BE LOST!\nProceed with format (Y/N)? ",
               Target, Found >= 0 ? " (the partition)" : "");
        fflush(stdout);
#ifdef _WIN32
        {
            DWORD N;
            if (!ReadFile(GetStdHandle(STD_INPUT_HANDLE), Answer, sizeof(Answer), &N, NULL) || N == 0) Answer[0] = 'n';
        }
#else
        if (!fgets(Answer, sizeof(Answer), stdin)) Answer[0] = 'n';
#endif
        if (Answer[0] != 'y' && Answer[0] != 'Y') {
            printf("\nFormat cancelled.\n");
            return 1;
        }
    }

#ifdef _WIN32
    if (IsDrive) {
        if (ImgLock(&Img) != 0) {
            printf("Cannot lock drive %s: it is in use (open files, a running program,\n"
                   "the system or paging file). Close them and try again.\n", Target);
            ImgClose(&Img);
            return 1;
        }
        Locked = 1;
        /* Whatever was mounted lets go of the volume before it is rewritten */
        ImgDismount(&Img);
    }
#endif

    ExtMkfsDefaults(&P, Img.Sectors * 512);
    if (P.JournalBlocks) P.JournalBlocks = (ULONG)(((ULONGLONG)P.JournalBlocks << 20) / P.BlockSize);
    if (!P.Quiet) {
        printf("Formatting %llu MB as ext%d...\n", (unsigned long long)(Img.Sectors / 2048),
               P.Revision0 ? 2 : P.Type);
    }
    if (ExtMakeFs(&Img, &P, Err, sizeof(Err)) != 0) {
        printf("Format failed: %s\n", Err);
#ifdef _WIN32
        if (Locked) ImgUnlock(&Img);
#endif
        ImgClose(&Img);
        return 1;
    }

#ifdef _WIN32
    if (IsDrive) {
        if (ImgSetType(&Img, 0x83) != 0) {
            printf("Note: the partition type could not be set to 83h (error %lu).\n",
                   (unsigned long)GetLastError());
        }
        ImgDismount(&Img);
        ImgUnlock(&Img);
    } else
#endif
    if (Found >= 0) {
        (VOID)SetMbrType(&Img, Found, 0x83);
    }
    ImgClose(&Img);

    if (!P.Quiet) printf("Format complete.\n");
    (void)Locked;
    return 0;
}
