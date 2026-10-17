/*
 * EXTNT - extboot: makes an ext volume start Windows NT.
 *
 *   extboot D: [/S:dir] [/Q]
 *   extboot \\.\PhysicalDrive0 /P:n [/S:dir] [/Q]
 *   extboot IMAGE [/P:n] [/S:dir] [/Q]
 *
 * The volume must have been formatted with extfmt /BOOT, and hold NTLDR,
 * NTDETECT.COM and BOOT.INI in its root and the Windows directory (with
 * EXTNT installed to start at boot: extinst /BOOT). extboot writes the boot
 * loader and the FAT16 view NTLDR reads (see extshim.c), and sets the
 * partition type to 06h so that NTLDR takes the volume for its own; on a
 * disk image it also marks the partition active.
 *
 * Run it again after changing NTLDR, the kernel, the HAL or a boot driver
 * from a system that has no EXTNT, or after Linux changed them; EXTNT
 * itself keeps the view current.
 */

#include "extusr.h"
#include "extsup.h"
#include "extimg.h"
#include "extshim.h"
#include "extbootbin.h"
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
    printf("Makes an ext volume start Windows NT.\n\n"
           "extboot drive: [/S:dir] [/Q]\n"
           "extboot \\\\.\\PhysicalDriveN /P:n [/S:dir] [/Q]\n"
           "extboot image-file [/P:n] [/S:dir] [/Q]\n\n"
           "  /S:dir   the Windows directory (default: from BOOT.INI)\n"
           "  /P:n     partition n (1-4) of a disk or disk image\n"
           "  /FAT     set the partition type to 06h (FAT16) instead of 83h (Linux)\n"
           "  /Q       quiet\n\n"
           "The volume must be formatted with extfmt /BOOT and hold NTLDR, NTDETECT.COM,\n"
           "BOOT.INI and Windows NT with EXTNT installed by extinst /BOOT.\n");
    return 2;
}

/* The MBR slot of a disk image's ext partition (83h or 06h), or the one asked for */
static int
FindPartition(PEXT_IMAGE Img, int Slot)
{
    UCHAR Mbr[512];
    int I;
    if (ImgRead(Img, 0, 1, Mbr) != 0 || Mbr[510] != 0x55 || Mbr[511] != 0xAA) return -1;
    {
        UCHAR Sb[1024];
        if (Slot < 0 && ImgRead(Img, 2, 2, Sb) == 0 && ExtIsSuperblock(Sb)) return -1;
    }
    for (I = 0; I < 4; I++) {
        UCHAR *E = Mbr + 446 + 16 * I;
        if (Slot >= 0 && I != Slot) continue;
        if (E[4] == 0 || E[4] == 0x05 || E[4] == 0x0F) { if (Slot >= 0) return -1; continue; }
        Img->Offset = (ULONGLONG)(E[8] | (E[9] << 8) | ((ULONG)E[10] << 16) | ((ULONG)E[11] << 24)) * 512;
        Img->PartitionStart = (ULONG)(Img->Offset / 512);
        Img->Sectors = E[12] | (E[13] << 8) | ((ULONG)E[14] << 16) | ((ULONG)E[15] << 24);
        if (Slot < 0) {
            UCHAR Sb[1024];
            if (ImgRead(Img, 2, 2, Sb) != 0 || !ExtIsSuperblock(Sb)) { Img->Offset = 0; Img->PartitionStart = 0; Img->Sectors = 0; continue; }
        }
        return I;
    }
    return -1;
}

/* Partition type 06h, and the only active one */
static int
SetMbr(PEXT_IMAGE Img, int Slot, UCHAR Type)
{
    UCHAR Mbr[512];
    ULONGLONG Off = Img->Offset, Sec = Img->Sectors;
    int R, I;
    Img->Offset = 0; Img->Sectors = 0;
    R = ImgRead(Img, 0, 1, Mbr);
    if (R == 0) {
        for (I = 0; I < 4; I++) Mbr[446 + 16 * I] = (UCHAR)(I == Slot ? 0x80 : 0);
        Mbr[446 + 16 * Slot + 4] = Type;
        R = ImgWrite(Img, 0, 1, Mbr);
    }
    Img->Offset = Off; Img->Sectors = Sec;
    return R;
}

/* ------------------------------------------------------------------ */
/* The view through the image host                                    */
/* ------------------------------------------------------------------ */

static EXT_STATUS
HostRead(PVOID Ctx, ULONGLONG Sector, ULONG Count, PVOID Buf)
{
    return ImgRead((PEXT_IMAGE)Ctx, Sector, Count, Buf) == 0 ? EXT_OK : EXT_IO_ERROR;
}

static EXT_STATUS
HostWrite(PVOID Ctx, ULONGLONG Sector, ULONG Count, const VOID *Buf)
{
    return ImgWrite((PEXT_IMAGE)Ctx, Sector, Count, Buf) == 0 ? EXT_OK : EXT_IO_ERROR;
}

static int
BuildView(PEXT_IMAGE Img, const char *WinDir, int Quiet)
{
    EXT_VOL Vol;
    EXT_SHIM_HOST H;
    EXT_SHIM_RESULT R;
    EXT_STATUS St;

    memset(&Vol, 0, sizeof(Vol));
    Vol.HostContext = Img;
    St = ExtMount(&Vol, FALSE);
    if (!NT_SUCCESS(St)) {
        printf("extboot failed: this is not an ext volume EXTNT can write (%lX).\n", (unsigned long)St);
        return 1;
    }
    if (Vol.ClusterBits) {
        printf("extboot failed: volumes with bigalloc (clusters) cannot start Windows NT.\n");
        ExtUnmountFree(&Vol);
        return 1;
    }
    if (Vol.ReadOnly || Vol.NeedsRecovery || Vol.Super->LastOrphan) {
        printf("extboot failed: the volume %s: run extchk /F first.\n",
               Vol.ReadOnly ? "has features EXTNT cannot write" : "was not shut down cleanly");
        ExtUnmountFree(&Vol);
        return 1;
    }
    memset(&H, 0, sizeof(H));
    H.Vol = &Vol;
    H.Ctx = Img;
    H.Read = HostRead;
    H.Write = HostWrite;
    H.CopyBlocks = (256 * 1024) / Vol.BlockSize ? (256 * 1024) / Vol.BlockSize : 1;
    H.PartitionStart = Img->PartitionStart;
    H.SectorsPerTrack = Img->SectorsPerTrack;
    H.Heads = Img->Heads;
    St = ExtShimUpdate(&H, WinDir, ExtBootCode, &R);
    ExtFlushSuper(&Vol);
    ExtHostFlush(&Vol);
    ExtUnmountFree(&Vol);
    if (!NT_SUCCESS(St)) {
        printf("extboot failed: %s%s%.11s%s\n", R.Error ? R.Error : "error",
               R.ErrorName[0] ? " (" : "", (const char *)R.ErrorName, R.ErrorName[0] ? ")" : "");
        return 1;
    }
    if (!Quiet) {
        if (R.MovedOut) printf("  %lu files moved further out to make room\n", (unsigned long)R.MovedOut);
        printf("  boot view: %lu files in %lu directories (Windows in \\%s), %lu moved near the start\n",
               (unsigned long)R.Files, (unsigned long)R.Dirs, R.WinDir, (unsigned long)R.MovedIn);
    }
    return 0;
}

int
main(int argc, char **argv)
{
    EXT_IMAGE Img;
    const char *Target = NULL, *SysRoot = NULL;
    int I, Slot = -1, Found = -1, Quiet = 0, IsDrive, Rc;
    UCHAR Type = 0x83;   /* Linux: honest, and extnt gives the boot volume its letter */

    for (I = 1; I < argc; I++) {
        const char *A = argv[I];
        if (A[0] == '/' || (A[0] == '-' && A[1] && (!A[2] || A[2] == ':'))) {
            char C = (char)((A[1] >= 'a' && A[1] <= 'z') ? A[1] - 32 : A[1]);
            if (!_stricmp(A + 1, "Q")) Quiet = 1;
            else if (!_stricmp(A + 1, "FAT")) Type = 0x06;
            else if (C == 'S' && A[2] == ':') SysRoot = A + 3;
            else if (C == 'P' && A[2] == ':') Slot = atoi(A + 3) - 1;
            else return Usage();
        } else if (Target == NULL) {
            Target = A;
        } else {
            return Usage();
        }
    }
    if (Target == NULL) return Usage();
    memset(&Img, 0, sizeof(Img));

    IsDrive = (Target[0] && Target[1] == ':' && Target[2] == 0);
#ifdef _WIN32
    if (IsDrive) {
        if (ImgOpenDrive(&Img, Target[0], 0) != 0) {
            printf("Cannot open drive %s (error %lu).\n", Target, (unsigned long)GetLastError());
            return 1;
        }
        if (ImgLock(&Img) != 0) {
            printf("Cannot lock drive %s: it is in use (open files, a running program,\n"
                   "the system or paging file). Close them and try again.\n", Target);
            ImgClose(&Img);
            return 1;
        }
        ImgDismount(&Img);
    } else
#endif
    {
        if (IsDrive) { printf("Drives can be prepared on Windows only.\n"); return 1; }
        if (ImgOpen(&Img, Target, -1, 0) != 0) {
            printf("Cannot open %s.\n", Target);
            return 1;
        }
        Found = FindPartition(&Img, Slot);
        if (Slot >= 0 && Found < 0) {
            printf("%s has no partition %d.\n", Target, Slot + 1);
            ImgClose(&Img);
            return 1;
        }
    }

    if (!Quiet) printf("Preparing %s to start Windows NT...\n", Target);
    Rc = BuildView(&Img, SysRoot, Quiet);
    if (Rc == 0) {
#ifdef _WIN32
        if (IsDrive) {
            if (ImgSetType(&Img, Type) != 0)
                printf("Note: the partition type could not be set (error %lu).\n",
                       (unsigned long)GetLastError());
            if (!Quiet) printf("Mark the partition active (Disk Administrator) if it is to start the computer.\n");
        } else
#endif
        if (Found >= 0) {
            if (SetMbr(&Img, Found, Type) != 0) printf("Note: the partition table could not be updated.\n");
        }
        if (!Quiet) printf("Done. Linux mounts the volume with: mount -t ext4 (or ext2/ext3)\n");
    }
#ifdef _WIN32
    if (IsDrive) { ImgDismount(&Img); ImgUnlock(&Img); }
#endif
    ImgClose(&Img);
    return Rc ? 1 : 0;
}
