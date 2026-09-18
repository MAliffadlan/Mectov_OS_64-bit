/* G2 bump allocator over d_brk (no free — fragmentation-free by design).
 * 16-byte aligned; returns 0 on OOM. Single-threaded use only (clone
 * workers share globals, like the DLINE rule in sys64.h). */
#ifndef UMALLOC64_H
#define UMALLOC64_H

#include "sys64.h"

static u64 uheap_cur = 0, uheap_end = 0;

static inline void *umalloc(u64 n) {
    u64 want, nb;
    if (n == 0) n = 1;
    n = (n + 15) & ~15ULL;
    if (!uheap_cur) {
        uheap_cur = uheap_end = (u64)d_brk(0);
        if (!uheap_cur) return 0;
    }
    if (uheap_cur + n < uheap_cur) return 0; /* wrap */
    if (uheap_cur + n > uheap_end) {
        want = (uheap_cur + n + 4095) & ~4095ULL;
        nb = (u64)d_brk(want);
        if ((long)nb < 0 || nb < want) return 0;
        uheap_end = nb;
    }
    want = uheap_cur;
    uheap_cur += n;
    return (void *)want;
}

#endif
