/* VGA-1 framebuffer text console: 8x16 glyphs blitted straight to a
 * 32bpp linear framebuffer (UC-mapped by mem64 pass 5, so plain volatile
 * stores are correct). No double buffering yet — minor tearing on scroll
 * is accepted for this milestone (noted in README).
 *
 * Called only under serial_lock (see cons64.h), so no SMP races here. */
#include "cons64.h"
#include "font8x16.h"

#define CONS_FG_DEF 0x00BBBBBBUL /* light gray text */
#define CONS_BG_DEF 0x00000000UL /* black */

static volatile u32 *fb = 0;
static u32 fb_stride = 0; /* u32 pixels per scanline (pitch/4) */
static u32 fb_w = 0, fb_h = 0;
static u32 ncols = 0, nrows = 0;
static u32 cur_x = 0, cur_y = 0;
static u32 fg_color = CONS_FG_DEF;
static int live = 0;

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
            fb[(py + row) * fb_stride + px + col] = on ? fg : bg;
        }
    }
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
            fb[(py + row) * fb_stride + px + col] = CONS_BG_DEF;
        }
    }
    cur_shown = 0;
}

static void cursor_draw(void) {
    u32 px = cur_x * 8, py = cur_y * 16;
    for (u32 row = 0; row < 16; row++) {
        if (py + row >= fb_h) break;
        for (u32 col = 0; col < 8; col++) {
            if (px + col >= fb_w) break;
            fb[(py + row) * fb_stride + px + col] = fg_color;
        }
    }
    cur_dx = cur_x;
    cur_dy = cur_y;
    cur_shown = 1;
}

static void scroll_up(void) {
    /* Move every pixel row up by one glyph row; clear the last band. */
    for (u32 y = 0; y + 16 < fb_h; y++) {
        volatile u32 *dst = fb + (u64)y * fb_stride;
        volatile u32 *src = fb + (u64)(y + 16) * fb_stride;
        for (u32 x = 0; x < fb_w; x++) dst[x] = src[x];
    }
    for (u32 y = fb_h - 16; y < fb_h; y++) {
        volatile u32 *dst = fb + (u64)y * fb_stride;
        for (u32 x = 0; x < fb_w; x++) dst[x] = CONS_BG_DEF;
    }
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
    for (u64 i = 0; i < (u64)fb_h * fb_stride; i++) fb[i] = CONS_BG_DEF;
    cur_shown = 0; /* wipe took down any block with everything else */
    cur_x = 0;
    cur_y = 0;
    cursor_draw();
}

void cons_set_fg(u32 rgb) {
    fg_color = rgb & 0x00FFFFFFUL;
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
                    fb[(py + row) * fb_stride + px + col] =
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
}

int cons_init(u64 addr, u32 pitch, u32 w, u32 h, u32 bpp) {
    /* Validate before touching anything: 32bpp linear only, sane bounded
     * geometry, framebuffer window under 64MB (refuse wild values that
     * would walk off into RAM on a stuck pitch). */
    if (!addr || bpp != 32 || !w || !h || w > 4096 || h > 4096) return 0;
    if (pitch < w * 4 || pitch > 16384) return 0;
    if ((u64)h * pitch > 64ULL * 1024 * 1024) return 0;
    if (!vmm_is_canonical(addr)) return 0;
    fb = (volatile u32 *)addr;
    fb_stride = pitch / 4;
    fb_w = w;
    fb_h = h;
    ncols = w / 8;
    nrows = h / 16;
    if (!ncols || !nrows) return 0;
    fg_color = CONS_FG_DEF;
    live = 1;
    cons_clear();
    return 1;
}
