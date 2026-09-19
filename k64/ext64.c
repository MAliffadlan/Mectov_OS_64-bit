/* F2a read-only ext2: superblock + group descriptors to find an inode,
 * direct / single- / double-indirect block maps (triple refused EFBIG),
 * hole-aware reads, per-call path walks. On-disk layout follows ext2 the
 * way the 32-bit ext2.c does; the 64-bit differences: ahci64 sector API,
 * no kmalloc (three static 4KB buffers under one lock — block_size is
 * always <= 4096), no VFS yet (single volume, F2b).
 *
 * Incompatible-feature policy like the hybrid ext64.c: unknown incompat
 * bits mean refused mount, never guessing.
 */
#include "cpu64.h"
#include "spin64.h"

#define EX_SUPER_OFF 1024
#define EX_SUPER_MAGIC 0xEF53
#define EX_ROOT_INO 2
#define EX_DIRECT 12
#define EX_MAX_BLOCK 4096

#define EX_INCOMPAT_FILETYPE 0x2
#define EX_INCOMPAT_RECOVER 0x8
#define EX_INCOMPAT_FLEX_BG 0x20
#define EX_INCOMPAT_EA_INODE 0x200
#define EX_INCOMPAT_OK                                                       \
    (EX_INCOMPAT_FILETYPE | EX_INCOMPAT_RECOVER | EX_INCOMPAT_FLEX_BG |       \
     EX_INCOMPAT_EA_INODE)

#define EX_ENOENT -2
#define EX_EIO -5
#define EX_ENOTDIR -20
#define EX_EISDIR -21
#define EX_EINVAL -22
#define EX_EFBIG -27

typedef struct {
    u16 mode;
    u32 size;
    u32 blocks; /* 512B sectors */
    u32 block[15]; /* 12 direct + single + double + triple */
} ex_inode_t;

static spin64_t ex_lock = SPIN64_INIT;
static int ex_vol_mounted = 0;
static int ex_drive = 0;
static u32 ex_block_size = 0;
static u32 ex_inodes = 0, ex_blocks = 0, ex_first_block = 0;
static u32 ex_bpg = 0, ex_ipg = 0, ex_inode_size = 0;

/* Static buffers (block_size <= 4096 always): B_DATA bulk, B_IND indirect
 * tables, B_META inode/GDT/superblock/bitmap. B_META2 is is_meta's private
 * scratch: it runs INSIDE alloc/free while the caller holds live data in
 * B_META/B_IND, so sharing would corrupt it (seen live: indirect table
 * overwritten with GDT content). One lock, no allocs. */
static u8 ex_bdata[EX_MAX_BLOCK];
static u8 ex_bind[EX_MAX_BLOCK];
static u8 ex_bmeta[EX_MAX_BLOCK];
static u8 ex_bmeta2[EX_MAX_BLOCK];

static u32 ex_rd32(const u8 *p, u32 off) {
    return (u32)p[off] | ((u32)p[off + 1] << 8) | ((u32)p[off + 2] << 16) |
           ((u32)p[off + 3] << 24);
}

static u16 ex_rd16(const u8 *p, u32 off) {
    return (u16)(p[off] | (p[off + 1] << 8));
}

static void ex_memcpy(void *d, const void *s, u64 n) {
    volatile u8 *p = (volatile u8 *)d;
    const volatile u8 *q = (const volatile u8 *)s;
    for (u64 i = 0; i < n; i++) p[i] = q[i];
}

static void ex_memzero(void *d, u64 n) {
    volatile u8 *p = (volatile u8 *)d;
    for (u64 i = 0; i < n; i++) p[i] = 0;
}

static int ex_streq(const char *a, const char *b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/* Read FS block blk into dst (4096-capable buffer). */
static int ex_block(u32 blk, void *dst) {
    u64 byte = (u64)blk * ex_block_size;
    u32 lba = (u32)(byte / 512);
    u32 nsec = ex_block_size / 512;
    u32 got = 0;
    while (got < nsec) {
        u32 boundary = 128 - ((lba + got) & 127);
        u32 n = nsec - got;
        if (n > boundary) n = boundary;
        if (ahci64_read(ex_drive, lba + got, (int)n,
                        (u8 *)dst + got * 512))
            return EX_EIO;
        got += n;
    }
    return 0;
}

static int ex_inode(u32 ino, ex_inode_t *out) {
    u32 per_group, grp, idx, gd_bytes, gd_blk, gd_off, itable;
    u32 ino_bytes, ib, io;
    const u8 *p;
    int i;
    if (!ino || ino > ex_inodes) return EX_ENOENT;
    per_group = ex_ipg;
    grp = (ino - 1) / per_group;
    idx = (ino - 1) % per_group;
    gd_bytes = (ex_first_block + 1) * ex_block_size + grp * 32;
    gd_blk = gd_bytes / ex_block_size;
    gd_off = gd_bytes % ex_block_size;
    if (ex_block(gd_blk, ex_bmeta)) return EX_EIO;
    itable = ex_rd32(ex_bmeta, gd_off + 8);
    if (!itable) return EX_EIO;
    ino_bytes = idx * ex_inode_size;
    ib = itable + ino_bytes / ex_block_size;
    io = ino_bytes % ex_block_size;
    if (io + 128 > ex_block_size) return EX_EIO;
    if (ex_block(ib, ex_bmeta)) return EX_EIO;
    p = ex_bmeta + io;
    out->mode = ex_rd16(p, 0);
    out->size = ex_rd32(p, 4);
    out->blocks = ex_rd32(p, 28);
    for (i = 0; i < 15; i++) out->block[i] = ex_rd32(p, 40 + i * 4);
    return 0;
}

/* Entry idx of an indirect block; 0 = hole. Uses B_IND (see note above). */
static u32 ex_ind(u32 blk, u32 idx) {
    u32 per = ex_block_size / 4;
    if (!blk || idx >= per) return 0;
    if (ex_block(blk, ex_bind)) return 0;
    return ex_rd32(ex_bind, idx * 4);
}

/* Logical -> physical block. 0 = hole/unimplemented. */
static u32 ex_bmap(const ex_inode_t *in, u32 lb, int *unsupported) {
    u32 per = ex_block_size / 4;
    if (lb < EX_DIRECT) return in->block[lb];
    lb -= EX_DIRECT;
    if (lb < per) return ex_ind(in->block[12], lb);
    lb -= per;
    if (lb < per * per) {
        u32 first = ex_ind(in->block[13], lb / per);
        return ex_ind(first, lb % per);
    }
    if (unsupported) *unsupported = 1;
    return 0;
}

/* Hole-aware read; buf is kernel (selftest) or caller-mapped (syscall). */
static long ex_data_read(const ex_inode_t *in, u64 off, void *buf, u32 len,
                         int *unsupported) {
    u8 *b = (u8 *)buf;
    long done = 0;
    if (off >= in->size) return 0;
    if (off + len > in->size) len = (u32)(in->size - off);
    while ((u32)done < len) {
        u64 pos = off + (u64)done;
        u32 lb = (u32)(pos / ex_block_size);
        u32 inb = (u32)(pos % ex_block_size);
        u32 take = ex_block_size - inb;
        u32 pb;
        if (take > len - (u32)done) take = len - (u32)done;
        pb = ex_bmap(in, lb, unsupported);
        if (pb) {
            if (ex_block(pb, ex_bdata)) return done ? done : EX_EIO;
            ex_memcpy(b + done, ex_bdata + inb, take);
        } else {
            ex_memzero(b + done, take);
        }
        done += take;
    }
    return done;
}

int ex_mounted(void) { return ex_vol_mounted; }

static void ex_audit_bitmaps(void); /* defined at end (needs all helpers) */


/* Mount drive (AHCI64 numbering). 0 ok, negative errno. Single volume. */
int ex_mount(int drive) {
    u32 magic, incompat, log, rev, isz;
    u64 f;
    if (ahci64_read(drive, EX_SUPER_OFF / 512, 1024 / 512, ex_bmeta))
        return EX_EIO;
    magic = ex_rd16(ex_bmeta, 56);
    if (magic != EX_SUPER_MAGIC) return EX_EINVAL;
    incompat = ex_rd32(ex_bmeta, 96);
    if (incompat & ~EX_INCOMPAT_OK) {
        s_puts("[FS] ext2 unsupported features mask=");
        s_hex64(incompat & ~EX_INCOMPAT_OK);
        s_puts("\n");
        return EX_EFBIG;
    }
    log = ex_rd32(ex_bmeta, 24);
    if (log > 2) return EX_EFBIG;
    rev = ex_rd32(ex_bmeta, 76);
    isz = rev >= 1 ? ex_rd32(ex_bmeta, 88) : 128;
    if (isz < 128 || isz > 1024 || (isz & (isz - 1))) return EX_EFBIG;
    f = spin64_lock_irqsave(&ex_lock);
    ex_drive = drive;
    ex_block_size = 1024u << log;
    ex_blocks = ex_rd32(ex_bmeta, 4);
    ex_inodes = ex_rd32(ex_bmeta, 0);
    ex_first_block = ex_rd32(ex_bmeta, 20);
    ex_bpg = ex_rd32(ex_bmeta, 32);
    ex_ipg = ex_rd32(ex_bmeta, 40);
    ex_inode_size = isz;
    ex_vol_mounted = 1;
    spin64_unlock_irqrestore(&ex_lock, f);
    ex_audit_bitmaps(); /* seed tools leave owned-but-unmarked blocks */
    s_puts("[FS] ext2 mounted drive=");
    s_dec64((u64)drive);
    s_puts(" blocks=");
    s_dec64(ex_blocks);
    s_puts(" block_size=");
    s_dec64(ex_block_size);
    s_puts(" inodes=");
    s_dec64(ex_inodes);
    s_puts(" bpg=");
    s_dec64(ex_bpg);
    s_puts(" ipg=");
    s_dec64(ex_ipg);
    s_puts(" isz=");
    s_dec64(ex_inode_size);
    s_puts("\n");
    return 0;
}

/* Resolve absolute path -> ino/is_dir/size. Depth cap 16. */
int ex_lookup(const char *path, u32 *ino_out, int *is_dir, u32 *size_out) {
    ex_inode_t in;
    const char *p;
    u64 f;
    int rc = 0;
    if (!ex_vol_mounted) return EX_EIO;
    f = spin64_lock_irqsave(&ex_lock);
    if (ex_inode(EX_ROOT_INO, &in)) {
        rc = EX_EIO;
        goto out;
    }
    if (path[0] == '/' && !path[1]) {
        *ino_out = EX_ROOT_INO;
        *is_dir = 1;
        *size_out = in.size;
        goto out;
    }
    p = path;
    while (*p) {
        char comp[64];
        int n = 0, found = 0;
        while (*p == '/') p++;
        if (!*p) break;
        while (*p && *p != '/' && n < 63) comp[n++] = *p++;
        comp[n] = '\0';
        while (*p && *p != '/') p++;
        if (ex_streq(comp, ".")) continue;
        if (!(in.mode & 0x4000)) {
            rc = EX_ENOTDIR;
            goto out;
        }
        /* Dir scan: direct + single-indirect blocks (matches write scope;
         * doubly-indirect dirs are vanishingly rare, refused below). */
        for (u32 lb = 0; lb * ex_block_size < in.size; lb++) {
            u32 pb;
            int uns = 0;
            u32 off;
            if (lb == EX_DIRECT + ex_block_size / 4) {
                rc = EX_EFBIG;
                goto out;
            }
            pb = ex_bmap(&in, lb, &uns);
            if (uns) {
                rc = EX_EFBIG;
                goto out;
            }
            if (!pb) continue;
            if (ex_block(pb, ex_bdata)) {
                rc = EX_EIO;
                goto out;
            }
            off = 0;
            while (off + 8 <= ex_block_size) {
                u32 ino = ex_rd32(ex_bdata, off);
                u16 rlen = ex_rd16(ex_bdata, off + 4);
                u8 nlen = ex_bdata[off + 6];
                char tmp[64];
                int i;
                if (!rlen || off + rlen > ex_block_size) break;
                if (ino && nlen && off + 8 + nlen <= ex_block_size) {
                    for (i = 0; i < nlen && i < 63; i++)
                        tmp[i] = (char)ex_bdata[off + 8 + i];
                    tmp[i] = '\0';
                    if (i == nlen && ex_streq(tmp, comp)) {
                        ex_inode_t next;
                        if (ex_inode(ino, &next)) {
                            rc = EX_EIO;
                            goto out;
                        }
                        *ino_out = ino;
                        *is_dir = (next.mode & 0x4000) != 0;
                        *size_out = next.size;
                        in = next;
                        found = 1;
                        break;
                    }
                }
                off += rlen;
            }
            if (found) break;
        }
        if (!found) {
            rc = EX_ENOENT;
            goto out;
        }
    }
out:
    spin64_unlock_irqrestore(&ex_lock, f);
    return rc;
}

/* Read file data by ino (holes -> zeros). Returns bytes or negative. */
long ex_read_ino(u32 ino, u64 off, void *buf, u32 len) {
    ex_inode_t in;
    int unsupported = 0;
    long got;
    u64 f;
    if (!ex_vol_mounted) return EX_EIO;
    f = spin64_lock_irqsave(&ex_lock);
    if (ex_inode(ino, &in)) {
        spin64_unlock_irqrestore(&ex_lock, f);
        return EX_EIO;
    }
    if (in.mode & 0x4000) {
        spin64_unlock_irqrestore(&ex_lock, f);
        return EX_EISDIR;
    }
    got = ex_data_read(&in, off, buf, len, &unsupported);
    spin64_unlock_irqrestore(&ex_lock, f);
    if (unsupported) return EX_EFBIG;
    return got;
}

/* List one directory entry by index (for READDIR). */
int ex_readdir_ino(u32 ino, u32 index, u32 *e_ino, int *e_dir, char *e_name) {
    ex_inode_t in;
    u32 seen = 0;
    u64 f;
    int rc = 0; /* exhausted cleanly (past-end is normal, not ENOENT) */
    if (!ex_vol_mounted) return EX_EIO;
    f = spin64_lock_irqsave(&ex_lock);
    if (ex_inode(ino, &in)) {
        rc = EX_EIO;
        goto out;
    }
    if (!(in.mode & 0x4000)) {
        rc = EX_ENOTDIR;
        goto out;
    }
    for (u32 lb = 0; lb * ex_block_size < in.size; lb++) {
        u32 pb;
        int uns = 0;
        u32 off;
        if (lb == EX_DIRECT + ex_block_size / 4) {
            rc = EX_EFBIG;
            goto out;
        }
        pb = ex_bmap(&in, lb, &uns);
        if (uns) {
            rc = EX_EFBIG;
            goto out;
        }
        if (!pb) continue;
        if (ex_block(pb, ex_bdata)) {
            rc = EX_EIO;
            goto out;
        }
        off = 0;
        while (off + 8 <= ex_block_size) {
            u32 ei = ex_rd32(ex_bdata, off);
            u16 rlen = ex_rd16(ex_bdata, off + 4);
            u8 nlen = ex_bdata[off + 6];
            int i;
            if (!rlen || off + rlen > ex_block_size) break;
            if (ei && nlen && off + 8 + nlen <= ex_block_size) {
                if (seen == index) {
                    ex_inode_t child;
                    for (i = 0; i < nlen && i < 63; i++)
                        e_name[i] = (char)ex_bdata[off + 8 + i];
                    e_name[i] = '\0';
                    *e_ino = ei;
                    if (ex_inode(ei, &child)) {
                        rc = EX_EIO;
                        goto out;
                    }
                    *e_dir = (child.mode & 0x4000) != 0;
                    rc = 1;
                    goto out;
                }
                seen++;
            }
            off += rlen;
        }
    }
out:
    spin64_unlock_irqrestore(&ex_lock, f);
    return rc;
}

/* ---- F2b write side (direct + single-indirect; doubly refused on alloc,
 * matching the 32-bit write scope) ---- */

/* Write one FS block from buf (chunked at 128-sector LBA boundaries,
 * mirroring the read batch rule). */
static int ex_write_block(u32 blk, const void *buf) {
    u64 byte = (u64)blk * ex_block_size;
    u32 lba = (u32)(byte / 512);
    u32 nsec = ex_block_size / 512;
    u32 got = 0;
    while (got < nsec) {
        u32 boundary = 128 - ((lba + got) & 127);
        u32 n = nsec - got;
        if (n > boundary) n = boundary;
        if (ahci64_write(ex_drive, lba + got, (int)n,
                         (const u8 *)buf + got * 512))
            return EX_EIO;
        got += n;
    }
    return 0;
}

/* Superblock / group-descriptor helpers (all via B_META, caller locked). */
static u32 ex_sb32(u32 off) { return ex_rd32(ex_bmeta, off); }

static int ex_read_sb(void) {
    if (ahci64_read(ex_drive, EX_SUPER_OFF / 512, 1024 / 512, ex_bmeta))
        return EX_EIO;
    return 0;
}

static int ex_write_sb(void) {
    u64 byte = EX_SUPER_OFF;
    u32 lba = (u32)(byte / 512);
    if (ahci64_write(ex_drive, lba, 2, ex_bmeta)) return EX_EIO;
    return 0;
}

/* Group descriptor block for group g (single-group images keep it simple;
 * multi-group walks each descriptor's own block like the read path). */
static int ex_gd_block(u32 grp, u32 *blk_out) {
    u32 gd_bytes = (ex_first_block + 1) * ex_block_size + grp * 32;
    *blk_out = gd_bytes / ex_block_size;
    return 0;
}

/* True for superblock, GDT, bitmaps, inode tables (never allocate).
 * Uses B_META2 scratch: runs inside alloc/free while callers hold live
 * data in B_META (bitmaps) and B_IND (indirect tables). */
static int ex_is_meta(u32 blk) {
    u32 sb0 = EX_SUPER_OFF / ex_block_size;
    u32 sb1 = (EX_SUPER_OFF + 1023) / ex_block_size;
    u32 ngroups, g;
    if (blk == 0 || blk < ex_first_block) return 1;
    if (blk >= sb0 && blk <= sb1) return 1;
    ngroups = (ex_blocks + ex_bpg - 1) / ex_bpg;
    if (ngroups > 32) ngroups = 32;
    for (g = 0; g < ngroups; g++) {
        u32 gd_blk, off, bbm, ibm, itab, ipgb;
        ex_gd_block(g, &gd_blk);
        if (blk == gd_blk) return 1;
        if (ex_block(gd_blk, ex_bmeta2)) continue;
        off = (g * 32) % ex_block_size;
        bbm = ex_rd32(ex_bmeta2, off + 0);
        ibm = ex_rd32(ex_bmeta2, off + 4);
        itab = ex_rd32(ex_bmeta2, off + 8);
        if (blk == bbm || blk == ibm) return 1;
        /* Backup superblock, GDT, reserved GDT: everything from the
         * group start up to the block bitmap (group 0: 1-65,
         * group 1: 8193-8257). Never allocate. */
        if (bbm && blk > g * ex_bpg && blk < bbm) return 1;
        ipgb = (ex_ipg * ex_inode_size + ex_block_size - 1) / ex_block_size;
        if (itab && blk >= itab && blk < itab + ipgb) return 1;
    }
    return 0;
}

/* Allocate one free data block (first fit, metadata-guarded). */
static int ex_alloc_block(u32 *blk_out) {
    u32 ngroups = (ex_blocks + ex_bpg - 1) / ex_bpg;
    u32 g;
    if (ngroups > 32) ngroups = 32;
    for (g = 0; g < ngroups; g++) {
        u32 gd_blk, bbm, bit, abs;
        ex_gd_block(g, &gd_blk);
        if (ex_block(gd_blk, ex_bmeta)) return EX_EIO;
        bbm = ex_rd32(ex_bmeta, (g * 32) % ex_block_size + 0);
        if (!bbm) return EX_EIO;
        if (ex_block(bbm, ex_bmeta)) return EX_EIO;
        for (bit = 0; bit < ex_bpg; bit++) {
            /* Bitmap bit N owns block N+1 (block 1 = bit 0; block 0
             * has no bit). Matches mke2fs/e2fsck convention. */
            abs = g * ex_bpg + bit + 1;
            if (abs >= ex_blocks) break;
            if (ex_is_meta(abs)) continue;
            if (!(ex_bmeta[bit / 8] & (1u << (bit % 8)))) {
                ex_bmeta[bit / 8] |= (u8)(1u << (bit % 8));
                if (ex_write_block(bbm, ex_bmeta)) return EX_EIO;
                /* Decrement free counts (superblock + group). */
                if (ex_read_sb()) return EX_EIO;
                {
                    u32 fb = ex_sb32(12);
                    u32 fi = ex_sb32(16);
                    if (fb) {
                        ex_bmeta[12] = (u8)(fb - 1);
                        ex_bmeta[13] = (u8)((fb - 1) >> 8);
                        ex_bmeta[14] = (u8)((fb - 1) >> 16);
                        ex_bmeta[15] = (u8)((fb - 1) >> 24);
                    }
                    (void)fi;
                }
                if (ex_write_sb()) return EX_EIO;
                if (ex_block(gd_blk, ex_bmeta)) return EX_EIO;
                {
                    u32 off = (g * 32) % ex_block_size;
                    u32 gfb = ex_rd32(ex_bmeta, off + 12) & 0xFFFF;
                    if (gfb) {
                        gfb--;
                        ex_bmeta[off + 12] = (u8)(gfb & 0xFF);
                        ex_bmeta[off + 13] = (u8)((gfb >> 8) & 0xFF);
                    }
                }
                if (ex_write_block(gd_blk, ex_bmeta)) return EX_EIO;
                /* Zero the fresh block (no stale data leaks). */
                ex_memzero(ex_bdata, ex_block_size);
                if (ex_write_block(abs, ex_bdata)) return EX_EIO;
                *blk_out = abs;
                return 0;
            }
        }
    }
    return EX_EFBIG; /* full */
}

/* Free one data block (bitmap + free counts). */
static int ex_free_block(u32 blk) {
    u32 ngroups = (ex_blocks + ex_bpg - 1) / ex_bpg;
    u32 g;
    if (!blk || ex_is_meta(blk)) return EX_EINVAL;
    if (ngroups > 32) ngroups = 32;
    for (g = 0; g < ngroups; g++) {
        u32 gd_blk, bit, bbm;
        if (!blk || (blk - 1) / ex_bpg != g) continue;
        bit = (blk - 1) % ex_bpg;
        ex_gd_block(g, &gd_blk);
        if (ex_block(gd_blk, ex_bmeta)) return EX_EIO;
        bbm = ex_rd32(ex_bmeta, (g * 32) % ex_block_size + 0);
        if (!bbm) return EX_EIO;
        if (ex_block(bbm, ex_bmeta)) return EX_EIO;
        ex_bmeta[bit / 8] &= (u8)~(1u << (bit % 8));
        if (ex_write_block(bbm, ex_bmeta)) return EX_EIO;
        if (ex_read_sb()) return EX_EIO;
        {
            u32 fb = ex_rd32(ex_bmeta, 12);
            ex_bmeta[12] = (u8)(fb + 1);
            ex_bmeta[13] = (u8)((fb + 1) >> 8);
            ex_bmeta[14] = (u8)((fb + 1) >> 16);
            ex_bmeta[15] = (u8)((fb + 1) >> 24);
        }
        if (ex_write_sb()) return EX_EIO;
        if (ex_block(gd_blk, ex_bmeta)) return EX_EIO;
        {
            u32 off = (g * 32) % ex_block_size;
            u32 gfb = ex_rd32(ex_bmeta, off + 12) & 0xFFFF;
            gfb++;
            ex_bmeta[off + 12] = (u8)(gfb & 0xFF);
            ex_bmeta[off + 13] = (u8)((gfb >> 8) & 0xFF);
        }
        if (ex_write_block(gd_blk, ex_bmeta)) return EX_EIO;
        return 0;
    }
    return EX_EINVAL;
}

/* Write an inode slot back (read-modify-write preserves the rest). */
static int ex_write_inode(u32 ino, const ex_inode_t *in) {
    u32 per_group = ex_ipg;
    u32 grp = (ino - 1) / per_group;
    u32 idx = (ino - 1) % per_group;
    u32 gd_bytes = (ex_first_block + 1) * ex_block_size + grp * 32;
    u32 gd_blk = gd_bytes / ex_block_size;
    u32 gd_off = gd_bytes % ex_block_size;
    u32 itable, ino_bytes, ib, io;
    u8 *p;
    int i;
    if (!ino || ino > ex_inodes) return EX_ENOENT;
    if (ex_block(gd_blk, ex_bmeta)) return EX_EIO;
    itable = ex_rd32(ex_bmeta, gd_off + 8);
    if (!itable) return EX_EIO;
    ino_bytes = idx * ex_inode_size;
    ib = itable + ino_bytes / ex_block_size;
    io = ino_bytes % ex_block_size;
    if (io + 128 > ex_block_size) return EX_EIO;
    if (ex_block(ib, ex_bmeta)) return EX_EIO;
    p = ex_bmeta + io;
    p[0] = (u8)(in->mode & 0xFF);
    p[1] = (u8)((in->mode >> 8) & 0xFF);
    p[4] = (u8)(in->size & 0xFF);
    p[5] = (u8)((in->size >> 8) & 0xFF);
    p[6] = (u8)((in->size >> 16) & 0xFF);
    p[7] = (u8)((in->size >> 24) & 0xFF);
    p[28] = (u8)(in->blocks & 0xFF);
    p[29] = (u8)((in->blocks >> 8) & 0xFF);
    p[30] = (u8)((in->blocks >> 16) & 0xFF);
    p[31] = (u8)((in->blocks >> 24) & 0xFF);
    for (i = 0; i < 15; i++) {
        p[40 + i * 4] = (u8)(in->block[i] & 0xFF);
        p[41 + i * 4] = (u8)((in->block[i] >> 8) & 0xFF);
        p[42 + i * 4] = (u8)((in->block[i] >> 16) & 0xFF);
        p[43 + i * 4] = (u8)((in->block[i] >> 24) & 0xFF);
    }
    if (ex_write_block(ib, ex_bmeta)) return EX_EIO;
    return 0;
}

/* Logical -> physical with optional allocation (direct + single only;
 * doubly-alloc refused EFBIG like the 32-bit scope). */
static int ex_get_block(ex_inode_t *in, u32 lb, int alloc, u32 *pb_out) {
    u32 per = ex_block_size / 4;
    if (lb < EX_DIRECT) {
        if (!in->block[lb]) {
            u32 nb;
            if (!alloc) {
                *pb_out = 0;
                return 0;
            }
            if (ex_alloc_block(&nb)) return EX_EFBIG;
            in->block[lb] = nb;
        }
        *pb_out = in->block[lb];
        return 0;
    }
    lb -= EX_DIRECT;
    if (lb < per) {
        u32 tab = in->block[12];
        u32 nb;
        if (!tab) {
            if (!alloc) {
                *pb_out = 0;
                return 0;
            }
            if (ex_alloc_block(&tab)) return EX_EFBIG;
            in->block[12] = tab;
            ex_memzero(ex_bind, ex_block_size);
            if (ex_write_block(tab, ex_bind)) return EX_EIO;
        }
        if (ex_block(tab, ex_bind)) return EX_EIO;
        nb = ex_rd32(ex_bind, lb * 4);
        if (!nb) {
            if (!alloc) {
                *pb_out = 0;
                return 0;
            }
            if (ex_alloc_block(&nb)) return EX_EFBIG;
            ex_bind[lb * 4] = (u8)(nb & 0xFF);
            ex_bind[lb * 4 + 1] = (u8)((nb >> 8) & 0xFF);
            ex_bind[lb * 4 + 2] = (u8)((nb >> 16) & 0xFF);
            ex_bind[lb * 4 + 3] = (u8)((nb >> 24) & 0xFF);
            if (ex_write_block(tab, ex_bind)) return EX_EIO;
        }
        *pb_out = nb;
        return 0;
    }
    return EX_EFBIG;
}

/* Recount data blocks (512B units) by walking direct + single. */
static u32 ex_count_blocks(const ex_inode_t *in) {
    u32 per = ex_block_size / 4;
    u32 n = 0, i;
    u32 per512 = ex_block_size / 512;
    for (i = 0; i < EX_DIRECT; i++)
        if (in->block[i]) n += per512;
    if (in->block[12]) {
        n += per512; /* the indirect table itself */
        if (ex_block(in->block[12], ex_bind) == 0) {
            for (i = 0; i < per; i++)
                if (ex_rd32(ex_bind, i * 4)) n += per512;
        }
    }
    return n;
}

/* True when doubly-indirect is in use (write paths refuse: EFBIG). */
static int ex_uses_doubly(const ex_inode_t *in) { return in->block[13] != 0; }

/* Free all data blocks of an inode (direct + single + table). */
static int ex_free_data(ex_inode_t *in) {
    u32 per = ex_block_size / 4;
    u32 i;
    int rc;
    if (in->block[13] || in->block[14]) return EX_EFBIG;
    for (i = 0; i < EX_DIRECT; i++) {
        if (in->block[i]) {
            rc = ex_free_block(in->block[i]);
            if (rc) return rc;
            in->block[i] = 0;
        }
    }
    if (in->block[12]) {
        if (ex_block(in->block[12], ex_bind)) return EX_EIO;
        for (i = 0; i < per; i++) {
            u32 b = ex_rd32(ex_bind, i * 4);
            if (b) {
                rc = ex_free_block(b);
                if (rc) return rc;
            }
        }
        rc = ex_free_block(in->block[12]);
        if (rc) return rc;
        in->block[12] = 0;
    }
    return 0;
}

/* Truncate to exactly size (only shrink-to-less supported in F2b;
 * callers grow via write). Doubly-mapped files refused. */
static int ex_truncate_locked(u32 ino, u32 size) {
    ex_inode_t in;
    int rc;
    if (ex_inode(ino, &in)) return EX_EIO;
    if (in.mode & 0x4000) return EX_EISDIR;
    if (ex_uses_doubly(&in)) return EX_EFBIG;
    if (size >= in.size) {
        if (size == in.size) return 0;
        return EX_EINVAL; /* no sparse growth here; write appends */
    }
    if (size == 0) {
        int rc = ex_free_data(&in);
        if (rc) return rc;
        in.size = 0;
        in.blocks = 0;
        return ex_write_inode(ino, &in);
    }
    /* Partial first block beyond size is kept whole (ext2 keeps the
     * tail bytes; size governs visibility). Only whole blocks past
     * the size frontier are freed. */
    {
        u32 keep = (size + ex_block_size - 1) / ex_block_size;
        u32 per = ex_block_size / 4;
        u32 i;
        for (i = keep; i < EX_DIRECT; i++) {
            if (in.block[i]) {
                rc = ex_free_block(in.block[i]);
                if (rc) return rc;
                in.block[i] = 0;
            }
        }
        if (keep >= EX_DIRECT) {
            /* keep all */
        } else if (in.block[12]) {
            /* Free single entries from the frontier, then the table
             * itself if it ends up empty. */
            u32 start = (keep > EX_DIRECT) ? keep - EX_DIRECT : 0;
            u32 j, any = 0;
            if (ex_block(in.block[12], ex_bind)) return EX_EIO;
            for (j = start; j < per; j++) {
                u32 b = ex_rd32(ex_bind, j * 4);
                if (b) {
                    rc = ex_free_block(b);
                    if (rc) return rc;
                    ex_bind[j * 4] = 0;
                    ex_bind[j * 4 + 1] = 0;
                    ex_bind[j * 4 + 2] = 0;
                    ex_bind[j * 4 + 3] = 0;
                }
            }
            for (j = 0; j < per; j++) {
                if (ex_rd32(ex_bind, j * 4)) {
                    any = 1;
                    break;
                }
            }
            if (any) {
                if (ex_write_block(in.block[12], ex_bind)) return EX_EIO;
            } else {
                rc = ex_free_block(in.block[12]);
                if (rc) return rc;
                in.block[12] = 0;
            }
        }
        (void)per;
    }
    in.size = size;
    in.blocks = ex_count_blocks(&in);
    return ex_write_inode(ino, &in);
}

/* Write file data (overwrite + append; off > size refused, no sparse).
 * Doubly-mapped files refused. Returns bytes or negative. */
static long ex_write_data_locked(u32 ino, u64 off, const void *buf, u32 len) {
    ex_inode_t in;
    const u8 *b = (const u8 *)buf;
    long done = 0;
    if (ex_inode(ino, &in)) return EX_EIO;
    if (in.mode & 0x4000) return EX_EISDIR;
    if (ex_uses_doubly(&in)) return EX_EFBIG;
    if (off > in.size) return EX_EINVAL;
    if (off + len < off) return EX_EINVAL;
    while ((u32)done < len) {
        u64 pos = off + (u64)done;
        u32 lb = (u32)(pos / ex_block_size);
        u32 inb = (u32)(pos % ex_block_size);
        u32 take = ex_block_size - inb;
        u32 pb;
        int rc;
        if (take > len - (u32)done) take = len - (u32)done;
        if (lb >= EX_DIRECT + ex_block_size / 4) return EX_EFBIG;
        rc = ex_get_block(&in, lb, 1, &pb);
        if (rc) return done ? done : rc;
        if (ex_block(pb, ex_bdata)) return done ? done : EX_EIO;
        ex_memcpy(ex_bdata + inb, b + done, take);
        if (ex_write_block(pb, ex_bdata)) return done ? done : EX_EIO;
        done += take;
    }
    if (off + (u64)done > in.size) in.size = (u32)(off + (u64)done);
    in.blocks = ex_count_blocks(&in);
    {
        int rc = ex_write_inode(ino, &in);
        if (rc) return done ? (long)done : (long)rc;
    }
    return done;
}

/* Allocate an inode (first free bit, files and dirs share the pool). */
static int ex_alloc_inode(u32 *ino_out) {
    u32 ngroups = (ex_inodes + ex_ipg - 1) / ex_ipg;
    u32 g;
    if (ngroups > 32) ngroups = 32;
    for (g = 0; g < ngroups; g++) {
        u32 gd_blk, ibm, bit, ino;
        ex_gd_block(g, &gd_blk);
        if (ex_block(gd_blk, ex_bmeta)) return EX_EIO;
        ibm = ex_rd32(ex_bmeta, (g * 32) % ex_block_size + 4);
        if (!ibm) return EX_EIO;
        if (ex_block(ibm, ex_bmeta)) return EX_EIO;
        for (bit = 0; bit < ex_block_size * 8; bit++) {
            if (bit >= ex_ipg) break;
            ino = g * ex_ipg + bit + 1;
            if (ino > ex_inodes || ino < 12) continue; /* reserve 1-11 */
            if (!(ex_bmeta[bit / 8] & (1u << (bit % 8)))) {
                ex_bmeta[bit / 8] |= (u8)(1u << (bit % 8));
                if (ex_write_block(ibm, ex_bmeta)) return EX_EIO;
                if (ex_read_sb()) return EX_EIO;
                {
                    u32 fi = ex_rd32(ex_bmeta, 16);
                    if (fi) {
                        ex_bmeta[16] = (u8)(fi - 1);
                        ex_bmeta[17] = (u8)((fi - 1) >> 8);
                        ex_bmeta[18] = (u8)((fi - 1) >> 16);
                        ex_bmeta[19] = (u8)((fi - 1) >> 24);
                    }
                }
                if (ex_write_sb()) return EX_EIO;
                if (ex_block(gd_blk, ex_bmeta)) return EX_EIO;
                {
                    u32 off = (g * 32) % ex_block_size;
                    u32 gfi = ex_rd32(ex_bmeta, off + 14) & 0xFFFF;
                    if (gfi) {
                        gfi--;
                        ex_bmeta[off + 14] = (u8)(gfi & 0xFF);
                        ex_bmeta[off + 15] = (u8)((gfi >> 8) & 0xFF);
                    }
                }
                if (ex_write_block(gd_blk, ex_bmeta)) return EX_EIO;
                *ino_out = ino;
                return 0;
            }
        }
    }
    return EX_EFBIG;
}

/* Append one name->ino entry to a directory (hole/split/grow). */
static int ex_dir_add(u32 dir_ino, u32 ino, const char *name, int is_dir) {
    ex_inode_t dir;
    u32 nlen = 0;
    u32 need, lb, per;
    while (name[nlen] && nlen < 64) nlen++;
    if (!nlen || nlen > 255) return EX_EINVAL;
    need = (8 + nlen + 3) & ~3u;
    if (ex_inode(dir_ino, &dir)) return EX_EIO;
    if (!(dir.mode & 0x4000)) return EX_ENOTDIR;
    if (ex_uses_doubly(&dir)) return EX_EFBIG;
    per = ex_block_size / 4;
    (void)per;
    for (lb = 0;; lb++) {
        u32 pb;
        int rc;
        u32 off;
        if (lb >= EX_DIRECT + ex_block_size / 4) return EX_EFBIG;
        rc = ex_get_block(&dir, lb, 1, &pb);
        if (rc) return rc;
        if (lb * ex_block_size >= dir.size) {
            /* Fresh block beyond EOF: single entry spanning it. Grows
             * the directory: persist size/blocks with it. */
            ex_memzero(ex_bdata, ex_block_size);
            ex_bdata[0] = (u8)(ino & 0xFF);
            ex_bdata[1] = (u8)((ino >> 8) & 0xFF);
            ex_bdata[2] = (u8)((ino >> 16) & 0xFF);
            ex_bdata[3] = (u8)((ino >> 24) & 0xFF);
            ex_bdata[4] = (u8)(ex_block_size & 0xFF);
            ex_bdata[5] = (u8)((ex_block_size >> 8) & 0xFF);
            ex_bdata[6] = (u8)nlen;
            ex_bdata[7] = (u8)(is_dir ? 2 : 1);
            for (u32 i = 0; i < nlen; i++) ex_bdata[8 + i] = (u8)name[i];
            if (ex_write_block(pb, ex_bdata)) return EX_EIO;
            dir.size += ex_block_size;
            dir.blocks = ex_count_blocks(&dir);
            return ex_write_inode(dir_ino, &dir);
        }
        if (ex_block(pb, ex_bdata)) return EX_EIO;
        off = 0;
        while (off + 8 <= ex_block_size) {
            u32 ei = ex_rd32(ex_bdata, off);
            u16 rlen = ex_rd16(ex_bdata, off + 4);
            u8 elen = ex_bdata[off + 6];
            u32 used;
            if (!rlen || off + rlen > ex_block_size) break;
            used = 8 + elen;
            used = (used + 3) & ~3u;
            if (!ei) {
                /* Free hole: fits if the whole slot fits. */
                if (rlen >= need) {
                    ex_bdata[off] = (u8)(ino & 0xFF);
                    ex_bdata[off + 1] = (u8)((ino >> 8) & 0xFF);
                    ex_bdata[off + 2] = (u8)((ino >> 16) & 0xFF);
                    ex_bdata[off + 3] = (u8)((ino >> 24) & 0xFF);
                    /* keep rlen (rest stays a hole only if room;
                     * simplify: consume whole slot). */
                    ex_bdata[off + 6] = (u8)nlen;
                    ex_bdata[off + 7] = (u8)(is_dir ? 2 : 1);
                    for (u32 i = 0; i < nlen; i++)
                        ex_bdata[off + 8 + i] = (u8)name[i];
                    if (ex_write_block(pb, ex_bdata)) return EX_EIO;
                    return 0;
                }
            } else if (rlen > used && rlen - used >= need) {
                /* Split slack at the end of a live entry. */
                u32 nroff = off + used;
                ex_bdata[off + 4] = (u8)(used & 0xFF);
                ex_bdata[off + 5] = (u8)((used >> 8) & 0xFF);
                ex_bdata[nroff] = (u8)(ino & 0xFF);
                ex_bdata[nroff + 1] = (u8)((ino >> 8) & 0xFF);
                ex_bdata[nroff + 2] = (u8)((ino >> 16) & 0xFF);
                ex_bdata[nroff + 3] = (u8)((ino >> 24) & 0xFF);
                ex_bdata[nroff + 4] = (u8)((rlen - used) & 0xFF);
                ex_bdata[nroff + 5] = (u8)(((rlen - used) >> 8) & 0xFF);
                ex_bdata[nroff + 6] = (u8)nlen;
                ex_bdata[nroff + 7] = (u8)(is_dir ? 2 : 1);
                for (u32 i = 0; i < nlen; i++)
                    ex_bdata[nroff + 8 + i] = (u8)name[i];
                if (ex_write_block(pb, ex_bdata)) return EX_EIO;
                return 0;
            }
            off += rlen;
        }
    }
}

/* Create a regular file under dir_ino. Name <= 63 chars here. */
static int ex_create_locked(u32 dir_ino, const char *name, u32 *ino_out) {
    ex_inode_t dir;
    u32 ino, i;
    if (ex_inode(dir_ino, &dir)) return EX_EIO;
    if (!(dir.mode & 0x4000)) return EX_ENOTDIR;
    if (ex_alloc_inode(&ino)) return EX_EFBIG;
    /* Zero the slot first (links_count=1), then fill basics. */
    {
        u32 per_group = ex_ipg;
        u32 grp = (ino - 1) / per_group;
        u32 idx = (ino - 1) % per_group;
        u32 gd_bytes = (ex_first_block + 1) * ex_block_size + grp * 32;
        u32 gd_blk = gd_bytes / ex_block_size;
        u32 gd_off = gd_bytes % ex_block_size;
        u32 itable, ino_bytes, ib, io;
        if (ex_block(gd_blk, ex_bmeta)) return EX_EIO;
        itable = ex_rd32(ex_bmeta, gd_off + 8);
        if (!itable) return EX_EIO;
        ino_bytes = idx * ex_inode_size;
        ib = itable + ino_bytes / ex_block_size;
        io = ino_bytes % ex_block_size;
        if (io + 128 > ex_block_size) return EX_EIO;
        if (ex_block(ib, ex_bmeta)) return EX_EIO;
        for (i = 0; i < ex_inode_size && i < ex_block_size; i++)
            ex_bmeta[io + i] = 0;
        ex_bmeta[io + 0] = 0xA4;
        ex_bmeta[io + 1] = 0x81;
        ex_bmeta[io + 26] = 1; /* links_count */
        if (ex_write_block(ib, ex_bmeta)) return EX_EIO;
    }
    if (ex_dir_add(dir_ino, ino, name, 0)) {
        /* Rollback omitted (F2b): leaked inode on failed add. The add
         * only fails on IO/full, both fatal-ish already. Documented. */
        return EX_EIO;
    }
    *ino_out = ino;
    return 0;
}

/* File size by ino. */
static int ex_fsize_locked(u32 ino, u32 *size_out) {
    ex_inode_t in;
    if (ex_inode(ino, &in)) return EX_EIO;
    if (in.mode & 0x4000) return EX_EISDIR;
    *size_out = in.size;
    return 0;
}

int ex_truncate(u32 ino, u32 size) {
    u64 f = spin64_lock_irqsave(&ex_lock);
    int r = ex_truncate_locked(ino, size);
    spin64_unlock_irqrestore(&ex_lock, f);
    return r;
}

long ex_write_data(u32 ino, u64 off, const void *buf, u32 len) {
    u64 f = spin64_lock_irqsave(&ex_lock);
    long r = ex_write_data_locked(ino, off, buf, len);
    spin64_unlock_irqrestore(&ex_lock, f);
    return r;
}

int ex_create(u32 dir_ino, const char *name, u32 *ino_out) {
    u64 f = spin64_lock_irqsave(&ex_lock);
    int r = ex_create_locked(dir_ino, name, ino_out);
    spin64_unlock_irqrestore(&ex_lock, f);
    return r;
}

int ex_fsize(u32 ino, u32 *size_out) {
    u64 f = spin64_lock_irqsave(&ex_lock);
    int r = ex_fsize_locked(ino, size_out);
    spin64_unlock_irqrestore(&ex_lock, f);
    return r;
}

/* Mount-time bitmap audit: set bits for owned-but-unmarked blocks (seed
 * tools leave them; our allocator trusts the bitmap). Walks every
 * non-reserved inode's direct + single + double pointers into a private
 * audit buffer (B_META/B_IND/B_DATA stay free for the nested reads),
 * writes back dirty bitmaps once per group, recomputes free counts. */
/* Mount-time bitmap audit: rebuild block bitmaps from the inode walk
 * (seed tools leave owned-but-unmarked AND stray-set bits; our allocator
 * trusts the bitmap absolutely). Per group: zero a fresh map in B_AUD,
 * mark metadata + every owned data block, write back, recompute free
 * counts. Reserved inodes (1,3-10) own nothing by spec and are skipped.
 * Triple-indirect owners would be missed (out of scope everywhere). */
static u8 ex_baud[EX_MAX_BLOCK];

/* Mark absolute block vb owned in the rebuild map of group g (1-based
 * bitmap convention: bit N owns block g*bpg+N+1). Returns 1 if newly
 * set. Out-of-range and foreign-group blocks are ignored. */
static int ex_audit_mark(u32 vb, u32 g, u32 *fixed) {
    u32 lo = g * ex_bpg, slot;
    if (!vb || vb >= ex_blocks) return 0;
    slot = vb - 1;
    if (slot < lo || slot >= lo + ex_bpg) return 0;
    slot -= lo;
    if (ex_baud[slot / 8] & (1u << (slot % 8))) return 0;
    ex_baud[slot / 8] |= (u8)(1u << (slot % 8));
    if (fixed) (*fixed)++;
    return 1;
}

static void ex_audit_bitmaps(void) {
    u32 ngroups = (ex_blocks + ex_bpg - 1) / ex_bpg;
    u32 g, fixed = 0, sb_free = 0;
    u64 f;
    if (ngroups > 32) ngroups = 32;
    f = spin64_lock_irqsave(&ex_lock);
    /* Pre-pass: triple-indirect owners can't be audited (out of scope
     * everywhere); their presence aborts the audit, bitmaps untouched. */
    for (g = 0; g < ngroups; g++) {
        u32 idx, ino;
        for (idx = 0; idx < ex_ipg; idx++) {
            ex_inode_t tin;
            ino = g * ex_ipg + idx + 1;
            if (ino > ex_inodes) break;
            if (ino == 1 || (ino >= 3 && ino <= 10)) continue;
            if (ex_inode(ino, &tin)) continue;
            if (!tin.mode) continue;
            if (tin.block[14]) {
                spin64_unlock_irqrestore(&ex_lock, f);
                s_puts("[FS] audit skipped (triple-indirect present)\n");
                return;
            }
        }
    }
    for (g = 0; g < ngroups; g++) {
        u32 gd_blk, bbm, idx, ino, b;
        u32 gfree = 0, k;
        ex_gd_block(g, &gd_blk);
        if (ex_block(gd_blk, ex_bmeta)) continue;
        bbm = ex_rd32(ex_bmeta, (g * 32) % ex_block_size + 0);
        if (!bbm) continue;
        /* Fresh map (rebuild, not patch): zero, then mark metadata. */
        for (k = 0; k < ex_block_size; k++) ex_baud[k] = 0;
        {
            u32 lo = g * ex_bpg, hi = lo + ex_bpg, m;
            u32 itab, ipgb;
            if (hi > ex_blocks) hi = ex_blocks;
#define EX_MARK(b)                                                     \
    do {                                                               \
        u32 _b = (b);                                                  \
        if (_b >= 1 && _b < ex_blocks && _b - 1 >= lo && _b - 1 < hi)   \
            ex_baud[(_b - 1 - lo) / 8] |=                              \
                (u8)(1u << ((_b - 1 - lo) % 8));                        \
    } while (0)
            /* Everything from the group start up to the block bitmap
             * is metadata: primary/backup superblock, GDT, reserved
             * GDT (group 0: blocks 1-65; group 1: 8193-8257). */
            for (m = lo + 1; m < bbm && m < hi; m++) EX_MARK(m);
            itab = ex_rd32(ex_bmeta, (g * 32) % ex_block_size + 8);
            ipgb = (ex_ipg * ex_inode_size + ex_block_size - 1) /
                   ex_block_size;
            EX_MARK(bbm);
            EX_MARK(ex_rd32(ex_bmeta, (g * 32) % ex_block_size + 4));
            for (m = 0; m < ipgb; m++) EX_MARK(itab + m);
#undef EX_MARK
        }
        {
            u32 ibm2 = ex_rd32(ex_bmeta, (g * 32) % ex_block_size + 4);
            /* Inode bitmap lives in B_META2: ex_inode() below reuses
             * B_META per call, so the bitmap needs its own home. */
            if (!ibm2 || ex_block(ibm2, ex_bmeta2)) continue;
        }
        /* Only visit inodes marked used in the inode bitmap. */
        for (idx = 0; idx < ex_ipg; idx++) {
            u32 per, k;
            ex_inode_t in;
            ino = g * ex_ipg + idx + 1;
            if (ino > ex_inodes) break;
            if (!(ex_bmeta2[idx / 8] & (1u << (idx % 8)))) continue;
            if (ino == 1 || (ino >= 3 && ino <= 10)) continue;
            if (ex_inode(ino, &in)) continue;
            if (!in.mode) continue;
            for (k = 0; k < EX_DIRECT; k++)
                ex_audit_mark(in.block[k], g, &fixed);
            per = ex_block_size / 4;
            if (in.block[12]) {
                ex_audit_mark(in.block[12], g, &fixed); /* table itself */
                if (ex_block(in.block[12], ex_bind)) continue;
                for (k = 0; k < per; k++)
                    ex_audit_mark(ex_rd32(ex_bind, k * 4), g, &fixed);
            }
            if (in.block[13]) {
                u32 j;
                ex_audit_mark(in.block[13], g, &fixed); /* table itself */
                if (ex_block(in.block[13], ex_bind)) continue;
                for (k = 0; k < per; k++) {
                    u32 l1 = ex_rd32(ex_bind, k * 4);
                    if (!l1 || l1 >= ex_blocks) continue;
                    ex_audit_mark(l1, g, &fixed); /* level-1 table */
                    if (ex_block(l1, ex_bdata)) continue;
                    for (j = 0; j < per; j++)
                        ex_audit_mark(ex_rd32(ex_bdata, j * 4), g, &fixed);
                }
            }
        }
        /* Slot b of this group's bitmap owns block g*bpg+b+1; count
         * only slots whose block exists. Trailing slots past the last
         * block are padding and must be SET before write-back
         * (e2fsck requires it). */
        for (b = 0; b < ex_bpg; b++) {
            u32 blk = g * ex_bpg + b + 1;
            if (blk >= ex_blocks) {
                ex_baud[b / 8] |= (u8)(1u << (b % 8));
                continue;
            }
            if (!(ex_baud[b / 8] & (1u << (b % 8)))) gfree++;
        }
        /* Write back + recompute this group's free count. */
        {
            u64 byte = (u64)bbm * ex_block_size;
            u32 lba = (u32)(byte / 512);
            u32 nsec = ex_block_size / 512;
            u32 got = 0;
            while (got < nsec) {
                u32 boundary = 128 - ((lba + got) & 127);
                u32 n = nsec - got;
                if (n > boundary) n = boundary;
                if (ahci64_write(ex_drive, lba + got, (int)n,
                                 ex_baud + got * 512))
                    break;
                got += n;
            }
        }
        if (ex_block(gd_blk, ex_bmeta)) continue;
        {
            u32 off = (g * 32) % ex_block_size;
            ex_bmeta[off + 12] = (u8)(gfree & 0xFF);
            ex_bmeta[off + 13] = (u8)((gfree >> 8) & 0xFF);
        }
        ex_write_block(gd_blk, ex_bmeta);
        sb_free += gfree;
    }
    if (ex_read_sb()) {
        spin64_unlock_irqrestore(&ex_lock, f);
        return;
    }
    ex_bmeta[12] = (u8)(sb_free & 0xFF);
    ex_bmeta[13] = (u8)((sb_free >> 8) & 0xFF);
    ex_bmeta[14] = (u8)((sb_free >> 16) & 0xFF);
    ex_bmeta[15] = (u8)((sb_free >> 24) & 0xFF);
    ex_write_sb();
    spin64_unlock_irqrestore(&ex_lock, f);
    {
        u64 f2 = s_lock_hold();
        s_puts_locked("[FS] audit fixed ");
        s_dec64_locked(fixed);
        s_puts_locked(" bits\n");
        s_lock_drop(f2);
    }
}

