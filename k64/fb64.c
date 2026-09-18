/* G2 display ownership + mapping (single-mapper model: one task owns the
 * screen; mapping it flips the console to graphics mode, releasing
 * restores text instantly). Device frames are NEVER refcounted and NEVER
 * COW (see the clone_level carve-out in mem64.c): MAP_SHARED semantics,
 * and teardown puts on ref-0 are harmless no-ops.
 *
 * LOCKING: owner flag + table work under mem_lock; the graphics-mode flip
 * + restore present under serial_lock. The two are NEVER nested — and the
 * shootdown-after-release rule from mem64.c applies to unmap.
 */
#include "cpu64.h"
#include "cons64.h"

/* 256GB: clear of identity RAM (<32GB), demos, ustacks, ELF ASLR (<4GB);
 * fresh PDPT slot, canonical low-half. */
#define FB_USER_BASE 0x4000000000ULL

static task64_t *fb_owner = 0;

static u64 fb_size(void) { return (u64)g_fb_h * g_fb_pitch; }

long fb_info(fbinfo_t *out) {
    volatile fbinfo_t *o;
    if (!g_fb_addr || !g_fb_w || !g_fb_h) return -19; /* ENODEV */
    o = (volatile fbinfo_t *)out;
    o->addr = g_fb_addr;
    o->pitch = g_fb_pitch;
    o->w = g_fb_w;
    o->h = g_fb_h;
    o->bpp = g_fb_bpp;
    o->size = fb_size();
    o->map_va = FB_USER_BASE;
    return 0;
}

long fb_map_current(void) {
    task64_t *self = task64_self();
    u64 size, npg, i, flags, mf;
    u64 *saved;
    if (!self) return -22;
    if (!g_fb_addr || !g_fb_w || !g_fb_h) return -19;
    mf = mem_lock_acquire();
    if (fb_owner && fb_owner != self) {
        mem_lock_release(mf);
        return -16; /* EBUSY: single mapper */
    }
    if (fb_owner == self) {
        mem_lock_release(mf);
        return (long)FB_USER_BASE; /* idempotent */
    }
    size = fb_size();
    npg = (size + 4095) / 4096;
    flags = VMM_RW | VMM_US | VMM_UC;
    if (cpu_nx_enabled()) flags |= VMM_NX;
    saved = vmm_get_root();
    vmm_set_root((u64 *)(cpu_read_cr3() & ~0xFFFULL));
    for (i = 0; i < npg; i++) {
        if (__vmm_map_page(FB_USER_BASE + i * 4096,
                           (g_fb_addr & ~0xFFFULL) + i * 4096, flags)) {
            while (i-- > 0) __vmm_unmap_page(FB_USER_BASE + i * 4096);
            vmm_set_root(saved);
            mem_lock_release(mf);
            return -12; /* ENOMEM */
        }
    }
    vmm_set_root(saved);
    fb_owner = self;
    mem_lock_release(mf);
    mf = s_lock_hold();
    cons_graphics(1);
    s_lock_drop(mf);
    return (long)FB_USER_BASE;
}

long fb_unmap_current(void) {
    task64_t *self = task64_self();
    u64 npg, i, mf;
    u64 *saved;
    if (!self) return -22;
    mf = mem_lock_acquire();
    if (fb_owner != self) {
        mem_lock_release(mf);
        return -22;
    }
    npg = (fb_size() + 4095) / 4096;
    saved = vmm_get_root();
    vmm_set_root((u64 *)(cpu_read_cr3() & ~0xFFFULL));
    for (i = 0; i < npg; i++) __vmm_unmap_page(FB_USER_BASE + i * 4096);
    vmm_set_root(saved);
    fb_owner = 0;
    mem_lock_release(mf);
    vmm_publish(); /* removals need the flush, no lock held */
    mf = s_lock_hold();
    cons_graphics(0);
    cons_present(); /* instant text restore */
    s_lock_drop(mf);
    return 0;
}

/* Owner exits/execs: yield the screen now. The mapping itself stays until
 * reap teardown (a zombie never runs, and teardown puts are no-ops). */
void fb_owner_release(task64_t *t) {
    int mine;
    u64 mf;
    if (!t) return;
    mf = mem_lock_acquire();
    mine = (fb_owner == t);
    if (mine) fb_owner = 0;
    mem_lock_release(mf);
    if (!mine) return;
    mf = s_lock_hold();
    cons_graphics(0);
    cons_present();
    s_lock_drop(mf);
}
