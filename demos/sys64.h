/* M4 Ring-3 syscall stubs (M4 ABI0 over int $0x80).
 * Freestanding: own types, no libc. The kernel preserves all registers
 * except RAX (result), so only "memory" needs clobbering. */
#ifndef SYS64_H
#define SYS64_H

typedef unsigned long u64;
typedef unsigned int u32;

static inline long sys0(long n) {
    long r;
    __asm__ __volatile__("int $0x80" : "=a"(r) : "a"(n) : "memory");
    return r;
}
static inline long sys1(long n, u64 a) {
    long r;
    __asm__ __volatile__("int $0x80" : "=a"(r) : "a"(n), "b"(a) : "memory");
    return r;
}
static inline long sys2(long n, u64 a, u64 b) {
    long r;
    __asm__ __volatile__("int $0x80"
                         : "=a"(r)
                         : "a"(n), "b"(a), "c"(b)
                         : "memory");
    return r;
}
static inline long sys3(long n, u64 a, u64 b, u64 c) {
    long r;
    __asm__ __volatile__("int $0x80"
                         : "=a"(r)
                         : "a"(n), "b"(a), "c"(b), "d"(c)
                         : "memory");
    return r;
}
/* D1 wide calls: RSI/RDI/R8/R9 via register variables (kernel ABI0
 * passes args in RBX,RCX,RDX,RSI,RDI,R8,R9; all but RAX preserved). */
static inline long sys4(long n, u64 a, u64 b, u64 c, u64 d) {
    long r;
    __asm__ __volatile__("int $0x80"
                         : "=a"(r)
                         : "a"(n), "b"(a), "c"(b), "d"(c), "S"(d)
                         : "memory");
    return r;
}
static inline long sys5(long n, u64 a, u64 b, u64 c, u64 d, u64 e) {
    long r;
    __asm__ __volatile__("int $0x80"
                         : "=a"(r)
                         : "a"(n), "b"(a), "c"(b), "d"(c), "S"(d), "D"(e)
                         : "memory");
    return r;
}
static inline long sys6(long n, u64 a, u64 b, u64 c, u64 d, u64 e, u64 f) {
    register u64 r8 __asm__("r8") = f;
    long r;
    __asm__ __volatile__("int $0x80"
                         : "=a"(r)
                         : "a"(n), "b"(a), "c"(b), "d"(c), "S"(d), "D"(e),
                           "r"(r8)
                         : "memory");
    return r;
}
static inline long sys7(long n, u64 a, u64 b, u64 c, u64 d, u64 e, u64 f,
                        u64 g) {
    register u64 r8 __asm__("r8") = f;
    register u64 r9 __asm__("r9") = g;
    long r;
    __asm__ __volatile__("int $0x80"
                         : "=a"(r)
                         : "a"(n), "b"(a), "c"(b), "d"(c), "S"(d), "D"(e),
                           "r"(r8), "r"(r9)
                         : "memory");
    return r;
}

static unsigned d_strlen(const char *s) __attribute__((unused));
static unsigned d_strlen(const char *s) {
    unsigned n = 0;
    while (s[n]) n++;
    return n;
}
static inline void d_puts(const char *s) {
    sys2(1, (u64)s, d_strlen(s));
}
static void d_putu(u64 v) __attribute__((unused));
static void d_putu(u64 v) {
    char buf[21];
    int n = 0;
    if (!v) { d_puts("0"); return; }
    while (v && n < 20) { buf[n++] = (char)('0' + v % 10); v /= 10; }
    while (n > 0) { char c = buf[--n]; sys2(1, (u64)&c, 1); }
}
/* Fixed 6-fraction printing for exact-value checks (no float printf). */
static void d_putd(double d) __attribute__((unused));
static void d_putd(double d) {
    if (d < 0) { d_puts("-"); d = -d; }
    unsigned long long ip = (unsigned long long)d;
    d_putu(ip);
    d_puts(".");
    unsigned long long frac = (unsigned long long)((d - (double)ip) * 1000000.0);
    /* MSD-first via divisor (exact for our 6-digit expectations). */
    unsigned long long div = 100000;
    for (int i = 0; i < 6; i++) {
        char c = (char)('0' + (frac / div) % 10);
        sys2(1, (u64)&c, 1);
        div /= 10;
    }
}

static inline long d_pid(void) { return sys0(20); }
static inline long d_ticks(void) { return sys0(8); }
static inline long d_yield(void) { return sys0(9); }
static inline long d_sleep(u64 t) { return sys1(19, t); }
static inline long d_exec(const char *name) { return sys1(76, (u64)name); }
/* waitpid(pid, &status, wnohang) -> reaped pid / 0 / negative errno. */
static inline long d_wait(long pid, long *st, long wnohang) {
    return sys3(72, (u64)pid, (u64)st, (u64)wnohang);
}

/* M7.2 extensions (mirror k64/cpu64.h numbers/layouts). */
typedef struct {
    int id, state, parent, cpu;
    char name[16];
} ps_entry_t;
typedef struct {
    u64 total_frames, free_frames;
} meminfo_t;
static inline long d_meminfo(meminfo_t *m) { return sys1(131, (u64)m); }
static inline long d_getcpu(void) { return sys0(133); }
static inline long d_getbase(void) { return sys0(132); }
static inline long d_brk(u64 nw) { return sys1(120, nw); }
static inline long d_ps(ps_entry_t *b, long max) {
    return sys2(134, (u64)b, (u64)max);
}
static inline long d_getchar(void) { return sys0(135); }
static inline long d_getmouse(void) { return sys0(137); }
/* G2 display (mirror k64/cpu64.h layouts/numbers). */
typedef struct {
    u64 addr, pitch, w, h, bpp, size, map_va;
} fbinfo_t;
static inline long d_fbinfo(fbinfo_t *f) { return sys1(138, (u64)f); }
static inline long d_fbmap(void) { return sys0(139); }
static inline long d_fbunmap(void) { return sys0(140); }
/* D1 window slots (mirror k64/cpu64.h numbers). */
static inline long d_wincreate(u64 w, u64 h, const char *t) {
    return sys3(141, w, h, (u64)t);
}
static inline long d_winclose(long id) { return sys1(142, (u64)id); }
static inline long d_winfill(long id, u64 x, u64 y, u64 w, u64 h, u64 rgb) {
    return sys6(143, (u64)id, x, y, w, h, rgb);
}
static inline long d_wintext(long id, u64 x, u64 y, const char *s, u64 len,
                             u64 fg, u64 bg) {
    return sys7(144, (u64)id, x, y, (u64)s, len, fg, bg);
}
static inline long d_winsetpos(long id, u64 x, u64 y) {
    return sys3(145, (u64)id, x, y);
}
static inline long d_winpresent(void) { return sys0(146); }
/* D2 events + listing (mirror k64/cpu64.h layouts/numbers). */
typedef struct {
    u32 type, d0, d1, d2;
} winev_t;
#define WEV_KEY 1
#define WEV_MOVE 2
#define WEV_BTN 3
#define WEV_ENTER 4
#define WEV_LEAVE 5
#define WEV_FOCUS 6
typedef struct {
    u32 id, x, y, w, h;
    int owner;
    char title[16];
} wininfo_t;
static inline long d_wingetevent(long id, winev_t *e, long max) {
    return sys3(147, (u64)id, (u64)e, (u64)max);
}
static inline long d_winfocus(long id) { return sys1(148, (u64)id); }
static inline long d_winlist(wininfo_t *w, long max) {
    return sys2(149, (u64)w, (u64)max);
}
static inline long d_winraise(long id) { return sys1(150, (u64)id); }
static inline long d_reboot(void) { return sys0(151); }
static inline long d_spawn(const char *n, long argc, const char **argv) {
    return sys3(136, (u64)n, (u64)argc, (u64)argv);
}

/* Line-buffered emit: a full line goes out in ONE syscall, so concurrent
 * tasks on SMP can interleave lines but never shatter them (each PRINT is
 * kernel-atomic, but a line built from N PRINTs is not).
 *
 * The buffer MUST live on the caller's stack (DLINE): clone workers share
 * all globals, so a shared static buffer corrupts across threads. Stacks
 * are always private per task (fresh pages even for clones). */
typedef struct {
    char b[224];
    int n;
} dline_t;
#define DLINE(name) dline_t name = { { 0 }, 0 }
static inline void dl_flush(dline_t *l) {
    if (l->n > 0) {
        sys2(1, (u64)l->b, (u64)l->n);
        l->n = 0;
    }
}
static inline void dl_s(dline_t *l, const char *s) {
    for (; *s; s++) {
        if (l->n >= (int)sizeof(l->b)) dl_flush(l);
        l->b[l->n++] = *s;
    }
}
static inline void dl_u(dline_t *l, u64 v) {
    char tmp[21];
    int n = 0;
    if (!v) { dl_s(l, "0"); return; }
    while (v && n < 20) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
    while (n > 0) {
        if (l->n >= (int)sizeof(l->b)) dl_flush(l);
        l->b[l->n++] = tmp[--n];
    }
}
static inline void dl_nl(dline_t *l) {
    if (l->n >= (int)sizeof(l->b)) dl_flush(l);
    l->b[l->n++] = '\n';
    dl_flush(l);
}

#endif
