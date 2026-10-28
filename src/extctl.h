/*
 * EXTNT - private file system controls (driver and tools)
 *
 *   EXN_FSCTL_QUERY_VOLUME  any handle of the volume: EXN_VOLUME_INFO
 *   EXN_FSCTL_GET_POSIX     any file handle: EXN_POSIX_INFO (mode, owner,
 *                           links, inode number, flags)
 *   EXN_FSCTL_SET_POSIX     file handle opened with FILE_WRITE_ATTRIBUTES:
 *                           input EXN_POSIX_INFO, Valid says which fields
 *   EXN_FSCTL_READ_LINK     a symbolic link opened as itself (DELETE access
 *                           or FILE_OPEN_REPARSE_POINT): the target text
 *   EXN_FSCTL_CREATE_LINK   a directory handle: input EXN_LINK_REQUEST
 *                           makes a symbolic link Name -> Target in it
 *   EXN_FSCTL_COMMIT        any handle: commit the journal now
 */

#ifndef _EXTCTL_H_
#define _EXTCTL_H_

#define EXN_FSCTL_QUERY_VOLUME  CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 0x8B0, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define EXN_FSCTL_GET_POSIX     CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 0x8B1, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define EXN_FSCTL_SET_POSIX     CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 0x8B2, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define EXN_FSCTL_READ_LINK     CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 0x8B3, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define EXN_FSCTL_CREATE_LINK   CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 0x8B4, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define EXN_FSCTL_COMMIT        CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 0x8B5, METHOD_BUFFERED, FILE_ANY_ACCESS)

typedef struct _EXN_VOLUME_INFO {
    ULONG       Version;            /* 1 */
    ULONG       BlockSize;
    ULONGLONG   BlocksCount;
    ULONGLONG   FreeBlocks;
    ULONG       InodesCount;
    ULONG       FreeInodes;
    ULONG       InodeSize;
    ULONG       GroupCount;
    ULONG       FeatureCompat;
    ULONG       FeatureIncompat;
    ULONG       FeatureRoCompat;
    ULONG       ReadOnly;           /* mounted read-only (asked or forced) */
    ULONG       ReadOnlyForced;     /* a feature EXTNT cannot write */
    ULONG       HasJournal;
    ULONG       Commits;            /* journal transactions written */
    UCHAR       Uuid[16];
    char        Label[17];
    char        Pad[3];
} EXN_VOLUME_INFO, *PEXN_VOLUME_INFO;

#define EXN_POSIX_MODE          0x01
#define EXN_POSIX_UID           0x02
#define EXN_POSIX_GID           0x04

typedef struct _EXN_POSIX_INFO {
    ULONG       Valid;              /* SET: which fields to apply */
    ULONG       Mode;               /* type and permission bits (st_mode) */
    ULONG       Uid;
    ULONG       Gid;
    ULONG       Links;
    ULONG       Inode;
    ULONG       Flags;              /* ext inode flags */
    ULONG       Generation;
} EXN_POSIX_INFO, *PEXN_POSIX_INFO;

typedef struct _EXN_LINK_REQUEST {
    USHORT      NameLength;         /* bytes of UTF-16 */
    USHORT      TargetLength;       /* bytes of the UTF-8 target that follows the name */
    WCHAR       Name[1];            /* then the target (UTF-8, not terminated) */
} EXN_LINK_REQUEST, *PEXN_LINK_REQUEST;

#endif
