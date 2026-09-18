#ifndef CONS64_H
#define CONS64_H

#include "cpu64.h"

/* VGA-1 framebuffer text console (32bpp linear framebuffer only).
 * Returns 1 when live, 0 when the mode is unusable (caller falls back to
 * serial-only). All output clipped to the mode geometry; grid derived as
 * cols = width/8, rows = height/16 (8x16 font).
 *
 * LOCKING: no locks inside — callers (s_write/s_printf paths) already hold
 * serial_lock, which serializes all CPUs. May be called with IF=0.
 * Exception: cons_status_tick() is a lock-free store, safe from timer IRQ.
 *
 * G0 2D: cons_pixel/fill/blit draw into the backbuffer WITHOUT presenting
 * (batch, then cons_present once). Text paths present internally. Bottom
 * 16px is a persistent status strip outside the text grid. */
int cons_init(u64 addr, u32 pitch, u32 w, u32 h, u32 bpp);
int cons_live(void);
void cons_clear(void);
void cons_putc(char c);
void cons_puts(const char *s);
void cons_set_fg(u32 rgb);
void cons_pixel(u32 x, u32 y, u32 rgb);
void cons_fill(u32 x, u32 y, u32 w, u32 h, u32 rgb);
void cons_blit(u32 x, u32 y, u32 w, u32 h, const u32 *px);
void cons_dims(u32 *w, u32 *h);
void cons_present(void);
void cons_status_tick(u32 t);

#endif
