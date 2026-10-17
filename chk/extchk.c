/*
 * EXTNT - extchk: checks (and with /F repairs) an ext2/ext3/ext4 volume.
 *
 *   extchk D: [/F] [/V] [/Q]
 *   extchk \\.\PhysicalDrive1 /P:n [/F] [/V]
 *   extchk IMAGE [/P:n] [/F] [/V]
 *
 * Repairs need the volume to itself: extchk locks it (nothing may be open
 * on it) and dismounts it afterwards, so the next access mounts what was
 * repaired. Without /F the volume is only read; a mounted volume may then
 * show differences that are only changes not yet written.
 *
 * Exit code: 0 clean, 1 errors fixed, 4 errors remain, 8 the check failed.
 */

#include "extusr.h"
#include "extsup.h"
#include "extimg.h"
#include "extchkc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <strings.h>
#define _stricmp strcasecmp
#endif

static int
Usage(void)
{
    printf("Checks an ext2/ext3/ext4 volume.\n\n"
           "extchk drive: [/F] [/V] [/Q]\n"
           "extchk \\\\.\\PhysicalDriveN /P:n [/F] [/V] [/Q]\n"
           "extchk image-file [/P:n] [/F] [/V] [/Q]\n\n"
           "  /F    fix the errors found\n"
           "  /V    show each pass\n"
           "  /Q    show only the summary\n"
           "  /P:n  partition n (1-4) of a disk or disk image\n");
    return 8;
}

/* A disk image: its partition (the one asked for, or the first Linux one) */
static int
FindPartition(PEXT_IMAGE Img, int Slot)
{
    UCHAR Mbr[512];
    int I;
    if (ImgRead(Img, 0, 1, Mbr) != 0 || Mbr[510] != 0x55 || Mbr[511] != 0xAA) return -1;
    if (Slot < 0) {
        /* a bare file system (no partition table)? its superblock says so */
        UCHAR Sb[1024];
        if (ImgRead(Img, 2, 2, Sb) == 0 && ExtIsSuperblock(Sb)) return -1;
    }
    for (I = 0; I < 4; I++) {
        UCHAR *E = Mbr + 446 + 16 * I;
        if ((Slot >= 0 && I != Slot) || (Slot < 0 && E[4] != 0x83)) continue;
        if (E[4] == 0) return -1;
        Img->Offset = (ULONGLONG)(E[8] | (E[9] << 8) | ((ULONG)E[10] << 16) | ((ULONG)E[11] << 24)) * 512;
        Img->PartitionStart = (ULONG)(Img->Offset / 512);
        Img->Sectors = E[12] | (E[13] << 8) | ((ULONG)E[14] << 16) | ((ULONG)E[15] << 24);
        return I;
    }
    return -1;
}

int
main(int argc, char **argv)
{
    EXT_IMAGE Img;
    const char *Target = NULL;
    ULONG Flags = 0;
    int I, Slot = -1, IsDrive, Locked = 0, Rc;

    for (I = 1; I < argc; I++) {
        const char *A = argv[I];
        if (A[0] == '/' || (A[0] == '-' && A[1] && !A[2])) {
            if (!_stricmp(A + 1, "F")) Flags |= EXT_CHECK_FIX;
            else if (!_stricmp(A + 1, "V")) Flags |= EXT_CHECK_VERBOSE;
            else if (!_stricmp(A + 1, "Q")) Flags |= EXT_CHECK_QUIET;
            else if ((A[1] == 'P' || A[1] == 'p') && A[2] == ':') Slot = atoi(A + 3) - 1;
            else return Usage();
        } else if (Target == NULL) {
            Target = A;
        } else {
            return Usage();
        }
    }
    if (Target == NULL) return Usage();

    IsDrive = (Target[0] && Target[1] == ':' && Target[2] == 0);
#ifdef _WIN32
    if (IsDrive) {
        if (ImgOpenDrive(&Img, Target[0], !(Flags & EXT_CHECK_FIX)) != 0) {
            printf("Cannot open drive %s (error %lu).\n", Target, (unsigned long)GetLastError());
            return 8;
        }
        if (ImgLock(&Img) == 0) {
            Locked = 1;
        } else if (Flags & EXT_CHECK_FIX) {
            printf("Cannot lock drive %s: it is in use. Close the files open on it\n"
                   "(or the programs running from it) and try again.\n", Target);
            ImgClose(&Img);
            return 8;
        } else {
            printf("Drive %s is in use: changes not yet written may show as errors.\n", Target);
        }
        if (Locked) {
            /* the driver writes everything out before the lock is granted */
        }
    } else
#endif
    {
        if (IsDrive) { printf("Drives can be checked on Windows only.\n"); return 8; }
        if (ImgOpen(&Img, Target, -1, !(Flags & EXT_CHECK_FIX)) != 0) {
            printf("Cannot open %s.\n", Target);
            return 8;
        }
        if (FindPartition(&Img, Slot) < 0 && Slot >= 0) {
            printf("%s has no partition %d.\n", Target, Slot + 1);
            return 8;
        }
    }

    printf("Checking %s%s...\n", Target, (Flags & EXT_CHECK_FIX) ? " (repairing)" : "");
    Rc = ExtCheckVolume(&Img, Flags);

#ifdef _WIN32
    if (Locked) {
        if (Flags & EXT_CHECK_FIX) ImgDismount(&Img);
        ImgUnlock(&Img);
    }
#endif
    ImgClose(&Img);
    (void)Locked;
    return Rc;
}
