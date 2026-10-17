/*
 * EXTNT - the ext3/ext4 journal (jbd2) on-disk format.
 *
 * The journal is an ordinary file (inode 8) whose blocks hold a log of
 * metadata (and, in data=journal, data). It opens with a superblock, then
 * a sequence of transactions; each is a descriptor block naming the final
 * homes of the data blocks that follow it, the data blocks themselves, and
 * a commit block. On mount a volume that needs recovery is replayed: the
 * logged blocks are written to their homes. EXTNT also writes real
 * transactions so a crash leaves the metadata consistent.
 *
 * All journal fields are big-endian on disk (unlike the rest of ext), so
 * they go through the Be* helpers.
 */

#ifndef _EXTJBD_H_
#define _EXTJBD_H_

#pragma pack(1)

#define JBD_MAGIC               0xC03B3998

/* Block types (h_blocktype) */
#define JBD_BT_DESCRIPTOR       1
#define JBD_BT_COMMIT           2
#define JBD_BT_SUPERBLOCK_V1    3
#define JBD_BT_SUPERBLOCK_V2    4
#define JBD_BT_REVOKE           5

typedef struct _JBD_HEADER {
    ULONG       Magic;                  /* JBD_MAGIC (big-endian) */
    ULONG       BlockType;
    ULONG       Sequence;
} JBD_HEADER, *PJBD_HEADER;

typedef struct _JBD_SUPERBLOCK {
    JBD_HEADER  Header;
    /* static */
    ULONG       BlockSize;
    ULONG       MaxLen;                 /* journal length in blocks */
    ULONG       First;                  /* first block of the log */
    /* dynamic */
    ULONG       Sequence;               /* first expected commit ID */
    ULONG       Start;                  /* block of the first transaction, 0 = empty */
    ULONG       ErrNo;
    /* v2 */
    ULONG       FeatureCompat;
    ULONG       FeatureIncompat;
    ULONG       FeatureRoCompat;
    UCHAR       Uuid[16];
    ULONG       NrUsers;
    ULONG       DynSuper;
    ULONG       MaxTransaction;
    ULONG       MaxTransData;
    UCHAR       ChecksumType;
    UCHAR       Padding2[3];
    ULONG       NumFcBlks;
    ULONG       Head;                   /* 0x58 */
    ULONG       Padding[40];            /* 0x5C */
    ULONG       Checksum;               /* 0xFC */
    UCHAR       Users[16 * 48];         /* 0x100 */
} JBD_SUPERBLOCK, *PJBD_SUPERBLOCK;

#define JBD_FI_REVOKE           0x0001
#define JBD_FI_64BIT            0x0002
#define JBD_FI_ASYNC_COMMIT     0x0004
#define JBD_FI_CSUM_V2          0x0008
#define JBD_FI_CSUM_V3          0x0010
#define JBD_FI_FAST_COMMIT      0x0020

#define JBD_FI_SUPPORTED        (JBD_FI_REVOKE | JBD_FI_64BIT | JBD_FI_CSUM_V2 | JBD_FI_CSUM_V3)

/* A descriptor block is the header followed by an array of these tags */
typedef struct _JBD_BLOCK_TAG3 {        /* csum_v3 tag */
    ULONG       BlockNrLo;
    ULONG       Flags;
    ULONG       BlockNrHi;
    ULONG       Checksum;
} JBD_BLOCK_TAG3, *PJBD_BLOCK_TAG3;

typedef struct _JBD_BLOCK_TAG {         /* classic / csum_v2 tag */
    ULONG       BlockNr;                /* low 32 bits of the home block */
    USHORT      Checksum;               /* csum_v2: low 16 of the block crc */
    USHORT      Flags;
    ULONG       BlockNrHi;              /* only with the 64bit feature */
} JBD_BLOCK_TAG, *PJBD_BLOCK_TAG;

#define JBD_FLAG_ESCAPE         1       /* block began with the jbd magic, escaped */
#define JBD_FLAG_SAME_UUID      2       /* no 16-byte uuid follows this tag */
#define JBD_FLAG_DELETED        4
#define JBD_FLAG_LAST_TAG       8

typedef struct _JBD_COMMIT_BLOCK {
    JBD_HEADER  Header;
    UCHAR       ChksumType;
    UCHAR       ChksumSize;
    UCHAR       Padding[2];
    ULONG       Checksum;               /* csum_v2+: crc32c of the commit block */
    ULONG       CommitSec;
    ULONG       CommitNsec;
} JBD_COMMIT_BLOCK, *PJBD_COMMIT_BLOCK;

typedef struct _JBD_REVOKE_HEADER {
    JBD_HEADER  Header;
    ULONG       Count;                  /* bytes used, including this header */
    /* then big-endian block numbers (4 or 8 bytes each) */
} JBD_REVOKE_HEADER, *PJBD_REVOKE_HEADER;

#pragma pack()

#endif /* _EXTJBD_H_ */
