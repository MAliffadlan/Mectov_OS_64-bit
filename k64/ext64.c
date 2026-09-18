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
    u32 block[15];
} ex_inode_t;

static spin64_t ex_lock = SPIN64_INIT;
static int ex_vol_mounted = 0;
static int ex_drive = 0;
static u32 ex_block_size = 0;
static u32 ex_inodes = 0, ex_blocks = 0, ex_first_block = 0;
static u32 ex_bpg = 0, ex_ipg = 0, ex_inode_size = 0;

/* Static buffers (block_size <= 4096 always): B_DATA bulk, B_IND indirect
 * tables (shared across nesting: extraction precedes the nested call),
 * B_META inode/GDT/superblock. One lock, no allocs. */
static u8 ex_bdata[EX_MAX_BLOCK];
static u8 ex_bind[EX_MAX_BLOCK];
static u8 ex_bmeta[EX_MAX_BLOCK];

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
        int n = nsec - got;
        if (n > 128) n = 128;
        if (ahci64_read(ex_drive, lba + got, n, (u8 *)dst + got * 512))
            return -EX_EIO;
        got += (u32)n;
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
    s_puts("[FS] ext2 mounted drive=");
    s_dec64((u64)drive);
    s_puts(" blocks=");
    s_dec64(ex_blocks);
    s_puts(" block_size=");
    s_dec64(ex_block_size);
    s_puts(" inodes=");
    s_dec64(ex_inodes);
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
