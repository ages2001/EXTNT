/*
 * EXTNT - user-mode host for the engine: an image file or (on Windows) a
 * volume opened with CreateFile. Mapped sector runs are read into heap
 * buffers; a run mapped twice at the same time shares one buffer, and a
 * dirty buffer is written back when its last mapping goes away. The cache is
 * sector-exact and rejects overlapping maps, exactly like the kernel metadata
 * cache the driver uses.
 */

#include "extusr.h"
#include "extsup.h"
#include "extimg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifndef _WIN32
#include <unistd.h>
#include <fcntl.h>
#endif

typedef struct _MAPPING {
    struct _MAPPING *Next;
    ULONGLONG   Sector;
    ULONG       Count;
    ULONG       Refs;
    BOOLEAN     Dirty;
    BOOLEAN     IsData;         /* file data: written through, never deferred */
    PUCHAR      Data;
} MAPPING;

static void
Die(const char *Msg, ULONGLONG A, ULONGLONG B)
{
    fprintf(stderr, "extimg: %s (%llu, %llu)\n", Msg, (unsigned long long)A, (unsigned long long)B);
#ifdef _WIN32
    ExitProcess(3);
#else
    abort();
#endif
}

int
ImgRead(PEXT_IMAGE Img, ULONGLONG Sector, ULONG Count, void *Buf)
{
    ULONGLONG Off = Img->Offset + Sector * 512;
    if (Img->Sectors && Sector + Count > Img->Sectors) return -1;
#ifdef _WIN32
    {
        LONG High = (LONG)(Off >> 32); DWORD Got;
        if (SetFilePointer(Img->Handle, (LONG)(ULONG)Off, &High, FILE_BEGIN) == 0xFFFFFFFF &&
            GetLastError() != NO_ERROR) return -1;
        if (!ReadFile(Img->Handle, Buf, Count * 512, &Got, NULL) || Got != Count * 512) return -1;
    }
#else
    if (pread(Img->Fd, Buf, (size_t)Count * 512, (off_t)Off) != (ssize_t)Count * 512) return -1;
#endif
    Img->Reads += Count;
    return 0;
}

int
ImgWrite(PEXT_IMAGE Img, ULONGLONG Sector, ULONG Count, const void *Buf)
{
    ULONGLONG Off = Img->Offset + Sector * 512;
    if (Img->ReadOnly) Die("write to a read-only image", Sector, Count);
    if (Img->Sectors && Sector + Count > Img->Sectors) return -1;
#ifdef _WIN32
    {
        LONG High = (LONG)(Off >> 32); DWORD Put;
        if (SetFilePointer(Img->Handle, (LONG)(ULONG)Off, &High, FILE_BEGIN) == 0xFFFFFFFF &&
            GetLastError() != NO_ERROR) return -1;
        if (!WriteFile(Img->Handle, Buf, Count * 512, &Put, NULL) || Put != Count * 512) return -1;
    }
#else
    if (pwrite(Img->Fd, Buf, (size_t)Count * 512, (off_t)Off) != (ssize_t)Count * 512) return -1;
#endif
    Img->Writes += Count;
    return 0;
}

static void
Drop(PEXT_IMAGE Img, MAPPING *M)
{
    MAPPING **P;
    if (M->Dirty && ImgWrite(Img, M->Sector, M->Count, M->Data) != 0) Die("write failed", M->Sector, M->Count);
    for (P = (MAPPING **)&Img->Maps; *P; P = &(*P)->Next)
        if (*P == M) { *P = M->Next; break; }
    Img->Mapped--;
    free(M->Data);
    free(M);
}

EXT_STATUS
ExtHostMap(PEXT_VOL Vol, ULONGLONG Sector, ULONG Count, ULONG Mode, PEXT_BUF Buf)
{
    PEXT_IMAGE Img = (PEXT_IMAGE)Vol->HostContext;
    MAPPING *M, *Next;

    if (Count == 0 || Count > 256) Die("bad map size", Sector, Count);
    for (M = (MAPPING *)Img->Maps; M; M = Next) {
        Next = M->Next;
        if (M->Sector == Sector && M->Count == Count) {
            M->Refs++;
            Buf->Data = M->Data; Buf->Sector = Sector; Buf->Count = Count;
            Buf->Host = M; Buf->Dirty = FALSE;
            if (EXT_MAP_MODE(Mode) == EXT_MAP_NEW) memset(M->Data, 0, Count * 512);
            return EXT_OK;
        }
        if (Sector < M->Sector + M->Count && M->Sector < Sector + Count) {
            /* a different run over the same sectors: write back a kept
               (unreferenced) one first; one still in use is an engine bug */
            if (M->Refs) Die("overlapping maps", Sector, M->Sector);
            Drop(Img, M);
        }
    }
    M = (MAPPING *)calloc(1, sizeof(MAPPING));
    M->Data = (PUCHAR)malloc(Count * 512);
    M->Sector = Sector; M->Count = Count; M->Refs = 1;
    M->IsData = (BOOLEAN)((Mode & EXT_MAP_DATA) != 0);
    if (EXT_MAP_MODE(Mode) == EXT_MAP_NEW) {
        memset(M->Data, 0, Count * 512);
    } else if (ImgRead(Img, Sector, Count, M->Data) != 0) {
        free(M->Data); free(M);
        return EXT_IO_ERROR;
    }
    M->Next = (MAPPING *)Img->Maps;
    Img->Maps = M;
    Img->Mapped++;
    Buf->Data = M->Data; Buf->Sector = Sector; Buf->Count = Count;
    Buf->Host = M; Buf->Dirty = FALSE;
    return EXT_OK;
}

VOID
ExtHostSetDirty(PEXT_VOL Vol, PEXT_BUF Buf)
{
    (void)Vol;
    ((MAPPING *)Buf->Host)->Dirty = TRUE;
    Buf->Dirty = TRUE;
}

VOID
ExtHostUnmap(PEXT_VOL Vol, PEXT_BUF Buf)
{
    PEXT_IMAGE Img = (PEXT_IMAGE)Vol->HostContext;
    MAPPING *M = (MAPPING *)Buf->Host;

    if (M == NULL) return;
    Buf->Host = NULL;
    if (--M->Refs) return;
    /* deferred mode keeps dirty metadata until the next commit/flush */
    if (Img->Defer && M->Dirty && !M->IsData) return;
    Drop(Img, M);
}

VOID
ExtHostEnumDirty(PEXT_VOL Vol, EXT_DIRTY_CB Cb, PVOID Ctx)
{
    PEXT_IMAGE Img = (PEXT_IMAGE)Vol->HostContext;
    MAPPING *M;
    for (M = (MAPPING *)Img->Maps; M; M = M->Next)
        if (M->Dirty && !M->IsData) Cb(Ctx, M->Sector, M->Count, M->Data);
}

EXT_STATUS
ExtHostFlush(PEXT_VOL Vol)
{
    PEXT_IMAGE Img = (PEXT_IMAGE)Vol->HostContext;
    MAPPING *M, *Next;
    for (M = (MAPPING *)Img->Maps; M; M = Next) {
        Next = M->Next;
        if (M->Dirty) {
            if (ImgWrite(Img, M->Sector, M->Count, M->Data) != 0) return EXT_IO_ERROR;
            M->Dirty = FALSE;
        }
        if (M->Refs == 0) Drop(Img, M);
    }
    Img->Flushes++;
    return EXT_OK;
}

EXT_STATUS
ExtHostReadDirect(PEXT_VOL Vol, ULONGLONG Sector, ULONG Count, PVOID Buf)
{
    return ImgRead((PEXT_IMAGE)Vol->HostContext, Sector, Count, Buf) == 0 ? EXT_OK : EXT_IO_ERROR;
}

EXT_STATUS
ExtHostWriteDirect(PEXT_VOL Vol, ULONGLONG Sector, ULONG Count, const VOID *Buf)
{
    return ImgWrite((PEXT_IMAGE)Vol->HostContext, Sector, Count, Buf) == 0 ? EXT_OK : EXT_IO_ERROR;
}

/* Throws away deferred dirty metadata without writing it (power-loss test) */
void
ImgDiscard(PEXT_IMAGE Img)
{
    MAPPING *M, *Next;
    for (M = (MAPPING *)Img->Maps; M; M = Next) {
        Next = M->Next;
        M->Dirty = FALSE;
        if (M->Refs == 0) Drop(Img, M);
    }
}

PVOID ExtHostAlloc(ULONG Bytes) { return malloc(Bytes ? Bytes : 1); }
VOID  ExtHostFree(PVOID P) { free(P); }
VOID  ExtHostFreed(PEXT_VOL Vol, ULONGLONG Sector, ULONG Count) { (void)Vol; (void)Sector; (void)Count; }

ULONG
ExtHostNow(VOID)
{
#ifdef _WIN32
    SYSTEMTIME St;
    FILETIME Ft;
    ULARGE_INTEGER U;
    GetSystemTime(&St);
    SystemTimeToFileTime(&St, &Ft);
    U.LowPart = Ft.dwLowDateTime;
    U.HighPart = Ft.dwHighDateTime;
    return (ULONG)((U.QuadPart - 116444736000000000ULL) / 10000000ULL);
#else
    return (ULONG)time(NULL);        /* ext stores seconds since 1970 UTC */
#endif
}

int
ImgOpen(PEXT_IMAGE Img, const char *Path, int Partition, int ReadOnly)
{
    UCHAR Mbr[512];
    memset(Img, 0, sizeof(*Img));
    Img->ReadOnly = ReadOnly;
#ifdef _WIN32
    Img->Handle = CreateFileA(Path, GENERIC_READ | (ReadOnly ? 0 : GENERIC_WRITE),
                              FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (Img->Handle == INVALID_HANDLE_VALUE) return -1;
#else
    Img->Fd = open(Path, ReadOnly ? O_RDONLY : O_RDWR);
    if (Img->Fd < 0) return -1;
#endif
    if (Partition >= 0) {
        if (ImgRead(Img, 0, 1, Mbr) != 0) return -1;
        if (Mbr[510] != 0x55 || Mbr[511] != 0xAA) return -1;
        {
            UCHAR *E = Mbr + 446 + 16 * Partition;
            ULONG Start = E[8] | (E[9] << 8) | ((ULONG)E[10] << 16) | ((ULONG)E[11] << 24);
            ULONG Len = E[12] | (E[13] << 8) | ((ULONG)E[14] << 16) | ((ULONG)E[15] << 24);
            if (Len == 0) return -1;
            Img->Offset = (ULONGLONG)Start * 512;
            Img->Sectors = Len;
            Img->PartitionStart = Start;
        }
    }
    return 0;
}

#ifdef _WIN32
#include <winioctl.h>

int
ImgOpenDrive(PEXT_IMAGE Img, char Letter, int ReadOnly)
{
    char Path[8];
    PARTITION_INFORMATION Pi;
    DISK_GEOMETRY Dg;
    DWORD N;

    memset(Img, 0, sizeof(*Img));
    Img->ReadOnly = ReadOnly;
    Path[0] = '\\'; Path[1] = '\\'; Path[2] = '.'; Path[3] = '\\';
    Path[4] = Letter; Path[5] = ':'; Path[6] = 0;
    Img->Handle = CreateFileA(Path, GENERIC_READ | (ReadOnly ? 0 : GENERIC_WRITE),
                              FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (Img->Handle == INVALID_HANDLE_VALUE) return -1;
    if (DeviceIoControl(Img->Handle, IOCTL_DISK_GET_PARTITION_INFO, NULL, 0, &Pi, sizeof(Pi), &N, NULL)) {
        Img->Sectors = (ULONGLONG)(Pi.PartitionLength.QuadPart / 512);
        Img->PartitionStart = (ULONG)(Pi.StartingOffset.QuadPart / 512);
    }
    if (DeviceIoControl(Img->Handle, IOCTL_DISK_GET_DRIVE_GEOMETRY, NULL, 0, &Dg, sizeof(Dg), &N, NULL)) {
        if (Dg.BytesPerSector != 512) { CloseHandle(Img->Handle); return -2; }
        Img->SectorsPerTrack = Dg.SectorsPerTrack;
        Img->Heads = Dg.TracksPerCylinder;
        if (Img->Sectors == 0)
            Img->Sectors = (ULONGLONG)Dg.Cylinders.QuadPart * Dg.TracksPerCylinder * Dg.SectorsPerTrack;
    }
    return 0;
}

int ImgLock(PEXT_IMAGE Img)
{ DWORD N; return DeviceIoControl(Img->Handle, FSCTL_LOCK_VOLUME, NULL, 0, NULL, 0, &N, NULL) ? 0 : -1; }

void ImgDismount(PEXT_IMAGE Img)
{ DWORD N; DeviceIoControl(Img->Handle, FSCTL_DISMOUNT_VOLUME, NULL, 0, NULL, 0, &N, NULL); }

void ImgUnlock(PEXT_IMAGE Img)
{ DWORD N; DeviceIoControl(Img->Handle, FSCTL_UNLOCK_VOLUME, NULL, 0, NULL, 0, &N, NULL); }

/* Partition type 83h (Linux) */
int ImgSetType(PEXT_IMAGE Img, UCHAR Type)
{
    SET_PARTITION_INFORMATION Sp;
    DWORD N;
    Sp.PartitionType = Type;
    return DeviceIoControl(Img->Handle, IOCTL_DISK_SET_PARTITION_INFO, &Sp, sizeof(Sp), NULL, 0, &N, NULL) ? 0 : -1;
}
#endif

void
ImgClose(PEXT_IMAGE Img)
{
    MAPPING *M, *Next;
    for (M = (MAPPING *)Img->Maps; M; M = Next) {      /* kept clean/dirty runs */
        Next = M->Next;
        if (M->Refs == 0) Drop(Img, M);
    }
    if (Img->Mapped) Die("maps still outstanding at close", Img->Mapped, 0);
#ifdef _WIN32
    CloseHandle(Img->Handle);
#else
    close(Img->Fd);
#endif
}
