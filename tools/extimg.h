/* EXTNT - user-mode image host (see extimg.c) */
#ifndef _EXTIMG_H_
#define _EXTIMG_H_

typedef struct _EXT_IMAGE {
#ifdef _WIN32
    HANDLE      Handle;
#else
    int         Fd;
#endif
    ULONGLONG   Offset;             /* bytes: start of the file system */
    ULONGLONG   Sectors;            /* 0 = unknown */
    ULONG       PartitionStart;     /* sectors (hidden sectors) */
    ULONG       SectorsPerTrack;    /* drive geometry, 0 = unknown */
    ULONG       Heads;
    int         ReadOnly;
    void       *Maps;
    ULONG       Mapped;
    ULONGLONG   Reads, Writes;
    int         Defer;              /* keep dirty metadata until ExtHostFlush (like the driver) */
    ULONG       Flushes;
} EXT_IMAGE, *PEXT_IMAGE;

/* Partition: MBR slot 0..3, or -1 for a bare file system */
int  ImgOpen(PEXT_IMAGE Img, const char *Path, int Partition, int ReadOnly);
void ImgClose(PEXT_IMAGE Img);
void ImgDiscard(PEXT_IMAGE Img);
int  ImgRead(PEXT_IMAGE Img, ULONGLONG Sector, ULONG Count, void *Buf);
int  ImgWrite(PEXT_IMAGE Img, ULONGLONG Sector, ULONG Count, const void *Buf);

#ifdef _WIN32
int  ImgOpenDrive(PEXT_IMAGE Img, char Letter, int ReadOnly);
int  ImgLock(PEXT_IMAGE Img);
void ImgDismount(PEXT_IMAGE Img);
void ImgUnlock(PEXT_IMAGE Img);
int  ImgSetType(PEXT_IMAGE Img, UCHAR Type);
#endif

#endif
