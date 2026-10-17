/*
 * EXTNT - files, directories, symlinks and hard links: create, unlink, link,
 * rename, read the target of a symlink, and read/write regular-file data.
 */

#ifdef EXT_KERNEL
#include "ext.h"
#else
#include "extusr.h"
#include "extsup.h"
#endif

#define EXT_LINK_MAX    65000

/* A directory's link count is 2 + subdirectories; past EXT_LINK_MAX it is
   pinned at 1 ("many"), which needs the dir_nlink feature (Linux rules) */
static BOOLEAN
DirCanGainSubdir(PEXT_VOL Vol, PEXT_INODE Dir)
{
    return (BOOLEAN)(Dir->LinksCount == 1 || Dir->LinksCount + 1 < EXT_LINK_MAX ||
                     (Vol->FeatureRoCompat & EXT_RO_DIR_NLINK));
}

static VOID
DirLinkInc(PEXT_VOL Vol, PEXT_INODE Dir)
{
    if (Dir->LinksCount == 1) return;
    if (Dir->LinksCount + 1 >= EXT_LINK_MAX) Dir->LinksCount = 1;
    else Dir->LinksCount++;
    (VOID)Vol;
}

static VOID
DirLinkDec(PEXT_INODE Dir)
{
    if (Dir->LinksCount > 2) Dir->LinksCount--;
}

static UCHAR
ExtModeToFileType(USHORT Mode)
{
    switch (Mode & EXT_S_IFMT) {
    case EXT_S_IFREG:  return EXT_FT_REG_FILE;
    case EXT_S_IFDIR:  return EXT_FT_DIR;
    case EXT_S_IFLNK:  return EXT_FT_SYMLINK;
    case EXT_S_IFCHR:  return EXT_FT_CHRDEV;
    case EXT_S_IFBLK:  return EXT_FT_BLKDEV;
    case EXT_S_IFIFO:  return EXT_FT_FIFO;
    case EXT_S_IFSOCK: return EXT_FT_SOCK;
    default:           return EXT_FT_UNKNOWN;
    }
}

/* Fill a brand-new inode's common fields */
static VOID
ExtInitInode(PEXT_VOL Vol, PEXT_INODE Inode, USHORT Mode)
{
    ULONG Now = ExtHostNow();
    ExtZero(Inode, sizeof(EXT_INODE));
    Inode->Mode = Mode;
    Inode->LinksCount = 1;
    Inode->Atime = Inode->Ctime = Inode->Mtime = Now;
    if (Vol->InodeSize > EXT_GOOD_OLD_INODE_SIZE) {
        USHORT Want = Vol->Super->WantExtraIsize ? Vol->Super->WantExtraIsize : 32;
        if (Want > Vol->InodeSize - EXT_GOOD_OLD_INODE_SIZE) Want = Vol->InodeSize - EXT_GOOD_OLD_INODE_SIZE;
        Inode->ExtraIsize = Want;
        Inode->Crtime = Now;
    }
}

/* Put an empty extent header into i_block for a new extent-based inode */
static VOID
ExtInitExtents(PEXT_VOL Vol, PEXT_INODE Inode)
{
    PEXT_EXTENT_HEADER H = (PEXT_EXTENT_HEADER)Inode->Block;
    ExtZero(Inode->Block, sizeof(Inode->Block));
    H->Magic = EXT_EXTENT_MAGIC;
    H->Entries = 0;
    H->Max = (USHORT)((sizeof(Inode->Block) - sizeof(EXT_EXTENT_HEADER)) / sizeof(EXT_EXTENT));
    H->Depth = 0;
    Inode->Flags |= EXT_IF_EXTENTS;
    (VOID)Vol;
}

EXT_STATUS
ExtReadLink(PEXT_VOL Vol, ULONG Ino, PEXT_INODE Inode, char *Buf, ULONG BufSize, PULONG Len)
{
    ULONGLONG Size = ExtInodeSize(Vol, Inode);
    ULONG Read;
    EXT_STATUS S;
    if ((Inode->Mode & EXT_S_IFMT) != EXT_S_IFLNK) return EXT_NOT_SUPPORTED;
    if (Size >= BufSize || Size > 65536) return EXT_NAME_TOO_LONG;
    S = ExtReadFile(Vol, Ino, Inode, 0, (ULONG)Size, Buf, &Read);
    if (!NT_SUCCESS(S)) return S;
    Buf[Read] = 0;
    *Len = Read;
    return EXT_OK;
}

/* Moves an inline file's data out to blocks (directories: see extdir.c) */
EXT_STATUS
ExtUninline(PEXT_VOL Vol, ULONG Ino, PEXT_INODE Inode)
{
    PUCHAR D;
    ULONG Len, Size, w;
    EXT_STATUS S;

    if (!(Inode->Flags & EXT_IF_INLINE_DATA)) return EXT_OK;
    if ((Inode->Mode & EXT_S_IFMT) == EXT_S_IFDIR) {
        return ExtUninlineDir(Vol, Ino, Inode);
    }
    S = ExtInlineLoad(Vol, Ino, Inode, &D, &Len);
    if (!NT_SUCCESS(S)) return S;
    Size = (ULONG)ExtInodeSize(Vol, Inode);
    if (Size > Len) Size = Len;

    Inode->Flags &= ~EXT_IF_INLINE_DATA;
    ExtZero(Inode->Block, sizeof(Inode->Block));
    if (Vol->HasExtents) ExtInitExtents(Vol, Inode);
    ExtInodeSetSize(Vol, Inode, 0);
    S = ExtSetXattr(Vol, Ino, Inode, EXT_XATTR_INDEX_SYSTEM, "data", 4, NULL, 0);
    if (NT_SUCCESS(S) && Size) S = ExtWriteFile(Vol, Ino, Inode, 0, Size, D, &w);
    ExtHostFree(D);
    if (!NT_SUCCESS(S)) return S;
    return ExtWriteInode(Vol, Ino, Inode);
}

EXT_STATUS
ExtCreate(PEXT_VOL Vol, ULONG DirIno, PEXT_INODE Dir, const char *Name, ULONG NameLen,
          USHORT Mode, const char *Target, PULONG NewIno, PEXT_INODE NewInode)
{
    ULONG Ino;
    EXT_INODE Inode;
    BOOLEAN IsDir = (BOOLEAN)((Mode & EXT_S_IFMT) == EXT_S_IFDIR);
    BOOLEAN IsLnk = (BOOLEAN)((Mode & EXT_S_IFMT) == EXT_S_IFLNK);
    BOOLEAN IsReg = (BOOLEAN)((Mode & EXT_S_IFMT) == EXT_S_IFREG);
    EXT_ENTRY Tmp;
    EXT_STATUS S;

    if (Vol->ReadOnly) return EXT_READ_ONLY;
    if (NameLen == 0 || NameLen > EXT_MAX_NAME) return EXT_BAD_NAME;
    if (Vol->OldExt) {
        ULONG ino = 0;
        if (Target != NULL) return EXT_NOT_SUPPORTED;   /* no symlink create on old-ext */
        S = ExtOldCreate(Vol, DirIno, Name, NameLen, Mode, &ino);
        if (!NT_SUCCESS(S)) return S;
        if (NewIno) *NewIno = ino;
        if (NewInode) ExtOldReadInode(Vol, ino, NewInode);
        (VOID)Dir; (VOID)IsDir; (VOID)IsLnk; (VOID)IsReg;
        return EXT_OK;
    }

    /* must not already exist */
    S = ExtLookup(Vol, DirIno, Dir, Name, NameLen, &Tmp);
    if (NT_SUCCESS(S)) return EXT_EXISTS;
    if (S != EXT_NOT_FOUND) return S;

    if (IsDir && !DirCanGainSubdir(Vol, Dir)) return EXT_TOO_MANY_LINKS;
    S = ExtAllocInode(Vol, IsDir, DirIno, &Ino);
    if (!NT_SUCCESS(S)) return S;
    S = ExtZeroInodeImage(Vol, Ino);
    if (!NT_SUCCESS(S)) { ExtFreeInode(Vol, Ino, IsDir); return S; }

    ExtInitInode(Vol, &Inode, Mode);

    if (IsLnk) {
        ULONG TLen = 0;
        while (Target && Target[TLen]) TLen++;
        if (TLen > Vol->BlockSize) { ExtFreeInode(Vol, Ino, FALSE); return EXT_NAME_TOO_LONG; }
        if (TLen < 60) {
            ExtCopy(Inode.Block, Target, TLen);
            ExtInodeSetSize(Vol, &Inode, TLen);
        } else {
            ULONG Written;
            if (Vol->HasExtents) ExtInitExtents(Vol, &Inode);
            S = ExtWriteFile(Vol, Ino, &Inode, 0, TLen, Target, &Written);
            if (!NT_SUCCESS(S)) { ExtFreeInodeData(Vol, Ino, &Inode); ExtFreeInode(Vol, Ino, FALSE); return S; }
        }
    } else if (IsDir) {
        ULONGLONG Disk;
        EXT_BUF Buf;
        PEXT_DIR_ENTRY De;
        ULONG End = Vol->HasMetaCsum ? Vol->BlockSize - (ULONG)sizeof(EXT_DIR_TAIL) : Vol->BlockSize;
        ULONG RecDot = EXT_DIR_REC_LEN(1);

        Inode.LinksCount = 2;       /* "." and the parent's entry */
        if (Vol->HasExtents) ExtInitExtents(Vol, &Inode);
        S = ExtGetFileBlock(Vol, Ino, &Inode, 0, &Disk, NULL);
        if (!NT_SUCCESS(S)) { ExtFreeInode(Vol, Ino, TRUE); return S; }
        ExtInodeSetSize(Vol, &Inode, Vol->BlockSize);
        S = ExtMapBlocks(Vol, Disk, 1, EXT_MAP_NEW, &Buf);
        if (!NT_SUCCESS(S)) { ExtFreeInodeData(Vol, Ino, &Inode); ExtFreeInode(Vol, Ino, TRUE); return S; }
        De = (PEXT_DIR_ENTRY)Buf.Data;
        De->Inode = Ino; De->RecLen = (USHORT)RecDot; De->NameLen = 1;
        De->FileType = Vol->HasFiletype ? EXT_FT_DIR : 0; De->Name[0] = '.';
        De = (PEXT_DIR_ENTRY)(Buf.Data + RecDot);
        De->Inode = DirIno; De->RecLen = (USHORT)(End - RecDot); De->NameLen = 2;
        De->FileType = Vol->HasFiletype ? EXT_FT_DIR : 0; De->Name[0] = '.'; De->Name[1] = '.';
        ExtDirBlockCsum(Vol, Ino, &Inode, Buf.Data);
        ExtSetDirty(Vol, &Buf);
        ExtHostUnmap(Vol, &Buf);
    } else if (IsReg) {
        if (Vol->HasExtents) ExtInitExtents(Vol, &Inode);
    } else {
        /* device/fifo/socket: the device number goes in Block[0] (not used by NT) */
    }

    S = ExtWriteInode(Vol, Ino, &Inode);
    if (!NT_SUCCESS(S)) { ExtFreeInodeData(Vol, Ino, &Inode); ExtFreeInode(Vol, Ino, IsDir); return S; }

    S = ExtAddEntry(Vol, DirIno, Dir, Name, NameLen, Ino, ExtModeToFileType(Mode));
    if (!NT_SUCCESS(S)) {
        ExtFreeInodeData(Vol, Ino, &Inode);
        ExtFreeInode(Vol, Ino, IsDir);
        return S;
    }

    if (IsDir) {
        DirLinkInc(Vol, Dir);   /* the new ".." points back here */
        Dir->Mtime = Dir->Ctime = ExtHostNow();
        ExtWriteInode(Vol, DirIno, Dir);
    } else {
        Dir->Mtime = Dir->Ctime = ExtHostNow();
        ExtWriteInode(Vol, DirIno, Dir);
    }

    if (NewIno) *NewIno = Ino;
    if (NewInode) *NewInode = Inode;
    return EXT_OK;
}

EXT_STATUS
ExtLink(PEXT_VOL Vol, ULONG DirIno, PEXT_INODE Dir, const char *Name, ULONG NameLen,
        ULONG TargetIno, PEXT_INODE Target)
{
    EXT_ENTRY Tmp;
    EXT_STATUS S;

    if (Vol->ReadOnly) return EXT_READ_ONLY;
    if (Vol->OldExt) return EXT_NOT_SUPPORTED;
    if ((Target->Mode & EXT_S_IFMT) == EXT_S_IFDIR) return EXT_IS_A_DIRECTORY;
    if (Target->LinksCount >= EXT_LINK_MAX) return EXT_TOO_MANY_LINKS;
    S = ExtLookup(Vol, DirIno, Dir, Name, NameLen, &Tmp);
    if (NT_SUCCESS(S)) return EXT_EXISTS;
    if (S != EXT_NOT_FOUND) return S;

    S = ExtAddEntry(Vol, DirIno, Dir, Name, NameLen, TargetIno, ExtModeToFileType(Target->Mode));
    if (!NT_SUCCESS(S)) return S;
    Target->LinksCount++;
    Target->Ctime = ExtHostNow();
    return ExtWriteInode(Vol, TargetIno, Target);
}

EXT_STATUS
ExtUnlink(PEXT_VOL Vol, ULONG DirIno, PEXT_INODE Dir, const char *Name, ULONG NameLen)
{
    EXT_ENTRY E;
    EXT_INODE Inode;
    BOOLEAN IsDir;
    EXT_STATUS S;

    if (Vol->ReadOnly) return EXT_READ_ONLY;
    if (Vol->OldExt) return ExtOldUnlink(Vol, DirIno, Name, NameLen);
    S = ExtLookup(Vol, DirIno, Dir, Name, NameLen, &E);
    if (!NT_SUCCESS(S)) return S;
    S = ExtReadInode(Vol, E.Inode, &Inode);
    if (!NT_SUCCESS(S)) return S;
    IsDir = (BOOLEAN)((Inode.Mode & EXT_S_IFMT) == EXT_S_IFDIR);

    if (IsDir) {
        BOOLEAN Empty;
        S = ExtDirIsEmpty(Vol, E.Inode, &Inode, &Empty);
        if (!NT_SUCCESS(S)) return S;
        if (!Empty) return EXT_NOT_EMPTY;
    }

    S = ExtRemoveEntry(Vol, DirIno, Dir, Name, NameLen);
    if (!NT_SUCCESS(S)) return S;

    if (IsDir) {
        /* directory: drops its own "." link and the parent's ".." link */
        ExtFreeInodeData(Vol, E.Inode, &Inode);
        Inode.LinksCount = 0;
        Inode.Dtime = ExtHostNow();
        ExtInodeSetSize(Vol, &Inode, 0);
        ExtWriteInode(Vol, E.Inode, &Inode);
        ExtFreeInode(Vol, E.Inode, TRUE);
        DirLinkDec(Dir);        /* the gone ".." */
    } else {
        if (Inode.LinksCount > 0) Inode.LinksCount--;
        if (Inode.LinksCount == 0) {
            ExtFreeInodeData(Vol, E.Inode, &Inode);
            Inode.Dtime = ExtHostNow();
            ExtInodeSetSize(Vol, &Inode, 0);
            ExtWriteInode(Vol, E.Inode, &Inode);
            ExtFreeInode(Vol, E.Inode, FALSE);
        } else {
            Inode.Ctime = ExtHostNow();
            ExtWriteInode(Vol, E.Inode, &Inode);
        }
    }
    Dir->Mtime = Dir->Ctime = ExtHostNow();
    return ExtWriteInode(Vol, DirIno, Dir);
}

EXT_STATUS
ExtRename(PEXT_VOL Vol, ULONG OldDir, PEXT_INODE OldDirInode, const char *OldName, ULONG OldLen,
          ULONG NewDir, PEXT_INODE NewDirInode, const char *NewName, ULONG NewLen)
{
    EXT_ENTRY Src, Dst;
    EXT_INODE SrcInode;
    BOOLEAN SrcIsDir;
    BOOLEAN SameDir = (BOOLEAN)(OldDir == NewDir);
    EXT_STATUS S;

    if (Vol->ReadOnly) return EXT_READ_ONLY;
    if (Vol->OldExt) return EXT_NOT_SUPPORTED;

    S = ExtLookup(Vol, OldDir, OldDirInode, OldName, OldLen, &Src);
    if (!NT_SUCCESS(S)) return S;
    S = ExtReadInode(Vol, Src.Inode, &SrcInode);
    if (!NT_SUCCESS(S)) return S;
    SrcIsDir = (BOOLEAN)((SrcInode.Mode & EXT_S_IFMT) == EXT_S_IFDIR);
    if (SrcIsDir && !SameDir && !DirCanGainSubdir(Vol, NewDirInode)) return EXT_TOO_MANY_LINKS;

    /* If the target exists, it must be removable and compatible */
    S = ExtLookup(Vol, NewDir, NewDirInode, NewName, NewLen, &Dst);
    if (NT_SUCCESS(S)) {
        if (Dst.Inode == Src.Inode) return EXT_OK;
        {
            EXT_INODE DstInode;
            BOOLEAN DstIsDir;
            S = ExtReadInode(Vol, Dst.Inode, &DstInode);
            if (!NT_SUCCESS(S)) return S;
            DstIsDir = (BOOLEAN)((DstInode.Mode & EXT_S_IFMT) == EXT_S_IFDIR);
            if (DstIsDir) {
                BOOLEAN Empty;
                S = ExtDirIsEmpty(Vol, Dst.Inode, &DstInode, &Empty);
                if (!NT_SUCCESS(S)) return S;
                if (!Empty) return EXT_NOT_EMPTY;
            }
            S = ExtUnlink(Vol, NewDir, NewDirInode, NewName, NewLen);
            if (!NT_SUCCESS(S)) return S;
            if (SameDir) {
                /* NewDirInode may have changed (links/size); re-read */
                ExtReadInode(Vol, NewDir, NewDirInode);
                if (OldDir == NewDir) ExtReadInode(Vol, OldDir, OldDirInode);
            }
        }
    } else if (S != EXT_NOT_FOUND) {
        return S;
    }

    /* Add the new entry, then remove the old one */
    S = ExtAddEntry(Vol, NewDir, NewDirInode, NewName, NewLen, Src.Inode, ExtModeToFileType(SrcInode.Mode));
    if (!NT_SUCCESS(S)) return S;
    if (SameDir) ExtReadInode(Vol, OldDir, OldDirInode);

    S = ExtRemoveEntry(Vol, OldDir, OldDirInode, OldName, OldLen);
    if (!NT_SUCCESS(S)) return S;

    if (SrcIsDir && !SameDir) {
        /* the moved directory's ".." now names the new parent */
        ExtSetDotDot(Vol, Src.Inode, &SrcInode, NewDir);
        DirLinkDec(OldDirInode);
        DirLinkInc(Vol, NewDirInode);
    }

    OldDirInode->Mtime = OldDirInode->Ctime = ExtHostNow();
    ExtWriteInode(Vol, OldDir, OldDirInode);
    if (!SameDir) {
        NewDirInode->Mtime = NewDirInode->Ctime = ExtHostNow();
        ExtWriteInode(Vol, NewDir, NewDirInode);
    }
    SrcInode.Ctime = ExtHostNow();
    ExtWriteInode(Vol, Src.Inode, &SrcInode);
    return EXT_OK;
}

/* ------------------------------------------------------------------ */
/* File data                                                           */
/* ------------------------------------------------------------------ */

EXT_STATUS
ExtReadFile(PEXT_VOL Vol, ULONG Ino, PEXT_INODE Inode, ULONGLONG Offset, ULONG Length,
            PVOID Buffer, PULONG Read)
{
    PUCHAR Out = (PUCHAR)Buffer;
    ULONGLONG Size = ExtInodeSize(Vol, Inode);
    ULONG Done = 0;

    if (Inode->Flags & EXT_IF_ENCRYPT) return EXT_ACCESS_DENIED;
    if (Offset >= Size) { *Read = 0; return EXT_OK; }
    if (Offset + Length > Size) Length = (ULONG)(Size - Offset);

    /* inline data (i_block + system.data) and fast symlinks (i_block) */
    if ((Inode->Flags & EXT_IF_INLINE_DATA) || ExtIsFastSymlink(Vol, Inode)) {
        PUCHAR D = (PUCHAR)Inode->Block;
        ULONG Len = 60;
        if (Inode->Flags & EXT_IF_INLINE_DATA) {
            EXT_STATUS S = ExtInlineLoad(Vol, Ino, Inode, &D, &Len);
            if (!NT_SUCCESS(S)) return S;
        }
        if (Offset < Len) {
            Done = (ULONG)MIN_UL((ULONGLONG)Length, Len - Offset);
            ExtCopy(Out, D + Offset, Done);
        }
        if (Inode->Flags & EXT_IF_INLINE_DATA) ExtHostFree(D);
        *Read = Done;
        return EXT_OK;
    }

    while (Done < Length) {
        ULONGLONG FileBlk = (Offset + Done) >> Vol->BlockShift;
        ULONG InBlk = (ULONG)((Offset + Done) & (Vol->BlockSize - 1));
        ULONG Chunk = (ULONG)MIN_UL(Length - Done, Vol->BlockSize - InBlk);
        ULONGLONG Disk;
        ULONG Run;
        EXT_STATUS S = ExtMapFileBlock(Vol, Ino, Inode, FileBlk, &Disk, &Run);
        if (!NT_SUCCESS(S)) return S;
        if (Disk == 0) {
            ExtZero(Out + Done, Chunk);
        } else {
            EXT_BUF Buf;
            S = ExtMapBlocks(Vol, Disk, 1, EXT_MAP_READ | EXT_MAP_DATA, &Buf);
            if (!NT_SUCCESS(S)) return S;
            ExtCopy(Out + Done, Buf.Data + InBlk, Chunk);
            ExtHostUnmap(Vol, &Buf);
        }
        Done += Chunk;
    }
    *Read = Done;
    return EXT_OK;
}

EXT_STATUS
ExtWriteFile(PEXT_VOL Vol, ULONG Ino, PEXT_INODE Inode, ULONGLONG Offset, ULONG Length,
             const VOID *Buffer, PULONG Written)
{
    const UCHAR *In = (const UCHAR *)Buffer;
    ULONG Done = 0;
    EXT_STATUS S;

    if (Vol->ReadOnly) return EXT_READ_ONLY;
    if (Vol->OldExt) {
        S = ExtOldWriteData(Vol, Ino, Offset, (const UCHAR *)Buffer, Length);
        if (!NT_SUCCESS(S)) return S;
        if (Written) *Written = Length;
        ExtOldReadInode(Vol, Ino, Inode);
        return EXT_OK;
    }
    if (Inode->Flags & EXT_IF_INLINE_DATA) {
        S = ExtUninline(Vol, Ino, Inode);
        if (!NT_SUCCESS(S)) return S;
    }
    if (ExtIsFastSymlink(Vol, Inode) && ExtInodeSize(Vol, Inode) != 0) return EXT_NOT_SUPPORTED;

    while (Done < Length) {
        ULONGLONG FileBlk = (Offset + Done) >> Vol->BlockShift;
        ULONG InBlk = (ULONG)((Offset + Done) & (Vol->BlockSize - 1));
        ULONG Chunk = (ULONG)MIN_UL(Length - Done, Vol->BlockSize - InBlk);
        ULONGLONG Disk;
        EXT_BUF Buf;
        BOOLEAN Fresh;
        ULONG Mode;

        S = ExtGetFileBlock(Vol, Ino, Inode, FileBlk, &Disk, &Fresh);
        if (!NT_SUCCESS(S)) break;
        /* a whole-block write, or a new block (whose old contents must not
           leak into the file): start from zeros */
        Mode = ((InBlk == 0 && Chunk == Vol->BlockSize) || Fresh) ? EXT_MAP_NEW : EXT_MAP_WRITE;
        S = ExtMapBlocks(Vol, Disk, 1, Mode | EXT_MAP_DATA, &Buf);
        if (!NT_SUCCESS(S)) break;
        ExtCopy(Buf.Data + InBlk, In + Done, Chunk);
        ExtSetDirty(Vol, &Buf);
        ExtHostUnmap(Vol, &Buf);
        Done += Chunk;
    }

    if (Offset + Done > ExtInodeSize(Vol, Inode)) {
        ExtInodeSetSize(Vol, Inode, Offset + Done);
    }
    Inode->Mtime = Inode->Ctime = ExtHostNow();
    *Written = Done;
    return (Done == Length) ? EXT_OK : EXT_DISK_FULL;
}

EXT_STATUS
ExtSetFileSize(PEXT_VOL Vol, ULONG Ino, PEXT_INODE Inode, ULONGLONG NewSize)
{
    ULONGLONG Old = ExtInodeSize(Vol, Inode);
    ULONGLONG NewBlocks = (NewSize + Vol->BlockSize - 1) >> Vol->BlockShift;
    EXT_STATUS S;

    if (Vol->ReadOnly) return EXT_READ_ONLY;
    if (Vol->OldExt) {
        /* minimal: set the size; a grow is sparse (zero-filled on read), a
           shrink keeps trailing blocks (reclaimed only on unlink) */
        Inode->SizeLo = (ULONG)NewSize;
        return ExtOldWriteInode(Vol, Ino, Inode);
    }
    if (Inode->Flags & EXT_IF_INLINE_DATA) {
        S = ExtUninline(Vol, Ino, Inode);
        if (!NT_SUCCESS(S)) return S;
    }
    if ((Inode->Mode & EXT_S_IFMT) == EXT_S_IFLNK) return EXT_NOT_SUPPORTED;
    if (NewSize < Old) {
        S = ExtTruncateBlocks(Vol, Ino, Inode, NewBlocks);
        if (!NT_SUCCESS(S)) return S;
    }
    /* Bytes past EOF inside the last block must read as zero once the file
       grows over them again: clear them now (from the smaller of the sizes) */
    {
        ULONGLONG Edge = NewSize < Old ? NewSize : Old;
        ULONG InBlk = (ULONG)(Edge & (Vol->BlockSize - 1));
        if (InBlk) {
            ULONGLONG Disk; ULONG Run;
            S = ExtMapFileBlock(Vol, Ino, Inode, Edge >> Vol->BlockShift, &Disk, &Run);
            if (NT_SUCCESS(S) && Disk) {
                EXT_BUF Buf;
                S = ExtMapBlocks(Vol, Disk, 1, EXT_MAP_WRITE, &Buf);
                if (NT_SUCCESS(S)) {
                    ExtZero(Buf.Data + InBlk, Vol->BlockSize - InBlk);
                    ExtSetDirty(Vol, &Buf);
                    ExtHostUnmap(Vol, &Buf);
                }
            }
        }
    }
    ExtInodeSetSize(Vol, Inode, NewSize);
    Inode->Mtime = Inode->Ctime = ExtHostNow();
    return ExtWriteInode(Vol, Ino, Inode);
}

/* ------------------------------------------------------------------ */
/* Orphans                                                             */
/* ------------------------------------------------------------------ */

/* Finishes what a crashed Linux left on the orphan list (s_last_orphan,
   chained through i_dtime): inodes with no links are deleted, the others
   truncated to i_size. As ext4_orphan_cleanup does at mount. */
EXT_STATUS
ExtProcessOrphans(PEXT_VOL Vol)
{
    ULONG Ino, Guard = 0;
    if (Vol->ReadOnly || Vol->Super->LastOrphan == 0) return EXT_OK;
    Ino = Vol->Super->LastOrphan;
    while (Ino && Guard++ < Vol->InodesCount) {
        EXT_INODE I;
        ULONG Next;
        BOOLEAN IsDir;
        if (Ino < Vol->FirstIno || Ino > Vol->InodesCount) break;
        if (!NT_SUCCESS(ExtReadInode(Vol, Ino, &I))) break;
        Next = I.Dtime;
        IsDir = (BOOLEAN)((I.Mode & EXT_S_IFMT) == EXT_S_IFDIR);
        if (I.LinksCount == 0) {
            ExtFreeInodeData(Vol, Ino, &I);
            I.Dtime = ExtHostNow();
            ExtInodeSetSize(Vol, &I, 0);
            ExtWriteInode(Vol, Ino, &I);
            ExtFreeInode(Vol, Ino, IsDir);
        } else {
            I.Dtime = 0;
            if (!(I.Flags & EXT_IF_INLINE_DATA) && !ExtIsFastSymlink(Vol, &I))
                ExtTruncateBlocks(Vol, Ino, &I, (ExtInodeSize(Vol, &I) + Vol->BlockSize - 1) >> Vol->BlockShift);
            ExtWriteInode(Vol, Ino, &I);
        }
        Ino = Next;
    }
    Vol->Super->LastOrphan = 0;
    return ExtFlushSuper(Vol);
}

/* ------------------------------------------------------------------ */
/* For the formatter: the root directory and reserved inodes           */
/* ------------------------------------------------------------------ */

/* A fresh inode Ino (a reserved one: no bitmap change) with Mode */
EXT_STATUS
ExtNewInodeAt(PEXT_VOL Vol, ULONG Ino, USHORT Mode, PEXT_INODE Inode)
{
    EXT_STATUS S = ExtZeroInodeImage(Vol, Ino);
    if (!NT_SUCCESS(S)) return S;
    ExtInitInode(Vol, Inode, Mode);
    if (Vol->HasExtents && (Mode & EXT_S_IFMT) != EXT_S_IFLNK) ExtInitExtents(Vol, Inode);
    return EXT_OK;
}

/* The root directory (inode 2, already marked in use): "." and ".." */
EXT_STATUS
ExtMakeRoot(PEXT_VOL Vol)
{
    EXT_INODE Inode;
    ULONGLONG Disk;
    EXT_BUF Buf;
    PEXT_DIR_ENTRY De;
    ULONG End = Vol->HasMetaCsum ? Vol->BlockSize - (ULONG)sizeof(EXT_DIR_TAIL) : Vol->BlockSize;
    ULONG RecDot = EXT_DIR_REC_LEN(1);
    EXT_STATUS S;

    S = ExtNewInodeAt(Vol, EXT_ROOT_INO, (USHORT)(EXT_S_IFDIR | 0755), &Inode);
    if (!NT_SUCCESS(S)) return S;
    Inode.LinksCount = 2;
    S = ExtGetFileBlock(Vol, EXT_ROOT_INO, &Inode, 0, &Disk, NULL);
    if (!NT_SUCCESS(S)) return S;
    ExtInodeSetSize(Vol, &Inode, Vol->BlockSize);
    S = ExtMapBlocks(Vol, Disk, 1, EXT_MAP_NEW, &Buf);
    if (!NT_SUCCESS(S)) return S;
    De = (PEXT_DIR_ENTRY)Buf.Data;
    De->Inode = EXT_ROOT_INO; De->RecLen = (USHORT)RecDot; De->NameLen = 1;
    De->FileType = Vol->HasFiletype ? EXT_FT_DIR : 0; De->Name[0] = '.';
    De = (PEXT_DIR_ENTRY)(Buf.Data + RecDot);
    De->Inode = EXT_ROOT_INO; De->RecLen = (USHORT)(End - RecDot); De->NameLen = 2;
    De->FileType = Vol->HasFiletype ? EXT_FT_DIR : 0; De->Name[0] = '.'; De->Name[1] = '.';
    ExtDirBlockCsum(Vol, EXT_ROOT_INO, &Inode, Buf.Data);
    ExtSetDirty(Vol, &Buf);
    ExtHostUnmap(Vol, &Buf);
    return ExtWriteInode(Vol, EXT_ROOT_INO, &Inode);
}

/* One more (empty) block at the end of directory Ino */
EXT_STATUS
ExtGrowDirectory(PEXT_VOL Vol, ULONG Ino, PEXT_INODE Inode)
{
    ULONGLONG Blk = ExtInodeSize(Vol, Inode) >> Vol->BlockShift;
    ULONGLONG Disk;
    EXT_BUF Buf;
    PEXT_DIR_ENTRY De;
    ULONG End = Vol->HasMetaCsum ? Vol->BlockSize - (ULONG)sizeof(EXT_DIR_TAIL) : Vol->BlockSize;
    EXT_STATUS S;

    S = ExtGetFileBlock(Vol, Ino, Inode, Blk, &Disk, NULL);
    if (!NT_SUCCESS(S)) return S;
    ExtInodeSetSize(Vol, Inode, (Blk + 1) << Vol->BlockShift);
    S = ExtMapBlocks(Vol, Disk, 1, EXT_MAP_NEW, &Buf);
    if (!NT_SUCCESS(S)) return S;
    De = (PEXT_DIR_ENTRY)Buf.Data;
    De->Inode = 0;
    De->RecLen = (USHORT)(End >= 65536 ? 65535 : End);     /* 64 KB blocks: the special value */
    ExtDirBlockCsum(Vol, Ino, Inode, Buf.Data);
    ExtSetDirty(Vol, &Buf);
    ExtHostUnmap(Vol, &Buf);
    return ExtWriteInode(Vol, Ino, Inode);
}
