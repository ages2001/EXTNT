/*
 * EXTNT - the on-disk structures of the second extended file system family:
 * ext2, ext3 and ext4. Everything here is little-endian on the disk, which
 * is the byte order of every system EXTNT runs on, so the fields are used
 * directly without swapping.
 *
 * References: the Linux kernel (fs/ext4/ext4.h) and the e2fsprogs on-disk
 * documentation. The journal structures (jbd2) are in extjbd.h.
 */

#ifndef _EXTDISK_H_
#define _EXTDISK_H_

#pragma pack(1)

/* ------------------------------------------------------------------ */
/* Superblock (at byte offset 1024, one block or more into the volume) */
/* ------------------------------------------------------------------ */

#define EXT_SUPER_OFFSET        1024
#define EXT_SUPER_MAGIC         0xEF53

typedef struct _EXT_SUPERBLOCK {
    ULONG       InodesCount;            /* 0x00 total inodes */
    ULONG       BlocksCountLo;          /* 0x04 total blocks (low 32) */
    ULONG       RBlocksCountLo;         /* 0x08 reserved blocks */
    ULONG       FreeBlocksCountLo;      /* 0x0C */
    ULONG       FreeInodesCount;        /* 0x10 */
    ULONG       FirstDataBlock;         /* 0x14 block holding the superblock (0 or 1) */
    ULONG       LogBlockSize;           /* 0x18 block size = 1024 << this */
    ULONG       LogClusterSize;         /* 0x1C bigalloc cluster size */
    ULONG       BlocksPerGroup;         /* 0x20 */
    ULONG       ClustersPerGroup;       /* 0x24 */
    ULONG       InodesPerGroup;         /* 0x28 */
    ULONG       MountTime;              /* 0x2C */
    ULONG       WriteTime;              /* 0x30 */
    USHORT      MountCount;             /* 0x34 */
    USHORT      MaxMountCount;          /* 0x36 */
    USHORT      Magic;                  /* 0x38 EXT_SUPER_MAGIC */
    USHORT      State;                  /* 0x3A */
    USHORT      Errors;                 /* 0x3C */
    USHORT      MinorRevLevel;          /* 0x3E */
    ULONG       LastCheck;              /* 0x40 */
    ULONG       CheckInterval;          /* 0x44 */
    ULONG       CreatorOs;              /* 0x48 */
    ULONG       RevLevel;               /* 0x4C 0 = original, 1 = dynamic */
    USHORT      DefResUid;              /* 0x50 */
    USHORT      DefResGid;              /* 0x52 */
    /* EXT2_DYNAMIC_REV */
    ULONG       FirstIno;               /* 0x54 first non-reserved inode */
    USHORT      InodeSize;              /* 0x58 size of an inode */
    USHORT      BlockGroupNr;           /* 0x5A group hosting this superblock */
    ULONG       FeatureCompat;          /* 0x5C */
    ULONG       FeatureIncompat;        /* 0x60 */
    ULONG       FeatureRoCompat;        /* 0x64 */
    UCHAR       Uuid[16];               /* 0x68 */
    char        VolumeName[16];         /* 0x78 */
    char        LastMounted[64];        /* 0x88 */
    ULONG       AlgorithmUsageBitmap;   /* 0xC8 */
    UCHAR       PreallocBlocks;         /* 0xCC */
    UCHAR       PreallocDirBlocks;      /* 0xCD */
    USHORT      ReservedGdtBlocks;      /* 0xCE */
    /* has_journal */
    UCHAR       JournalUuid[16];        /* 0xD0 */
    ULONG       JournalInum;            /* 0xE0 inode of the journal */
    ULONG       JournalDev;             /* 0xE4 */
    ULONG       LastOrphan;             /* 0xE8 head of the orphan inode list */
    ULONG       HashSeed[4];            /* 0xEC htree hash seed */
    UCHAR       DefHashVersion;         /* 0xFC */
    UCHAR       JnlBackupType;          /* 0xFD */
    USHORT      DescSize;               /* 0xFE group descriptor size (64bit) */
    ULONG       DefaultMountOpts;       /* 0x100 */
    ULONG       FirstMetaBg;            /* 0x104 */
    ULONG       MkfsTime;               /* 0x108 */
    ULONG       JnlBlocks[17];          /* 0x10C backup of the journal inode */
    /* 64bit */
    ULONG       BlocksCountHi;          /* 0x150 */
    ULONG       RBlocksCountHi;         /* 0x154 */
    ULONG       FreeBlocksCountHi;      /* 0x158 */
    USHORT      MinExtraIsize;          /* 0x15C */
    USHORT      WantExtraIsize;         /* 0x15E */
    ULONG       Flags;                  /* 0x160 */
    USHORT      RaidStride;             /* 0x164 */
    USHORT      MmpInterval;            /* 0x166 */
    ULONGLONG   MmpBlock;               /* 0x168 */
    ULONG       RaidStripeWidth;        /* 0x170 */
    UCHAR       LogGroupsPerFlex;       /* 0x174 flex_bg group size = 1 << this */
    UCHAR       ChecksumType;           /* 0x175 1 = crc32c */
    USHORT      ReservedPad;            /* 0x176 */
    ULONGLONG   KbytesWritten;          /* 0x178 */
    ULONG       SnapshotInum;           /* 0x180 */
    ULONG       SnapshotId;             /* 0x184 */
    ULONGLONG   SnapshotRBlocksCount;   /* 0x188 */
    ULONG       SnapshotList;           /* 0x190 */
    ULONG       ErrorCount;             /* 0x194 */
    ULONG       FirstErrorTime;         /* 0x198 */
    ULONG       FirstErrorIno;          /* 0x19C */
    ULONGLONG   FirstErrorBlock;        /* 0x1A0 */
    UCHAR       FirstErrorFunc[32];     /* 0x1A8 */
    ULONG       FirstErrorLine;         /* 0x1C8 */
    ULONG       LastErrorTime;          /* 0x1CC */
    ULONG       LastErrorIno;           /* 0x1D0 */
    ULONG       LastErrorLine;          /* 0x1D4 */
    ULONGLONG   LastErrorBlock;         /* 0x1D8 */
    UCHAR       LastErrorFunc[32];      /* 0x1E0 */
    UCHAR       MountOpts[64];          /* 0x200 */
    ULONG       UsrQuotaInum;           /* 0x240 */
    ULONG       GrpQuotaInum;           /* 0x244 */
    ULONG       OverheadBlocks;         /* 0x248 */
    ULONG       BackupBgs[2];           /* 0x24C */
    UCHAR       EncryptAlgos[4];        /* 0x254 */
    UCHAR       EncryptPwSalt[16];      /* 0x258 */
    ULONG       LpfIno;                 /* 0x268 lost+found inode */
    ULONG       PrjQuotaInum;           /* 0x26C */
    ULONG       ChecksumSeed;           /* 0x270 crc32c(~0, uuid) */
    UCHAR       WtimeHi;                /* 0x274 */
    UCHAR       MtimeHi;                /* 0x275 */
    UCHAR       MkfsTimeHi;             /* 0x276 */
    UCHAR       LastcheckHi;            /* 0x277 */
    UCHAR       FirstErrorTimeHi;       /* 0x278 */
    UCHAR       LastErrorTimeHi;        /* 0x279 */
    UCHAR       Pad2[2];                /* 0x27A */
    USHORT      Encoding;               /* 0x27C casefold */
    USHORT      EncodingFlags;          /* 0x27E */
    ULONG       OrphanFileInum;         /* 0x280 */
    ULONG       Reserved[94];           /* 0x284 */
    ULONG       Checksum;               /* 0x3FC crc32c of the superblock */
} EXT_SUPERBLOCK, *PEXT_SUPERBLOCK;

/* State */
#define EXT_VALID_FS            0x0001  /* cleanly unmounted */
#define EXT_ERROR_FS            0x0002  /* errors detected */
#define EXT_ORPHAN_FS           0x0004  /* orphans being recovered */

/* FeatureCompat */
#define EXT_FC_DIR_PREALLOC     0x0001
#define EXT_FC_IMAGIC_INODES    0x0002
#define EXT_FC_HAS_JOURNAL      0x0004
#define EXT_FC_EXT_ATTR         0x0008
#define EXT_FC_RESIZE_INODE     0x0010
#define EXT_FC_DIR_INDEX        0x0020  /* htree directories */
#define EXT_FC_SPARSE_SUPER2    0x0200
#define EXT_FC_FAST_COMMIT      0x0400
#define EXT_FC_ORPHAN_FILE      0x1000

/* FeatureIncompat - a volume with an unknown bit here must not be mounted */
#define EXT_FI_COMPRESSION      0x0001
#define EXT_FI_FILETYPE         0x0002  /* dir entries carry a file type */
#define EXT_FI_RECOVER          0x0004  /* needs journal recovery */
#define EXT_FI_JOURNAL_DEV      0x0008
#define EXT_FI_META_BG          0x0010
#define EXT_FI_EXTENTS          0x0040  /* inodes may use extent trees */
#define EXT_FI_64BIT            0x0080  /* 64-bit block numbers */
#define EXT_FI_MMP              0x0100
#define EXT_FI_FLEX_BG          0x0200
#define EXT_FI_EA_INODE         0x0400  /* large xattrs in their own inode */
#define EXT_FI_DIRDATA          0x1000
#define EXT_FI_CSUM_SEED        0x2000
#define EXT_FI_LARGEDIR         0x4000  /* 3-level htree, >2 GB dirs */
#define EXT_FI_INLINE_DATA      0x8000  /* small files/dirs inside the inode */
#define EXT_FI_ENCRYPT          0x10000
#define EXT_FI_CASEFOLD         0x20000

/* FeatureRoCompat - unknown bit here: mount read-only */
#define EXT_RO_SPARSE_SUPER     0x0001
#define EXT_RO_LARGE_FILE       0x0002
#define EXT_RO_HUGE_FILE        0x0008
#define EXT_RO_GDT_CSUM         0x0010  /* group descriptors have a crc16 */
#define EXT_RO_DIR_NLINK        0x0020
#define EXT_RO_EXTRA_ISIZE      0x0040
#define EXT_RO_HAS_SNAPSHOT     0x0080
#define EXT_RO_QUOTA            0x0100
#define EXT_RO_BIGALLOC         0x0200
#define EXT_RO_METADATA_CSUM    0x0400  /* crc32c everywhere, supersedes GDT_CSUM */
#define EXT_RO_READONLY         0x1000
#define EXT_RO_PROJECT          0x2000
#define EXT_RO_ORPHAN_PRESENT   0x10000

/* The features EXTNT understands. A volume with any other incompat bit is
   refused; any other ro_compat bit means read-only. */
#define EXT_FI_SUPPORTED        (EXT_FI_FILETYPE | EXT_FI_RECOVER | EXT_FI_META_BG | \
                                 EXT_FI_EXTENTS | EXT_FI_64BIT | EXT_FI_FLEX_BG | \
                                 EXT_FI_INLINE_DATA | EXT_FI_CSUM_SEED | EXT_FI_LARGEDIR | \
                                 EXT_FI_EA_INODE)
#define EXT_RO_SUPPORTED        (EXT_RO_SPARSE_SUPER | EXT_RO_LARGE_FILE | EXT_RO_HUGE_FILE | \
                                 EXT_RO_GDT_CSUM | EXT_RO_DIR_NLINK | EXT_RO_EXTRA_ISIZE | \
                                 EXT_RO_METADATA_CSUM | EXT_RO_BIGALLOC)
/* Incompat features that can be read but not written: the volume is mounted
   read-only (external journal, multi-mount protection, encrypted and
   case-folded directories) */
#define EXT_FI_READ_ONLY        (EXT_FI_JOURNAL_DEV | EXT_FI_MMP | EXT_FI_ENCRYPT | EXT_FI_CASEFOLD)

/* ------------------------------------------------------------------ */
/* Block group descriptor (32 bytes, or 64 with the 64bit feature)     */
/* ------------------------------------------------------------------ */

typedef struct _EXT_GROUP_DESC {
    ULONG       BlockBitmapLo;          /* 0x00 */
    ULONG       InodeBitmapLo;          /* 0x04 */
    ULONG       InodeTableLo;           /* 0x08 */
    USHORT      FreeBlocksCountLo;      /* 0x0C */
    USHORT      FreeInodesCountLo;      /* 0x0E */
    USHORT      UsedDirsCountLo;        /* 0x10 */
    USHORT      Flags;                  /* 0x12 */
    ULONG       ExcludeBitmapLo;        /* 0x14 */
    USHORT      BlockBitmapCsumLo;      /* 0x18 */
    USHORT      InodeBitmapCsumLo;      /* 0x1A */
    USHORT      ItableUnusedLo;         /* 0x1C unused inodes at the tail */
    USHORT      Checksum;               /* 0x1E crc16 or crc32c-low */
    /* 64bit (DescSize > 32) */
    ULONG       BlockBitmapHi;          /* 0x20 */
    ULONG       InodeBitmapHi;          /* 0x24 */
    ULONG       InodeTableHi;           /* 0x28 */
    USHORT      FreeBlocksCountHi;      /* 0x2C */
    USHORT      FreeInodesCountHi;      /* 0x2E */
    USHORT      UsedDirsCountHi;        /* 0x30 */
    USHORT      ItableUnusedHi;         /* 0x32 */
    ULONG       ExcludeBitmapHi;        /* 0x34 */
    USHORT      BlockBitmapCsumHi;      /* 0x38 */
    USHORT      InodeBitmapCsumHi;      /* 0x3A */
    ULONG       Reserved;               /* 0x3C */
} EXT_GROUP_DESC, *PEXT_GROUP_DESC;

#define EXT_BG_INODE_UNINIT     0x0001  /* inode bitmap/table not initialised */
#define EXT_BG_BLOCK_UNINIT     0x0002  /* block bitmap not initialised */
#define EXT_BG_INODE_ZEROED     0x0004  /* inode table has been zeroed */

/* ------------------------------------------------------------------ */
/* Inode                                                                */
/* ------------------------------------------------------------------ */

#define EXT_NDIR_BLOCKS         12
#define EXT_IND_BLOCK           12      /* single indirect */
#define EXT_DIND_BLOCK          13      /* double indirect */
#define EXT_TIND_BLOCK          14      /* triple indirect */
#define EXT_N_BLOCKS            15

typedef struct _EXT_INODE {
    USHORT      Mode;                   /* 0x00 type and permissions */
    USHORT      UidLo;                  /* 0x02 */
    ULONG       SizeLo;                 /* 0x04 */
    ULONG       Atime;                  /* 0x08 */
    ULONG       Ctime;                  /* 0x0C */
    ULONG       Mtime;                  /* 0x10 */
    ULONG       Dtime;                  /* 0x14 deletion time */
    USHORT      GidLo;                  /* 0x18 */
    USHORT      LinksCount;             /* 0x1A */
    ULONG       BlocksLo;              /* 0x1C 512-byte sectors used (low) */
    ULONG       Flags;                  /* 0x20 */
    ULONG       Version;                /* 0x24 (osd1) */
    ULONG       Block[EXT_N_BLOCKS];    /* 0x28 block map / extent root / inline data */
    ULONG       Generation;             /* 0x64 */
    ULONG       FileAclLo;              /* 0x68 xattr block (low) */
    ULONG       SizeHi;                 /* 0x6C (dir_acl on dirs in old fs) */
    ULONG       ObsoFaddr;              /* 0x70 */
    USHORT      BlocksHi;               /* 0x74 (osd2 linux: l_i_blocks_high) */
    USHORT      FileAclHi;              /* 0x76 */
    USHORT      UidHi;                  /* 0x78 */
    USHORT      GidHi;                  /* 0x7A */
    USHORT      ChecksumLo;             /* 0x7C crc32c low */
    USHORT      Reserved;               /* 0x7E */
    /* extra fields when InodeSize > 128 and ExtraIsize covers them */
    USHORT      ExtraIsize;             /* 0x80 */
    USHORT      ChecksumHi;             /* 0x82 */
    ULONG       CtimeExtra;             /* 0x84 nanoseconds<<2 | epoch bits */
    ULONG       MtimeExtra;             /* 0x88 */
    ULONG       AtimeExtra;             /* 0x8C */
    ULONG       Crtime;                 /* 0x90 creation time */
    ULONG       CrtimeExtra;            /* 0x94 */
    ULONG       VersionHi;              /* 0x98 */
    ULONG       ProjId;                 /* 0x9C */
} EXT_INODE, *PEXT_INODE;

#define EXT_GOOD_OLD_INODE_SIZE 128

/* Mode: type (high 4 bits) */
#define EXT_S_IFMT              0xF000
#define EXT_S_IFSOCK            0xC000
#define EXT_S_IFLNK             0xA000
#define EXT_S_IFREG             0x8000
#define EXT_S_IFBLK             0x6000
#define EXT_S_IFDIR             0x4000
#define EXT_S_IFCHR             0x2000
#define EXT_S_IFIFO             0x1000

/* Inode flags */
#define EXT_IF_SECRM            0x00000001
#define EXT_IF_UNRM             0x00000002
#define EXT_IF_COMPR            0x00000004
#define EXT_IF_SYNC             0x00000008
#define EXT_IF_IMMUTABLE        0x00000010
#define EXT_IF_APPEND           0x00000020
#define EXT_IF_NODUMP           0x00000040
#define EXT_IF_NOATIME          0x00000080
#define EXT_IF_ENCRYPT          0x00000800  /* fscrypt: contents/names encrypted */
#define EXT_IF_INDEX            0x00001000  /* htree directory */
#define EXT_IF_IMAGIC           0x00002000
#define EXT_IF_JOURNAL_DATA     0x00004000
#define EXT_IF_NOTAIL           0x00008000
#define EXT_IF_DIRSYNC          0x00010000
#define EXT_IF_TOPDIR           0x00020000
#define EXT_IF_HUGE_FILE        0x00040000
#define EXT_IF_EXTENTS          0x00080000  /* uses an extent tree */
#define EXT_IF_EA_INODE         0x00200000
#define EXT_IF_INLINE_DATA      0x10000000
#define EXT_IF_CASEFOLD         0x40000000  /* case-insensitive directory */

/* Reserved inode numbers */
#define EXT_BAD_INO             1
#define EXT_ROOT_INO            2
#define EXT_USR_QUOTA_INO       3
#define EXT_GRP_QUOTA_INO       4
#define EXT_BOOT_LOADER_INO     5
#define EXT_UNDEL_DIR_INO       6
#define EXT_RESIZE_INO          7
#define EXT_JOURNAL_INO         8
#define EXT_FIRST_NONRES_INO    11      /* when FirstIno is the classic value */

/* ------------------------------------------------------------------ */
/* Extent tree (in Block[] of an EXT_IF_EXTENTS inode, and in blocks)   */
/* ------------------------------------------------------------------ */

#define EXT_EXTENT_MAGIC        0xF30A

typedef struct _EXT_EXTENT_HEADER {
    USHORT      Magic;                  /* EXT_EXTENT_MAGIC */
    USHORT      Entries;                /* valid entries that follow */
    USHORT      Max;                    /* capacity */
    USHORT      Depth;                  /* 0 = leaf (extents), >0 = index */
    ULONG       Generation;
} EXT_EXTENT_HEADER, *PEXT_EXTENT_HEADER;

typedef struct _EXT_EXTENT_IDX {        /* internal node entry */
    ULONG       Block;                  /* covers file blocks from here */
    ULONG       LeafLo;                 /* child node block (low) */
    USHORT      LeafHi;
    USHORT      Unused;
} EXT_EXTENT_IDX, *PEXT_EXTENT_IDX;

typedef struct _EXT_EXTENT {            /* leaf entry */
    ULONG       Block;                  /* first file block */
    USHORT      Len;                    /* blocks; >32768 means uninitialised, len-32768 */
    USHORT      StartHi;
    ULONG       StartLo;                /* first disk block */
} EXT_EXTENT, *PEXT_EXTENT;

#define EXT_EXTENT_INIT_MAX     32768   /* len above this = uninitialised extent */
/* A crc32c tail follows the entries when metadata_csum is set */
typedef struct _EXT_EXTENT_TAIL {
    ULONG       Checksum;
} EXT_EXTENT_TAIL, *PEXT_EXTENT_TAIL;

/* ------------------------------------------------------------------ */
/* Directory entries                                                    */
/* ------------------------------------------------------------------ */

typedef struct _EXT_DIR_ENTRY {
    ULONG       Inode;                  /* 0 = unused slot */
    USHORT      RecLen;                 /* to the next entry */
    UCHAR       NameLen;                /* with filetype feature: low byte of name length */
    UCHAR       FileType;               /* with filetype feature */
    char        Name[255];
} EXT_DIR_ENTRY, *PEXT_DIR_ENTRY;

#define EXT_DIR_HEADER          8       /* bytes before Name */

#define EXT_FT_UNKNOWN          0
#define EXT_FT_REG_FILE         1
#define EXT_FT_DIR              2
#define EXT_FT_CHRDEV           3
#define EXT_FT_BLKDEV           4
#define EXT_FT_FIFO             5
#define EXT_FT_SOCK            6
#define EXT_FT_SYMLINK          7
#define EXT_FT_DIR_CSUM         0xDE    /* a fake entry (inode 0, name len 0) holding the block crc32c */

/* rec_len rounded up to 4 */
#define EXT_DIR_REC_LEN(NameLen) (((EXT_DIR_HEADER + (NameLen)) + 3) & ~3U)

/* The tail of a directory block with metadata_csum: a dir entry with
   Inode = 0, RecLen = 12, NameLen = 0, FileType = 0xDE, then the crc32c */
typedef struct _EXT_DIR_TAIL {
    ULONG       ReservedZero1;
    USHORT      RecLen;                 /* 12 */
    UCHAR       ReservedZero2;
    UCHAR       Reserved0xDE;
    ULONG       Checksum;
} EXT_DIR_TAIL, *PEXT_DIR_TAIL;

/* ------------------------------------------------------------------ */
/* Hashed directory tree (htree) - an index over the directory blocks   */
/* ------------------------------------------------------------------ */

typedef struct _EXT_DX_ROOT {
    /* occupies dirblock 0; "." and ".." come first as normal dir entries */
    EXT_DIR_ENTRY Dot;                  /* not used as a struct; see extdir.c */
    UCHAR       Raw[1];
} EXT_DX_ROOT, *PEXT_DX_ROOT;

typedef struct _EXT_DX_ENTRY {
    ULONG       Hash;                   /* 0 for the first entry of a node */
    ULONG       Block;                  /* logical dir block of the child */
} EXT_DX_ENTRY, *PEXT_DX_ENTRY;

#define EXT_DX_HASH_LEGACY          0
#define EXT_DX_HASH_HALF_MD4        1
#define EXT_DX_HASH_TEA             2
#define EXT_DX_HASH_LEGACY_UNSIGNED 3
#define EXT_DX_HASH_HALF_MD4_UNSIGNED 4
#define EXT_DX_HASH_TEA_UNSIGNED    5

/* ------------------------------------------------------------------ */
/* Extended attributes                                                  */
/* ------------------------------------------------------------------ */

#define EXT_XATTR_MAGIC         0xEA020000

typedef struct _EXT_XATTR_HEADER {
    ULONG       Magic;                  /* EXT_XATTR_MAGIC */
    ULONG       Refcount;
    ULONG       Blocks;                 /* always 1 */
    ULONG       Hash;
    ULONG       Checksum;
    ULONG       Reserved[3];
} EXT_XATTR_HEADER, *PEXT_XATTR_HEADER;

/* In the inode's extra space, the entries follow this 4-byte magic */
typedef struct _EXT_XATTR_IBODY_HEADER {
    ULONG       Magic;                  /* EXT_XATTR_MAGIC */
} EXT_XATTR_IBODY_HEADER, *PEXT_XATTR_IBODY_HEADER;

typedef struct _EXT_XATTR_ENTRY {
    UCHAR       NameLen;
    UCHAR       NameIndex;              /* namespace prefix code */
    USHORT      ValueOffs;              /* from the start of the value area */
    ULONG       ValueInum;             /* value in its own inode, else 0 */
    ULONG       ValueSize;
    ULONG       Hash;
    char        Name[1];
} EXT_XATTR_ENTRY, *PEXT_XATTR_ENTRY;

#define EXT_XATTR_INDEX_USER                1
#define EXT_XATTR_INDEX_POSIX_ACL_ACCESS    2
#define EXT_XATTR_INDEX_POSIX_ACL_DEFAULT   3
#define EXT_XATTR_INDEX_TRUSTED             4
#define EXT_XATTR_INDEX_SECURITY            6
#define EXT_XATTR_INDEX_SYSTEM              7

#define EXT_XATTR_ENTRY_SIZE(NameLen) (((sizeof(EXT_XATTR_ENTRY) - 1 + (NameLen)) + 3) & ~3U)
#define EXT_XATTR_PAD           4
#define EXT_XATTR_ROUND         (EXT_XATTR_PAD - 1)
#define EXT_XATTR_SIZE(Size)   (((Size) + EXT_XATTR_ROUND) & ~EXT_XATTR_ROUND)

#pragma pack()

#endif /* _EXTDISK_H_ */
