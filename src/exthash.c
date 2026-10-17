/*
 * EXTNT - the ext2/3/4 directory hash (htree), ported from e2fsprogs
 * lib/ext2fs/dirhash.c. Produces the (major, minor) hash of a name used to
 * place and find it in a hashed directory.
 */

#ifdef EXT_KERNEL
#include "ext.h"
#else
#include "extusr.h"
#include "extsup.h"
#endif

#define DELTA 0x9E3779B9

static VOID
TeaTransform(ULONG buf[4], ULONG const in[])
{
    ULONG sum = 0;
    ULONG b0 = buf[0], b1 = buf[1];
    ULONG a = in[0], b = in[1], c = in[2], d = in[3];
    int n = 16;

    do {
        sum += DELTA;
        b0 += ((b1 << 4) + a) ^ (b1 + sum) ^ ((b1 >> 5) + b);
        b1 += ((b0 << 4) + c) ^ (b0 + sum) ^ ((b0 >> 5) + d);
    } while (--n);

    buf[0] += b0;
    buf[1] += b1;
}

/* F, G and H are the MD4 functions */
#define F(x, y, z) ((z) ^ ((x) & ((y) ^ (z))))
#define G(x, y, z) (((x) & (y)) + (((x) ^ (y)) & (z)))
#define H(x, y, z) ((x) ^ (y) ^ (z))
#define ROTATE_LEFT(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

static VOID
HalfMd4Transform(ULONG buf[4], ULONG const in[])
{
    ULONG a = buf[0], b = buf[1], c = buf[2], d = buf[3];

#define RND1(a, b, c, d, k, s) (a += F(b, c, d) + in[k], a = ROTATE_LEFT(a, s))
    RND1(a, b, c, d, 0, 3); RND1(d, a, b, c, 1, 7); RND1(c, d, a, b, 2, 11); RND1(b, c, d, a, 3, 19);
    RND1(a, b, c, d, 4, 3); RND1(d, a, b, c, 5, 7); RND1(c, d, a, b, 6, 11); RND1(b, c, d, a, 7, 19);

#define RND2(a, b, c, d, k, s) (a += G(b, c, d) + in[k] + 0x5A827999, a = ROTATE_LEFT(a, s))
    RND2(a, b, c, d, 1, 3); RND2(d, a, b, c, 3, 5); RND2(c, d, a, b, 5, 9); RND2(b, c, d, a, 7, 13);
    RND2(a, b, c, d, 0, 3); RND2(d, a, b, c, 2, 5); RND2(c, d, a, b, 4, 9); RND2(b, c, d, a, 6, 13);

#define RND3(a, b, c, d, k, s) (a += H(b, c, d) + in[k] + 0x6ED9EBA1, a = ROTATE_LEFT(a, s))
    RND3(a, b, c, d, 3, 3); RND3(d, a, b, c, 7, 9); RND3(c, d, a, b, 2, 11); RND3(b, c, d, a, 6, 15);
    RND3(a, b, c, d, 1, 3); RND3(d, a, b, c, 5, 9); RND3(c, d, a, b, 0, 11); RND3(b, c, d, a, 4, 15);

    buf[0] += a; buf[1] += b; buf[2] += c; buf[3] += d;
}

static ULONG
Str2HashBuf(const char *msg, int len, ULONG *buf, int num, int sig)
{
    ULONG pad, val;
    int i;
    const UCHAR *um = (const UCHAR *)msg;
    const signed char *sm = (const signed char *)msg;

    pad = (ULONG)len | ((ULONG)len << 8);
    pad |= pad << 16;

    val = pad;
    if (len > num * 4) len = num * 4;
    for (i = 0; i < len; i++) {
        val = (sig ? (ULONG)sm[i] : (ULONG)um[i]) + (val << 8);
        if ((i % 4) == 3) {
            *buf++ = val;
            val = pad;
            num--;
        }
    }
    if (--num >= 0) *buf++ = val;
    while (--num >= 0) *buf++ = pad;
    return 0;
}

ULONG
ExtNameHash(PEXT_VOL Vol, UCHAR Version, const char *Name, ULONG Len, PULONG MinorHash)
{
    ULONG hash = 0, minor = 0;
    ULONG buf[4];
    const char *p;
    int i;
    ULONG in[8];
    int sig = 1;        /* signed by default (legacy_unsigned / md4_unsigned flip) */
    ULONG Major;

    /* The seed (s_hash_seed), or the default initial values */
    if (Vol->HashSeed[0] || Vol->HashSeed[1] || Vol->HashSeed[2] || Vol->HashSeed[3]) {
        buf[0] = Vol->HashSeed[0]; buf[1] = Vol->HashSeed[1];
        buf[2] = Vol->HashSeed[2]; buf[3] = Vol->HashSeed[3];
    } else {
        buf[0] = 0x67452301; buf[1] = 0xEFCDAB89; buf[2] = 0x98BADCFE; buf[3] = 0x10325476;
    }

    switch (Version) {
    case EXT_DX_HASH_LEGACY_UNSIGNED: sig = 0; /* fall through */
    case EXT_DX_HASH_LEGACY:
        {
            /* dx_hack_hash */
            ULONG hash0 = 0x12A3FE2D, hash1 = 0x37ABE8F9, h;
            p = Name;
            for (i = 0; i < (int)Len; i++) {
                ULONG ch = sig ? (ULONG)(LONG)(signed char)*p : (ULONG)(UCHAR)*p;
                h = hash1 + (hash0 ^ (ch * 7152373));
                if (h & 0x80000000) h -= 0x7FFFFFFF;
                hash1 = hash0; hash0 = h; p++;
            }
            Major = hash0 << 1;
            minor = 0;
            goto out;
        }
    case EXT_DX_HASH_HALF_MD4_UNSIGNED: sig = 0; /* fall through */
    case EXT_DX_HASH_HALF_MD4:
        p = Name;
        {
            int l = (int)Len;
            while (l > 0) {
                Str2HashBuf(p, l, in, 8, sig);
                HalfMd4Transform(buf, in);
                l -= 32; p += 32;
            }
        }
        Major = buf[1];
        minor = buf[2];
        goto out;
    case EXT_DX_HASH_TEA_UNSIGNED: sig = 0; /* fall through */
    case EXT_DX_HASH_TEA:
    default:
        p = Name;
        {
            int l = (int)Len;
            while (l > 0) {
                Str2HashBuf(p, l, in, 4, sig);
                TeaTransform(buf, in);
                l -= 16; p += 16;
            }
        }
        Major = buf[0];
        minor = buf[1];
        goto out;
    }

out:
    hash = Major & ~1U;     /* bit 0 reserved */
    if (hash == 0xFFFFFFFEUL) hash = 0xFFFFFFFCUL;     /* (EOF_32BIT - 1) << 1 */
    if (MinorHash) *MinorHash = minor;
    return hash;
}
