/* G0 framebuffer console: everything draws into a 4MB static backbuffer
 * (kernel .bss — PMM reserves the whole image to _kernel_end, and the
 * identity map covers it), then cons_present() copies only dirty scanline
 * bands to the UC framebuffer. Per-char present cost is one 16-row band,
 * not a 3MB copy; scrolls flush the text area.
 *
 * Bottom 16px is a persistent status strip (never scrolled): G0 tag, RGB
 * bars, and a tick progress bar redrawn lazily on the next present after
 * cons_status_tick(). Text grid is therefore rows = height/16 - 1.
 *
 * Called only under serial_lock (see cons64.h), so no SMP races here.
 * cons_status_tick() is the exception: a lock-free u32 store from IRQ. */
#include "cons64.h"
#include "font8x16.h"

#define CONS_FG_DEF 0x00BBBBBBUL /* light gray text */
#define CONS_BG_DEF 0x00000000UL /* black */
#define CONS_BB_BYTES (4UL * 1024 * 1024) /* static backbuffer cap */
#define CONS_STRIP_BG 0x00000080UL /* dark blue status strip */

static volatile u32 *fb = 0;
static u32 cons_bb[CONS_BB_BYTES / 4]; /* draw target; flushed by present */
static u32 fb_stride = 0; /* u32 pixels per scanline (pitch/4) */
static u32 fb_w = 0, fb_h = 0;
static u32 ncols = 0, nrows = 0; /* nrows = TEXT rows; strip below them */
static u32 cur_x = 0, cur_y = 0;
static u32 fg_color = CONS_FG_DEF;
static int live = 0;

/* Dirty pixel-row band [dirty_y0, dirty_y1); empty when y0 >= y1. */
static u32 dirty_y0 = 0, dirty_y1 = 0;

static void mark_dirty(u32 y0, u32 y1) {
    if (y0 >= y1) return;
    if (y0 < dirty_y0) dirty_y0 = y0;
    if (y1 > dirty_y1) dirty_y1 = y1;
}

int cons_live(void) {
    return live;
}

static void cell_draw(u32 cx, u32 cy, unsigned char c, u32 fg, u32 bg) {
    u32 px = cx * 8, py = cy * 16;
    for (u32 row = 0; row < 16; row++) {
        if (py + row >= fb_h) break;
        unsigned char bits = font8x16_data[c][row];
        for (u32 col = 0; col < 8; col++) {
            if (px + col >= fb_w) break;
            /* Row bit 7 = leftmost pixel. */
            u32 on = (bits >> (7 - col)) & 1;
            cons_bb[(py + row) * fb_stride + px + col] = on ? fg : bg;
        }
    }
    mark_dirty(py, py + 16);
}

/* The block cursor is just pixels: remember where it was painted so it
 * can be taken down (restored to background) before the cursor moves.
 * The cell underneath is always empty (past-end-of-line or a fresh line
 * start about to be overwritten), so restoring never eats text. Every
 * path erases before moving, hence newline()/scroll_up() never smear
 * and '\r' never strands a block over column 0. */
static u32 cur_dx = 0, cur_dy = 0;
static int cur_shown = 0;

static void cursor_erase(void) {
    u32 px, py;
    if (!cur_shown) return;
    px = cur_dx * 8;
    py = cur_dy * 16;
    for (u32 row = 0; row < 16; row++) {
        if (py + row >= fb_h) break;
        for (u32 col = 0; col < 8; col++) {
            if (px + col >= fb_w) break;
            cons_bb[(py + row) * fb_stride + px + col] = CONS_BG_DEF;
        }
    }
    mark_dirty(py, py + 16);
    cur_shown = 0;
}

static void cursor_draw(void) {
    u32 px = cur_x * 8, py = cur_y * 16;
    for (u32 row = 0; row < 16; row++) {
        if (py + row >= fb_h) break;
        for (u32 col = 0; col < 8; col++) {
            if (px + col >= fb_w) break;
            cons_bb[(py + row) * fb_stride + px + col] = fg_color;
        }
    }
    mark_dirty(py, py + 16);
    cur_dx = cur_x;
    cur_dy = cur_y;
    cur_shown = 1;
}

static void scroll_up(void) {
    /* Text area only: the status strip below nrows*16 never moves. */
    u32 text_h = nrows * 16;
    for (u32 y = 0; y + 16 < text_h; y++) {
        u32 *dst = cons_bb + (u64)y * fb_stride;
        u32 *src = cons_bb + (u64)(y + 16) * fb_stride;
        for (u32 x = 0; x < fb_w; x++) dst[x] = src[x];
    }
    for (u32 y = text_h - 16; y < text_h; y++) {
        u32 *dst = cons_bb + (u64)y * fb_stride;
        for (u32 x = 0; x < fb_w; x++) dst[x] = CONS_BG_DEF;
    }
    mark_dirty(0, text_h);
}

static void newline(void) {
    cur_x = 0;
    cur_y++;
    if (cur_y >= nrows) {
        cur_y = nrows - 1;
        scroll_up();
    }
}

void cons_clear(void) {
    if (!live) return;
    for (u64 i = 0; i < (u64)fb_h * fb_stride; i++) cons_bb[i] = CONS_BG_DEF;
    mark_dirty(0, fb_h);
    cur_shown = 0; /* wipe took down any block with everything else */
    cur_x = 0;
    cur_y = 0;
    cursor_draw();
    cons_present();
}

void cons_set_fg(u32 rgb) {
    fg_color = rgb & 0x00FFFFFFUL;
}

/* ---- G0: status strip + 2D primitives + present ----
 * Primitives draw into the backbuffer and mark dirty WITHOUT presenting,
 * so callers can batch shapes and flush once with cons_present(). The
 * text paths (putc/puts/clear) present internally. */

/* Tick value the strip should reflect; lock-free u32 store from timer. */
static u32 status_want = 0, status_drawn = 0;

void cons_status_tick(u32 t) {
    status_want = t; /* aligned u32 store: atomic on x86, no lock needed */
}

static void fill_rect(u32 x, u32 y, u32 w, u32 h, u32 rgb) {
    u32 x1, y1;
    if (x >= fb_w || y >= fb_h) return;
    x1 = x + w;
    y1 = y + h;
    if (x1 > fb_w) x1 = fb_w;
    if (y1 > fb_h) y1 = fb_h;
    for (u32 yy = y; yy < y1; yy++) {
        u32 *dst = cons_bb + (u64)yy * fb_stride;
        for (u32 xx = x; xx < x1; xx++) dst[xx] = rgb;
    }
    mark_dirty(y, y1);
}

/* Persistent strip redraw: G0 tag, RGB bars, tick progress bar. Runs on
 * the next present after cons_status_tick() (lazy: zero timer-IRQ cost,
 * piggybacks the 1Hz tick log that already holds serial_lock). */
static void status_redraw(void) {
    u32 y0 = nrows * 16;
    u32 prog;
    fill_rect(0, y0, fb_w, 16, CONS_STRIP_BG);
    cell_draw(0, nrows, 'G', 0x00FFFFFFUL, CONS_STRIP_BG);
    cell_draw(1, nrows, '0', 0x00FFFFFFUL, CONS_STRIP_BG);
    fill_rect(64, y0, 64, 16, 0x00FF0000UL);
    fill_rect(128, y0, 64, 16, 0x0000FF00UL);
    fill_rect(192, y0, 64, 16, 0x000000FFUL);
    prog = (status_want % 1000) * 384 / 1000;
    if (prog) fill_rect(288, y0, prog, 16, 0x0000FF00UL);
}

void cons_pixel(u32 x, u32 y, u32 rgb) {
    if (!live) return;
    if (x >= fb_w || y >= fb_h) return;
    cons_bb[(u64)y * fb_stride + x] = rgb;
    mark_dirty(y, y + 1);
}

void cons_fill(u32 x, u32 y, u32 w, u32 h, u32 rgb) {
    if (!live) return;
    fill_rect(x, y, w, h, rgb);
}

void cons_blit(u32 x, u32 y, u32 w, u32 h, const u32 *px) {
    u32 x1, y1;
    if (!live || !px) return;
    if (x >= fb_w || y >= fb_h) return;
    x1 = x + w;
    y1 = y + h;
    if (x1 > fb_w) x1 = fb_w;
    if (y1 > fb_h) y1 = fb_h;
    for (u32 yy = y; yy < y1; yy++) {
        u32 *dst = cons_bb + (u64)yy * fb_stride;
        const u32 *src = px + (u64)(yy - y) * w;
        for (u32 xx = x; xx < x1; xx++) dst[xx] = src[xx - x];
    }
    mark_dirty(y, y1);
}

void cons_dims(u32 *w, u32 *h) {
    if (w) *w = fb_w;
    if (h) *h = fb_h;
}

/* G2 graphics mode: while a userspace mapper owns the screen, present()
 * is a no-op (text keeps accumulating in the backbuffer invisibly).
 * Releasing marks everything dirty, so the next present restores the
 * console + strip + cursor in one flush. Serial-lock discipline throughout
 * (flag only changes under it; present() always runs under it). */
static int gfx_on = 0;

void cons_graphics(int on) {
    if (on) {
        gfx_on = 1;
        return;
    }
    if (!gfx_on) return;
    gfx_on = 0;
    mark_dirty(0, fb_h);
}

/* G1 mouse cursor: 12x16 arrow, bit 11 = leftmost pixel. Composited onto
 * the DISPLAY during present() — never stored in the backbuffer, so text,
 * strip redraws and scrolls can neither corrupt nor smear it. */
static const u16 mouse_sprite[16] = {
    0x800, 0xC00, 0xE00, 0xF00, 0xF80, 0xFC0, 0xFE0, 0xFF0,
    0xFF8, 0xFC0, 0xEC0, 0xC60, 0x060, 0x060, 0x040, 0x000,
};
#define CONS_CURSOR_FG 0x00FFFFFFUL /* white arrow */

void cons_cursor_moved(u32 ox, u32 oy, u32 nx, u32 ny) {
    (void)ox;
    (void)nx;
    /* Rows are copied whole, so only Y bands matter; pad generously. */
    if (oy < fb_h) {
        u32 a = oy > 16 ? oy - 16 : 0, b = oy + 32;
        if (b > fb_h) b = fb_h;
        mark_dirty(a, b);
    }
    if (ny < fb_h) {
        u32 a = ny > 16 ? ny - 16 : 0, b = ny + 32;
        if (b > fb_h) b = fb_h;
        mark_dirty(a, b);
    }
}

void cons_present(void) {
    u32 y0, y1, mx = 0, my = 0;
    int mshow = 0;
    if (!live) return;
    if (gfx_on) return; /* G2: mapper owns the screen; bb still updates */
    if (status_want != status_drawn) {
        status_redraw();
        status_drawn = status_want;
    }
    mouse_cursor_state(&mx, &my, &mshow);
    if (!mshow) {
        mx = 0;
        my = fb_h;
    }
    y0 = dirty_y0;
    y1 = dirty_y1;
    dirty_y0 = fb_h;
    dirty_y1 = 0;
    if (y0 >= y1 || y0 >= fb_h) return;
    if (y1 > fb_h) y1 = fb_h;
    for (u32 y = y0; y < y1; y++) {
        volatile u32 *d = fb + (u64)y * fb_stride;
        u32 *s = cons_bb + (u64)y * fb_stride;
        for (u32 x = 0; x < fb_w; x++) d[x] = s[x];
        if (y >= my && y < my + 16) {
            u16 bits = mouse_sprite[y - my];
            for (u32 i = 0; i < 12; i++) {
                if (bits & (0x800u >> i)) {
                    u32 xx = mx + i;
                    if (xx < fb_w) d[xx] = CONS_CURSOR_FG;
                }
            }
        }
    }
}

void cons_putc(char c) {
    if (!live) return;
    if (c == '\r') {
        /* CR in "\r\n" output must not paint: drawing the cursor at
         * column 0 here would strand a block over the finished line's
         * first glyph (the following '\n'/chars redraw it anyway). */
        cursor_erase();
        cur_x = 0;
        return;
    }
    if (c != '\n' && c != '\b' && c != '\t' && (unsigned char)c < 32)
        return; /* other control chars: ignore (no cursor move) */
    cursor_erase();
    if (c == '\n') {
        newline();
    } else if (c == '\b') {
        if (cur_x > 0) {
            cur_x--;
        } else if (cur_y > 0) {
            cur_y--;
            cur_x = ncols - 1;
        } else {
            cursor_draw();
            cons_present();
            return;
        }
        cell_draw(cur_x, cur_y, ' ', fg_color, CONS_BG_DEF);
    } else if (c == '\t') {
        u32 next = (cur_x + 8) & ~7U;
        while (cur_x < next) {
            if (cur_x >= ncols) break;
            cell_draw(cur_x, cur_y, ' ', fg_color, CONS_BG_DEF);
            cur_x++;
        }
        if (cur_x >= ncols) newline();
    } else if ((unsigned char)c >= 32) {
        cell_draw(cur_x, cur_y, (unsigned char)c, fg_color, CONS_BG_DEF);
        cur_x++;
        if (cur_x >= ncols) newline();
    }
    cursor_draw();
    cons_present();
}

void cons_puts(const char *s) {
    if (!live || !s) return;
    /* Take down any block from earlier cons_putc output; this loop never
     * paints mid-string (single draw at the end), so no erase is needed
     * again inside, and scrolls stay smear-free. */
    cursor_erase();
    for (; *s; s++) {
        /* cons_putc draws the cursor every char; cheaper to inline the
         * loop without per-char cursor, then draw once at the end. */
        char c = *s;
        if (c == '\n') {
            newline();
        } else if (c == '\r') {
            cur_x = 0;
        } else if ((unsigned char)c >= 32) {
            /* Draw glyph without cursor churn (cursor redrawn below). */
            u32 px = cur_x * 8, py = cur_y * 16;
            unsigned char bits_row[16];
            for (u32 i = 0; i < 16; i++) bits_row[i] = font8x16_data[(unsigned char)c][i];
            for (u32 row = 0; row < 16; row++) {
                if (py + row >= fb_h) break;
                for (u32 col = 0; col < 8; col++) {
                    if (px + col >= fb_w) break;
                    u32 on = (bits_row[row] >> (7 - col)) & 1;
                    cons_bb[(py + row) * fb_stride + px + col] =
                        on ? fg_color : CONS_BG_DEF;
                }
            }
            cur_x++;
            if (cur_x >= ncols) newline();
        }
        /* \b \t and other controls intentionally render as nothing here;
         * cons_putc covers them for single-char callers. */
    }
    cursor_draw();
    cons_present();
}

int cons_init(u64 addr, u32 pitch, u32 w, u32 h, u32 bpp) {
    /* Validate before touching anything: 32bpp linear only, sane bounded
     * geometry, framebuffer window under 64MB (refuse wild values that
     * would walk off into RAM on a stuck pitch). The backbuffer is a 4MB
     * static: bigger modes fall back to serial-only. Bottom 16px is the
     * status strip, so at least 2 text rows of height are required. */
    if (!addr || bpp != 32 || !w || !h || w > 4096 || h > 4096) return 0;
    if (pitch < w * 4 || pitch > 16384) return 0;
    if ((u64)h * pitch > 64ULL * 1024 * 1024) return 0;
    if ((u64)h * pitch > sizeof(cons_bb)) return 0;
    if (!vmm_is_canonical(addr)) return 0;
    fb = (volatile u32 *)addr;
    fb_stride = pitch / 4;
    fb_w = w;
    fb_h = h;
    ncols = w / 8;
    nrows = h / 16;
    if (ncols < 1 || nrows < 2) return 0;
    nrows--; /* last 16px: persistent status strip, never scrolled */
    fg_color = CONS_FG_DEF;
    live = 1;
    status_drawn = 0xFFFFFFFF; /* force strip paint on first present */
    cons_clear();
    return 1;
}
