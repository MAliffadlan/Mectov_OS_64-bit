/* F2b open file table: per-task locals (task64_t.fd_table[16], global
 * indices, -1 free) -> global entries (64). Offsets live here; all data
 * goes through ex_*. Lock order: fd_lock -> ex_lock (ex never takes fd;
 * ahci below both). exit/fork hooks live in task64.c. exec keeps fds
 * (documented; no CLOEXEC yet). */
#include "cpu64.h"
#include "spin64.h"

#define FD_MAX_TASK 16
#define FD_MAX_GLOBAL 64
#define FD_MAX_OFF (64ULL * 1024 * 1024)

typedef struct {
    int in_use, ref;
    u32 ino;
    u64 off;
    int flags;
} fildes_t;

static fildes_t fds[FD_MAX_GLOBAL];
static spin64_t fd_lock = SPIN64_INIT;

static int fd_local_get(task64_t *t, int fd) {
    if (!t || fd < 0 || fd >= FD_MAX_TASK) return -1;
    return t->fd_table[fd];
}

static int fd_local_set(task64_t *t, int fd, int g) {
    if (!t || fd < 0 || fd >= FD_MAX_TASK) return -1;
    t->fd_table[fd] = g;
    return 0;
}

static int fd_alloc_global(u32 ino, int flags) {
    for (int i = 0; i < FD_MAX_GLOBAL; i++) {
        if (!fds[i].in_use) {
            fds[i].in_use = 1;
            fds[i].ref = 1;
            fds[i].ino = ino;
            fds[i].off = 0;
            fds[i].flags = flags;
            return i;
        }
    }
    return -1;
}

static int fd_map_local(task64_t *t, int g) {
    for (int i = 0; i < FD_MAX_TASK; i++) {
        if (t->fd_table[i] < 0) {
            t->fd_table[i] = g;
            return i;
        }
    }
    return -1;
}

int fd_open(const char *path, int flags) {
    task64_t *self = task64_self();
    u32 ino = 0;
    int is_dir = 0, acc;
    u32 size = 0;
    int g, fd;
    u64 f;
    long rc;
    if (!self) return -22;
    acc = flags & 3;
    if (acc != 0 && acc != 1 && acc != 2) return -22;
    rc = ex_lookup(path, &ino, &is_dir, &size);
    if (rc) {
        /* Create (files only, existing dirs): split dir + leaf. */
        int len = 0, slash = -1, i;
        u32 dir_ino = 0;
        int dir_is = 0;
        u32 dir_sz = 0;
        char dir[256], leaf[64];
        if (!(flags & 0x40) /* O_CREAT */) return (int)rc;
        while (path[len] && len < 256) len++;
        if (len >= 256 || !len) return -22;
        for (i = 0; i < len; i++)
            if (path[i] == '/') slash = i;
        if (slash < 0) return -22; /* absolute paths only */
        if (slash == 0) {
            dir[0] = '/';
            dir[1] = '\0';
        } else {
            if (slash >= 256) return -22;
            for (i = 0; i < slash; i++) dir[i] = path[i];
            dir[slash] = '\0';
        }
        if (len - slash - 1 <= 0 || len - slash - 1 > 63) return -22;
        for (i = 0; i < len - slash - 1; i++) leaf[i] = path[slash + 1 + i];
        leaf[len - slash - 1] = '\0';
        rc = ex_lookup(dir, &dir_ino, &dir_is, &dir_sz);
        if (rc) return (int)rc;
        if (!dir_is) return -20;
        rc = ex_create(dir_ino, leaf, &ino);
        if (rc) return (int)rc;
        is_dir = 0;
    }
    if (is_dir && acc != 0) return -21; /* dirs read-only here */
    if (!is_dir && (flags & 0x200 /* O_TRUNC */) && acc != 0) {
        rc = ex_truncate(ino, 0);
        if (rc) return (int)rc;
    }
    f = spin64_lock_irqsave(&fd_lock);
    g = fd_alloc_global(ino, flags);
    fd = (g >= 0) ? fd_map_local(self, g) : -1;
    if (fd < 0 && g >= 0) {
        fds[g].in_use = 0; /* table full: release */
        spin64_unlock_irqrestore(&fd_lock, f);
        return -12;
    }
    spin64_unlock_irqrestore(&fd_lock, f);
    return fd;
}

long fd_read(int fd, void *buf, u64 len) {
    task64_t *self = task64_self();
    int g;
    u64 f;
    long got;
    if (!self) return -22;
    f = spin64_lock_irqsave(&fd_lock);
    g = fd_local_get(self, fd);
    if (g < 0 || g >= FD_MAX_GLOBAL || !fds[g].in_use) {
        spin64_unlock_irqrestore(&fd_lock, f);
        return -9;
    }
    if ((fds[g].flags & 3) == 1) { /* O_WRONLY */
        spin64_unlock_irqrestore(&fd_lock, f);
        return -22;
    }
    {
        u32 ino = fds[g].ino;
        u64 off = fds[g].off;
        spin64_unlock_irqrestore(&fd_lock, f);
        got = ex_read_ino(ino, off, buf, (u32)len);
        if (got < 0) return got;
    }
    f = spin64_lock_irqsave(&fd_lock);
    g = fd_local_get(self, fd);
    if (g >= 0 && fds[g].in_use) fds[g].off += (u64)got;
    spin64_unlock_irqrestore(&fd_lock, f);
    return got;
}

long fd_write(int fd, const void *buf, u64 len) {
    task64_t *self = task64_self();
    int g;
    u64 f;
    long got;
    if (!self) return -22;
    f = spin64_lock_irqsave(&fd_lock);
    g = fd_local_get(self, fd);
    if (g < 0 || g >= FD_MAX_GLOBAL || !fds[g].in_use) {
        spin64_unlock_irqrestore(&fd_lock, f);
        return -9;
    }
    if ((fds[g].flags & 3) == 0) { /* O_RDONLY */
        spin64_unlock_irqrestore(&fd_lock, f);
        return -22;
    }
    {
        u32 ino = fds[g].ino;
        u64 off = fds[g].off;
        spin64_unlock_irqrestore(&fd_lock, f);
        got = ex_write_data(ino, off, buf, (u32)len);
        if (got < 0) return got;
    }
    f = spin64_lock_irqsave(&fd_lock);
    g = fd_local_get(self, fd);
    if (g >= 0 && fds[g].in_use) fds[g].off += (u64)got;
    spin64_unlock_irqrestore(&fd_lock, f);
    return got;
}

long fd_close(int fd) {
    task64_t *self = task64_self();
    int g;
    u64 f;
    if (!self) return -22;
    f = spin64_lock_irqsave(&fd_lock);
    g = fd_local_get(self, fd);
    if (g < 0 || g >= FD_MAX_GLOBAL || !fds[g].in_use) {
        spin64_unlock_irqrestore(&fd_lock, f);
        return -9;
    }
    fd_local_set(self, fd, -1);
    if (--fds[g].ref <= 0) {
        fds[g].in_use = 0;
        fds[g].ref = 0;
    }
    spin64_unlock_irqrestore(&fd_lock, f);
    return 0;
}

long fd_lseek(int fd, u64 off, int whence) {
    task64_t *self = task64_self();
    int g;
    u64 f, newoff;
    u32 size = 0;
    if (!self) return -22;
    f = spin64_lock_irqsave(&fd_lock);
    g = fd_local_get(self, fd);
    if (g < 0 || g >= FD_MAX_GLOBAL || !fds[g].in_use) {
        spin64_unlock_irqrestore(&fd_lock, f);
        return -9;
    }
    {
        u32 ino = fds[g].ino;
        u64 cur = fds[g].off;
        spin64_unlock_irqrestore(&fd_lock, f);
        if (whence == 2) {
            if (ex_fsize(ino, &size)) return -5;
            if (off > FD_MAX_OFF - size) return -22;
            newoff = size + off;
        } else if (whence == 1) {
            if (off > FD_MAX_OFF - cur) return -22;
            newoff = cur + off;
        } else if (whence == 0) {
            newoff = off;
        } else {
            return -22;
        }
        if (newoff > FD_MAX_OFF) return -22;
    }
    f = spin64_lock_irqsave(&fd_lock);
    g = fd_local_get(self, fd);
    if (g >= 0 && fds[g].in_use) fds[g].off = newoff;
    spin64_unlock_irqrestore(&fd_lock, f);
    return (long)newoff;
}

/* Close every fd of an exiting task (mirrors fb/win release). */
void fd_close_all(task64_t *t) {
    u64 f;
    int i;
    if (!t) return;
    f = spin64_lock_irqsave(&fd_lock);
    for (i = 0; i < FD_MAX_TASK; i++) {
        int g = t->fd_table[i];
        if (g >= 0 && g < FD_MAX_GLOBAL && fds[g].in_use) {
            if (--fds[g].ref <= 0) {
                fds[g].in_use = 0;
                fds[g].ref = 0;
            }
        }
        t->fd_table[i] = -1;
    }
    spin64_unlock_irqrestore(&fd_lock, f);
}

/* Fork inheritance: copy table, bump shared refs. */
void fd_inherit(task64_t *dst, task64_t *src) {
    u64 f;
    int i;
    if (!dst || !src) return;
    f = spin64_lock_irqsave(&fd_lock);
    for (i = 0; i < FD_MAX_TASK; i++) {
        int g = src->fd_table[i];
        dst->fd_table[i] = g;
        if (g >= 0 && g < FD_MAX_GLOBAL && fds[g].in_use) fds[g].ref++;
    }
    spin64_unlock_irqrestore(&fd_lock, f);
}
