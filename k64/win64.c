/* D1 window slots: kernel-composited rectangles with per-slot buffers.
 *
 * Pool is a static 8MB (kernel .bss — PMM already reserves the image to
 * _kernel_end, identity map covers it, plain cached RAM so drawing is
 * fast). Slots composite bottom-to-top in cons_present(), above the
 * console backbuffer and below the mouse cursor. No alpha, no restack:
 * z-order is slot index order (creation order).
 *
 * LOCKING: public ops take serial_lock internally (slots + console dirty
 * bands share it with the text paths). win_composite_scanline runs under
 * the caller's present() lock — never locks itself. Owner slots die with
 * their task via win_owner_release (exit/exec hooks, mirrors fb64).
 */
#include "cpu64.h"
#include "cons64.h"
#include "font8x16.h"

#define WIN_MAX 8
#define WIN_POOL_BYTES (8UL * 1024 * 1024)
#define WIN_TITLE 16
#define WIN_MAX_DIM 1024

typedef struct {
    int used;
    u32 x, y, w, h;
    u32 off; /* byte offset into pool */
    task64_t *owner;
    char title[WIN_TITLE];
} win_t;

static win_t wins[WIN_MAX];
static u32 win_pool[WIN_POOL_BYTES / 4];
static u64 pool_bump = 0;

typedef struct {
    u32 off, size;
} win_free_t;
static win_free_t freev[WIN_MAX + 1];
static int freen = 0;

/* First-fit from the free list, else bump. Caller holds serial_lock. */
static int pool_alloc(u32 size, u32 *off_out) {
    int i, j;
    for (i = 0; i < freen; i++) {
        if (freev[i].size >= size) {
            *off_out = freev[i].off;
            if (freev[i].size == size) {
                for (j = i; j + 1 < freen; j++) freev[j] = freev[j + 1];
                freen--;
            } else {
                freev[i].off += size;
                freev[i].size -= size;
            }
            return 0;
        }
    }
    if (pool_bump + size > WIN_POOL_BYTES) return -1;
    *off_out = (u32)pool_bump;
    pool_bump += size;
    return 0;
}

static void pool_free(u32 off, u32 size) {
    int i, j;
    if (freen >= WIN_MAX + 1) return; /* leak, never corrupt */
    freev[freen].off = off;
    freev[freen].size = size;
    freen++;
    /* Insertion-sort by off, then merge neighbours (n tiny). */
    for (i = freen - 1; i > 0 && freev[i].off < freev[i - 1].off; i--) {
        win_free_t t = freev[i];
        freev[i] = freev[i - 1];
        freev[i - 1] = t;
    }
    for (i = 0; i + 1 < freen;) {
        if (freev[i].off + freev[i].size == freev[i + 1].off) {
            freev[i].size += freev[i + 1].size;
            for (j = i + 1; j + 1 < freen; j++) freev[j] = freev[j + 1];
            freen--;
        } else {
            i++;
        }
    }
}

static win_t *win_get(int id, task64_t *self, int need_owner) {
    if (id < 0 || id >= WIN_MAX || !wins[id].used) return 0;
    if (need_owner && wins[id].owner != self) return 0;
    return &wins[id];
}

long win_create(u32 w, u32 h, const char *title) {
    task64_t *self = task64_self();
    u64 size;
    u32 off;
    int id;
    u64 f;
    long ret;
    if (!self) return -22;
    if (!w || !h || w > WIN_MAX_DIM || h > WIN_MAX_DIM) return -22;
    size = (u64)w * h * 4;
    if (size > WIN_POOL_BYTES) return -12;
    f = s_lock_hold();
    ret = -12;
    for (id = 0; id < WIN_MAX; id++)
        if (!wins[id].used) break;
    if (id < WIN_MAX && pool_alloc((u32)size, &off) == 0) {
        win_t *s = &wins[id];
        s->used = 1;
        /* Cascade default; server SETPOSes exact geometry after. */
        s->x = (u32)(64 + id * 32);
        s->y = (u32)(64 + id * 24);
        s->w = w;
        s->h = h;
        s->off = off;
        s->owner = self;
        for (int i = 0; i < WIN_TITLE; i++) {
            s->title[i] = title[i];
            if (!title[i]) break;
        }
        s->title[WIN_TITLE - 1] = '\0';
        ret = id;
    }
    s_lock_drop(f);
    return ret;
}

long win_close(int id) {
    task64_t *self = task64_self();
    u64 f;
    long ret;
    if (!self) return -22;
    f = s_lock_hold();
    ret = -22;
    {
        win_t *s = win_get(id, self, 1);
        if (s) {
            u32 y0 = s->y, y1 = s->y + s->h;
            pool_free(s->off, s->w * s->h * 4);
            s->used = 0;
            s->owner = 0;
            cons_mark_dirty(y0, y1); /* erase the ghost next present */
            ret = 0;
        }
    }
    s_lock_drop(f);
    return ret;
}

long win_fill(int id, u32 x, u32 y, u32 w, u32 h, u32 rgb) {
    task64_t *self = task64_self();
    u64 f;
    long ret;
    if (!self) return -22;
    f = s_lock_hold();
    ret = -22;
    {
        win_t *s = win_get(id, self, 1);
        if (s) {
            u32 x1 = x + w, y1 = y + h;
            u32 *base = win_pool + s->off / 4;
            u32 yy, xx;
            if (x1 > s->w) x1 = s->w;
            if (y1 > s->h) y1 = s->h;
            for (yy = y; yy < y1; yy++) {
                u32 *dst = base + (u64)yy * s->w;
                for (xx = x; xx < x1; xx++) dst[xx] = rgb;
            }
            if (x < x1 && y < y1)
                cons_mark_dirty(s->y + y, s->y + y1);
            ret = 0;
        }
    }
    s_lock_drop(f);
    return ret;
}

long win_text(int id, u32 x, u32 y, const char *us, u64 len, u32 fg,
              u32 bg) {
    task64_t *self = task64_self();
    u64 f;
    long ret;
    if (!self) return -22;
    if (len > 256) return -22;
    f = s_lock_hold();
    ret = -22;
    {
        win_t *s = win_get(id, self, 1);
        if (s) {
            u32 *base = win_pool + s->off / 4;
            volatile const char *p = (volatile const char *)us;
            u32 i;
            fg &= 0x00FFFFFFUL;
            bg &= 0x00FFFFFFUL;
            for (i = 0; i < (u32)len; i++) {
                unsigned char c = (unsigned char)p[i];
                u32 px = x + (u64)i * 8;
                unsigned char bits_row[16];
                u32 row, col;
                if (px + 8 > s->w) break;
                for (row = 0; row < 16; row++)
                    bits_row[row] = font8x16_data[c][row];
                for (row = 0; row < 16; row++) {
                    u32 yy = y + row;
                    u32 *dst;
                    if (yy >= s->h) break;
                    dst = base + (u64)yy * s->w;
                    for (col = 0; col < 8; col++) {
                        u32 on = (bits_row[row] >> (7 - col)) & 1;
                        dst[px + col] = on ? fg : bg;
                    }
                }
            }
            if (len) cons_mark_dirty(s->y + y, s->y + y + 16);
            ret = 0;
        }
    }
    s_lock_drop(f);
    return ret;
}

long win_setpos(int id, u32 x, u32 y) {
    task64_t *self = task64_self();
    u32 w, h;
    u64 f;
    long ret;
    if (!self) return -22;
    {
        u32 sw = 0, sh = 0;
        cons_dims(&sw, &sh);
        if (!sw) return -19;
        w = sw;
        h = sh;
    }
    f = s_lock_hold();
    ret = -22;
    {
        win_t *s = win_get(id, self, 1);
        if (s) {
            u32 oy0 = s->y, oy1 = s->y + s->h;
            if (s->w <= w)
                s->x = (x > w - s->w) ? w - s->w : x;
            else
                s->x = 0;
            if (s->h <= h)
                s->y = (y > h - s->h) ? h - s->h : y;
            else
                s->y = 0;
            cons_mark_dirty(oy0, oy1);
            cons_mark_dirty(s->y, s->y + s->h);
            ret = 0;
        }
    }
    s_lock_drop(f);
    return ret;
}

/* Close every slot owned by an exiting/execing task (mirrors fb64).
 * No locks held by callers; serial only, never nested. */
void win_owner_release(task64_t *t) {
    u64 f;
    int i;
    if (!t) return;
    f = s_lock_hold();
    for (i = 0; i < WIN_MAX; i++) {
        if (wins[i].used && wins[i].owner == t) {
            u32 y0 = wins[i].y;
            pool_free(wins[i].off, wins[i].w * wins[i].h * 4);
            wins[i].used = 0;
            wins[i].owner = 0;
            cons_mark_dirty(y0, y0 + wins[i].h);
        }
    }
    s_lock_drop(f);
}

/* Blend all slots covering display row y (bottom-to-top), called from
 * cons_present() under its lock — never locks. */
void win_composite_scanline(u32 y, u32 *drow, u32 fb_w) {
    int i;
    for (i = 0; i < WIN_MAX; i++) {
        win_t *s = &wins[i];
        u32 x0, x1, x;
        u32 *src;
        if (!s->used) continue;
        if (y < s->y || y >= s->y + s->h) continue;
        src = win_pool + s->off / 4 + (u64)(y - s->y) * s->w;
        x0 = s->x;
        x1 = s->x + s->w;
        if (x0 >= fb_w) continue;
        if (x1 > fb_w) x1 = fb_w;
        for (x = x0; x < x1; x++) drow[x] = src[x - x0];
    }
}
