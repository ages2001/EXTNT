/* EXTNT - making an ext2/ext3/ext4 file system (see extmkfs.c) */
#ifndef _EXTMKFS_H_
#define _EXTMKFS_H_

#define EXT_MKFS_DEFAULT    0xFFFFFFFFUL

typedef struct _EXT_MKFS {
    int         Type;               /* 2, 3 or 4 */
    ULONG       BlockSize;          /* 0 = by size (1024 below 512 MB, else 4096) */
    ULONG       ClusterSize;        /* bigalloc: allocation unit, a power of two over the block size; 0 = none */
    ULONG       InodeSize;          /* 0 = 256 (128 for floppies, 128 on rev 0) */
    ULONG       InodeRatio;         /* bytes per inode, 0 = by size */
    ULONG       Inodes;             /* exact count instead of the ratio, 0 = no */
    ULONG       ReservedPercent;    /* root reserve; EXT_MKFS_DEFAULT = 5 */
    ULONG       JournalBlocks;      /* 0 = default size by volume size */
    ULONG       BlocksPerGroup;     /* 0 = 8 * block size */
    ULONG       FlexGroups;         /* ext4: groups per flex group (power of two), 0 = 16 */
    ULONG       FeaturesOn[3];      /* compat, incompat, ro_compat to add */
    ULONG       FeaturesOff[3];     /* ... and to take away */
    int         Revision0;          /* the original ext2 format (no features at all) */
    int         Lazy;               /* ext4: do not zero inode tables (uninit groups) */
    int         Quiet;
    int         Boot;               /* reserve the boot area (inode 5) for extboot */
    char        Label[17];          /* UTF-8, up to 16 bytes */
    UCHAR       Uuid[16];           /* all zero = make one */
} EXT_MKFS, *PEXT_MKFS;

/* Makes the file system over the whole of Img (Img->Sectors sectors).
   Returns 0, or -1 with a message in Error. */
/* The boot area: FAT and root directory of the FAT16 view (272 sectors)
   plus room for its directories */
#define EXT_BOOT_FAT_SECTORS    (256 + 16)
#define EXT_BOOT_DIR_BYTES      (256 * 1024)

int ExtMakeFs(PEXT_IMAGE Img, PEXT_MKFS P, char *Error, unsigned ErrorSize);

/* Fills in the defaults mke2fs would choose for a volume of Bytes */
void ExtMkfsDefaults(PEXT_MKFS P, ULONGLONG Bytes);

#endif
