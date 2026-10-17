/*
 * EXTNT - boot loader for Windows NT on an ext volume (sectors 0-1, 1 KB:
 * the space ext leaves for a boot loader before its superblock)
 *
 * The volume carries a FAT16 view of the files the NT loader needs (see
 * fmt/extshim.c): a FAT and a root directory in the blocks of the ext boot
 * loader inode, and FAT clusters that are the ext blocks themselves. This
 * code reads that view like any FAT16 boot sector: it finds NTLDR in the
 * root directory, follows its cluster chain, loads it at 2000:0000 and
 * starts it the way the Windows NT boot sectors do:
 *
 *      DL = boot drive, DS:SI -> the BPB (07C0:000B), ES = 03E8h
 *
 * NTLDR then reads the same view with its own FAT code.
 *
 * Disk access uses the INT 13h extensions when the BIOS has them, else CHS
 * through the BPB geometry.
 *
 * Built with GNU as:  as --32 -o extboot.o extboot.S
 *                     ld -m elf_i386 -Ttext 0x7C00 --oformat binary -o extboot.bin extboot.o
 * extboot fills in the BPB (bytes 3-61).
 */

        .code16
        .intel_syntax noprefix
        .text
        .globl  _start

/* BPB fields (in the boot sector at 7C00h) */
#define BPB_BPS         0x7C0B
#define BPB_SPC         0x7C0D
#define BPB_RESERVED    0x7C0E
#define BPB_FATS        0x7C10
#define BPB_ROOTENT     0x7C11
#define BPB_SPF         0x7C16
#define BPB_SPT         0x7C18
#define BPB_HEADS       0x7C1A
#define BPB_HIDDEN      0x7C1C
#define BPB_DRIVE       0x7C24

/* Work areas (0000:xxxx) */
#define FAT_BUF         0x0600      /* one FAT sector */
#define ROOT_BUF        0x1000      /* the root directory (up to 16 KB) */
#define LOAD_SEG        0x2000      /* NTLDR goes to 2000:0000 */
#define LOAD_LIMIT      0x9000      /* and must end below 9000:0000 */

_start:
        jmp     boot
        nop

        /* The BPB: written by extboot */
        .fill   0x3E - 3, 1, 0

boot:
        cli
        xor     ax, ax
        mov     ss, ax
        mov     sp, 0x7C00
        mov     ds, ax
        mov     es, ax
        sti
        cld
        mov     [BPB_DRIVE], dl

        /* INT 13h extensions? */
        mov     byte ptr [use_ext], 0
        mov     ah, 0x41
        mov     bx, 0x55AA
        int     0x13
        jc      1f
        cmp     bx, 0xAA55
        jne     1f
        test    cl, 1
        jz      1f
        mov     byte ptr [use_ext], 1
1:
        /* Sector 1 to 0000:7E00 */
        mov     eax, 1
        mov     cx, 1
        mov     bx, 0x7E00
        call    read_sectors
        jmp     stage2

/*
 * read_sectors: EAX = sector (relative to the volume), CX = count,
 * ES:BX = buffer (no 64 KB boundary crossed). One sector at a time
 * through CHS, up to 64 at a time through the extensions.
 * Preserves all registers.
 */
read_sectors:
        push    es
        pushad
2:
        push    eax
        push    cx
        add     eax, [BPB_HIDDEN]
        cmp     byte ptr [use_ext], 0
        je      3f
        cmp     cx, 64
        jbe     4f
        mov     cx, 64
4:
        mov     si, offset dap
        mov     word ptr [si + 2], cx
        mov     word ptr [si + 4], bx
        mov     word ptr [si + 6], es
        mov     dword ptr [si + 8], eax
        mov     dword ptr [si + 12], 0
        mov     dl, [BPB_DRIVE]
        mov     ah, 0x42
        push    cx
        int     0x13
        pop     cx
        jc      disk_error
        jmp     5f
3:      /* CHS: one sector */
        xor     edx, edx
        movzx   ecx, word ptr [BPB_SPT]
        div     ecx
        inc     dx
        mov     cl, dl
        xor     edx, edx
        movzx   esi, word ptr [BPB_HEADS]
        div     esi
        cmp     eax, 1023
        ja      disk_error
        mov     dh, dl
        mov     ch, al
        shl     ah, 6
        or      cl, ah
        mov     dl, [BPB_DRIVE]
        mov     di, 5
6:
        mov     ax, 0x0201
        push    di
        int     0x13
        pop     di
        jnc     7f
        xor     ah, ah
        int     0x13
        dec     di
        jnz     6b
        jmp     disk_error
7:
        mov     cx, 1
5:
        mov     ax, cx
        shl     ax, 5                       /* paragraphs */
        mov     dx, es
        add     dx, ax
        mov     es, dx
        pop     dx
        pop     eax
        movzx   ecx, cx
        add     eax, ecx
        sub     dx, cx
        mov     cx, dx
        jnz     2b
        popad
        pop     es
        ret

disk_error:
        mov     si, offset msg_disk
fatal:
        call    print
        mov     si, offset msg_restart
        call    print
        xor     ax, ax
        int     0x16
        int     0x19

print:
        lodsb
        or      al, al
        jz      8f
        mov     ah, 0x0E
        mov     bx, 7
        int     0x10
        jmp     print
8:      ret

use_ext:        .byte   0
dap:            .byte   16, 0
                .word   0, 0, 0
                .long   0, 0
msg_disk:       .asciz  "\r\nEXTNT boot: disk read error"
msg_restart:    .asciz  "\r\nPress a key to restart\r\n"

        .org    510
        .word   0xAA55

/* ================================================================== */
/* Sector 1 (0000:7E00)                                               */
/* ================================================================== */

stage2:
        /* root directory: after the reserved sectors and the FATs */
        movzx   eax, byte ptr [BPB_FATS]
        movzx   ecx, word ptr [BPB_SPF]
        mul     ecx
        movzx   ecx, word ptr [BPB_RESERVED]
        add     eax, ecx
        mov     [fat_start], ecx
        movzx   ecx, word ptr [BPB_ROOTENT]
        add     cx, 15
        shr     cx, 4                       /* sectors */
        cmp     cx, 32
        ja      bad_fs
        mov     bx, ROOT_BUF
        call    read_sectors
        add     eax, ecx
        mov     [data_start], eax

        /* NTLDR in the root */
        mov     si, ROOT_BUF
        mov     dx, [BPB_ROOTENT]
9:
        cmp     byte ptr [si], 0
        je      not_found
        test    byte ptr [si + 11], 0x18    /* not a directory or label */
        jnz     10f
        push    si
        mov     di, offset ntldr_name
        mov     cx, 11
        repe    cmpsb
        pop     si
        je      found
10:
        add     si, 32
        dec     dx
        jnz     9b
not_found:
        mov     si, offset msg_missing
        jmp     fatal
bad_fs:
        mov     si, offset msg_badfs
        jmp     fatal

found:
        mov     eax, [si + 28]              /* size */
        add     eax, 511
        shr     eax, 9
        mov     [remaining], eax
        movzx   eax, word ptr [si + 26]     /* first cluster */
        mov     word ptr [load_seg], LOAD_SEG
        mov     word ptr [fat_cached], 0xFFFF

next_cluster:
        cmp     dword ptr [remaining], 0
        je      start_ntldr
        cmp     ax, 2
        jb      bad_fs
        cmp     ax, 0xFFF0
        jae     bad_fs
        /* sector = data_start + (cluster - 2) * spc */
        push    eax
        sub     eax, 2
        movzx   ecx, byte ptr [BPB_SPC]
        mul     ecx
        add     eax, [data_start]
        cmp     ecx, [remaining]
        jbe     11f
        mov     ecx, [remaining]
11:
        sub     [remaining], ecx
        mov     dx, [load_seg]
        cmp     dx, LOAD_LIMIT
        jae     too_big
        mov     es, dx
        xor     bx, bx
        call    read_sectors
        shl     cx, 5
        add     dx, cx
        mov     [load_seg], dx
        xor     dx, dx
        mov     es, dx
        pop     eax

        /* the next cluster from the FAT (2 bytes an entry) */
        movzx   ebx, ax
        shr     ebx, 8                      /* FAT sector holding the entry */
        and     ax, 0xFF                    /* entry within it */
        cmp     bx, [fat_cached]
        je      12f
        mov     [fat_cached], bx
        push    eax
        mov     eax, ebx
        add     eax, [fat_start]
        mov     cx, 1
        mov     bx, FAT_BUF
        call    read_sectors
        pop     eax
12:
        movzx   si, al
        shl     si, 1
        mov     ax, [FAT_BUF + si]
        movzx   eax, ax
        cmp     ax, 0xFFF8
        jb      next_cluster
        cmp     dword ptr [remaining], 0
        jne     bad_fs

start_ntldr:
        mov     dl, [BPB_DRIVE]
        mov     ax, 0x03E8
        mov     es, ax
        mov     ax, 0x07C0
        mov     ds, ax
        mov     si, 0x000B
        push    LOAD_SEG
        push    0
        retf

too_big:
        mov     si, offset msg_big
        jmp     fatal

fat_start:      .long   0
data_start:     .long   0
remaining:      .long   0
load_seg:       .word   0
fat_cached:     .word   0
ntldr_name:     .ascii  "NTLDR      "
msg_missing:    .asciz  "\r\nEXTNT boot: NTLDR is missing"
msg_badfs:      .asciz  "\r\nEXTNT boot: the boot files are damaged (run extboot)"
msg_big:        .asciz  "\r\nEXTNT boot: NTLDR is too large"

        /* the Windows directory (extboot writes it; EXTNT reads it to keep
           the view current) */
        .org    0x3F0
windir:         .fill   16, 1, 0
