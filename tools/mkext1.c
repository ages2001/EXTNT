/*
 * mkext1 - create an original-ext ("extfs", Rémy Card, 1992) volume.
 *
 * No mkfs for the pre-ext2 format survives, so EXTNT ships its own: it is the
 * only way to produce a volume for the old-ext read AND write paths, and it
 * defines the exact on-disk contract those paths honour.  1 KB blocks.
 *
 * Layout:  block 0 boot | block 1 superblock (@1024) | blocks 2..FirstData-1
 * inode table (64-byte inodes) | FirstData.. data.  Free data blocks are a
 * singly linked list threaded through the blocks themselves (each free block's
 * first u32 is the next free block, 0 = end); the head is superblock field
 * s_firstfreeblock (@8).  Free inodes are found by scanning the table for a
 * zero (mode==0, links==0) slot.  Directory entries: inode(u32), rec_len(u16),
 * name_len(u16), name[]; the stored inode is the 1-based disk inode number.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define BS        1024u
#define OLD_MAGIC 0x137D
#define INO_SZ    64u
#define S_IFDIR   0x4000u

static unsigned char *img;
static unsigned long  nblocks;

static void put16(unsigned char *p, unsigned v){ p[0]=v&0xff; p[1]=(v>>8)&0xff; }
static void put32(unsigned char *p, unsigned long v){ p[0]=v&0xff; p[1]=(v>>8)&0xff; p[2]=(v>>16)&0xff; p[3]=(v>>24)&0xff; }
static unsigned char *blk(unsigned long b){ return img + (size_t)b*BS; }

int main(int argc, char **argv)
{
    unsigned long sizeMB, ninodes, itb, firstdata, b;
    unsigned char *sb, *rootino, *rootdir;
    FILE *f;

    if (argc < 3) { fprintf(stderr, "usage: mkext1 IMG SIZE_MB\n"); return 2; }
    sizeMB = strtoul(argv[2], NULL, 10);
    if (sizeMB < 1) sizeMB = 1;
    nblocks = sizeMB * 1024;                 /* 1 KB blocks */

    img = (unsigned char *)calloc(nblocks, BS);
    if (!img) { fprintf(stderr, "out of memory\n"); return 1; }

    ninodes = nblocks / 4; if (ninodes < 32) ninodes = 32;
    itb = (ninodes * INO_SZ + BS - 1) / BS;
    firstdata = 2 + itb;                     /* boot + sb + inode table */

    /* ---- superblock at byte 1024 (block 1) ---- */
    sb = blk(1);
    put32(sb + 0,  ninodes);                 /* s_ninodes */
    put32(sb + 4,  nblocks);                 /* s_nzones  */
    put32(sb + 8,  firstdata + 1);           /* s_firstfreeblock (head of list) */
    put32(sb + 12, (nblocks - firstdata - 1));/* s_freeblockscount (minus root dir blk) */
    put32(sb + 16, 0);                       /* s_firstfreeinode (unused: scan) */
    put32(sb + 20, ninodes - 1);             /* s_freeinodescount (root uses one) */
    put32(sb + 24, firstdata);               /* s_firstdatazone */
    put32(sb + 28, 0);                       /* s_logzonesize -> 1 KB */
    put32(sb + 32, 0x7fffffff);              /* s_maxsize */
    put16(sb + 56, OLD_MAGIC);               /* s_magic */

    /* ---- root inode = disk inode 1, table slot 0 (block 2, offset 0) ---- */
    rootino = blk(2);
    put16(rootino + 0,  S_IFDIR | 0755);     /* mode */
    put16(rootino + 2,  0);                  /* uid */
    put32(rootino + 4,  BS);                 /* size = one directory block */
    put32(rootino + 8,  0);                  /* time */
    put16(rootino + 12, 0);                  /* gid */
    put16(rootino + 14, 2);                  /* links (., ..) */
    put32(rootino + 16, firstdata);          /* i_block[0] -> the root dir block */

    /* ---- root directory block ---- */
    rootdir = blk(firstdata);
    /* "." : disk inode 1, rec_len 12 */
    put32(rootdir + 0, 1); put16(rootdir + 4, 12); put16(rootdir + 6, 1); rootdir[8] = '.';
    /* ".." : disk inode 1, rec_len fills the rest of the block */
    put32(rootdir + 12, 1); put16(rootdir + 16, (unsigned)(BS - 12)); put16(rootdir + 18, 2);
    rootdir[20] = '.'; rootdir[21] = '.';

    /* ---- thread every remaining data block onto the free list ---- */
    for (b = firstdata + 1; b < nblocks; b++)
        put32(blk(b), (b + 1 < nblocks) ? (b + 1) : 0);

    f = fopen(argv[1], "wb");
    if (!f) { fprintf(stderr, "cannot create %s\n", argv[1]); return 1; }
    if (fwrite(img, BS, nblocks, f) != nblocks) { fprintf(stderr, "write failed\n"); fclose(f); return 1; }
    fclose(f);
    free(img);
    printf("mkext1: %lu blocks, %lu inodes, inode table %lu blocks, first data %lu\n",
           nblocks, ninodes, itb, firstdata);
    return 0;
}
