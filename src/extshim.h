/* EXTNT - the FAT16 boot view of an ext volume (see extshim.c) */
#ifndef _EXTSHIM_H_
#define _EXTSHIM_H_

#define EXT_SHIM_OEM            "EXTNTBT1"      /* bytes 3-10 of sector 0 */
#define EXT_SHIM_WINDIR         0x3F0           /* the Windows directory, 16 bytes, in sector 1 */
#define EXT_SHIM_CODE_SIZE      1024

/* What the view is built through: the mounted, writable volume and the host's disk access */
typedef struct _EXT_SHIM_HOST {
    PEXT_VOL    Vol;
    PVOID       Ctx;
    /* sectors of the volume, bypassing any cache (file data) */
    EXT_STATUS  (*Read)(PVOID Ctx, ULONGLONG Sector, ULONG Count, PVOID Buf);
    EXT_STATUS  (*Write)(PVOID Ctx, ULONGLONG Sector, ULONG Count, const VOID *Buf);
    /* optional: a journal commit between files; may a file's blocks move; it moved */
    EXT_STATUS  (*Commit)(PVOID Ctx);
    BOOLEAN     (*CanMove)(PVOID Ctx, ULONG Ino);
    VOID        (*Moved)(PVOID Ctx, ULONG Ino);
    ULONG       CopyBlocks;             /* blocks copied at a time */
    /* for a new boot sector: the BPB's geometry */
    ULONG       PartitionStart, SectorsPerTrack, Heads;
} EXT_SHIM_HOST, *PEXT_SHIM_HOST;

typedef struct _EXT_SHIM_RESULT {
    const char *Error;                  /* why it failed (NULL: it did not) */
    UCHAR       ErrorName[12];          /* the file it is about, if any */
    ULONG       Files, Dirs;
    ULONG       MovedIn;                /* boot files moved into reach */
    ULONG       MovedOut;               /* other files moved out of the way */
    char        WinDir[16];
} EXT_SHIM_RESULT, *PEXT_SHIM_RESULT;

/*
 * Builds the boot view (Code: the 1024-byte boot loader, written with its
 * BPB) or brings it up to date (Code NULL: the boot sector stays). WinDir:
 * the Windows directory, or NULL: the one recorded in the boot sector, else
 * BOOT.INI's default entry.
 */
EXT_STATUS ExtShimUpdate(PEXT_SHIM_HOST Host, const char *WinDir, const UCHAR *Code, PEXT_SHIM_RESULT Result);

/* TRUE if sector 0 (512 bytes) is the boot sector of a view */
BOOLEAN ExtShimPresent(const UCHAR *Sector0);

#endif
