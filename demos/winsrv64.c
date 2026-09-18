/* G3 winsrv: minimal single-window server. Background + one draggable
 * terminal window (title bar, client text grid from the shared 8x16 font,
 * line echo with clear/help/exit) + arrow cursor, all direct to the mapped
 * framebuffer. Full redraw on input change only.
 *
 * Runs via shell `run winsrv`, hence args_main. Serial protocol for the
 * gate: WIN-READY, WIN-LINE <text>, WIN-DRAG x,y, WIN-EXIT. */
#include "sys64.h"
#include "umalloc.h"
#include "../k64/font8x16.c"

typedef unsigned short u16;

#define SCR_W 1024
#define SCR_H 768
#define WIN_W 640
#define WIN_H 400
#define TITLE_H 20
#define BORDER 2

#define C_BG 0x001A2B3CU
#define C_FRAME 0x00C0C0C0U
#define C_TITLE 0x00000080U
#define C_CLIENT 0x00000000U
#define C_TEXT 0x00BBBBBBU
#define C_TTITLE 0x00FFFFFFU
#define C_CURSOR 0x00FFFFFFU

static volatile u32 *g_fb;
static u32 g_stride;
static int win_x = 192, win_y = 150;
static int dragging = 0, grab_x = 0, grab_y = 0;

static int tcols, trows;
static char *tbuf; /* trows x 80 text buffer */
static int cur_row;
static char iline[80];
static int iline_n;

static const u16 arrow[16] = {
    0x800, 0xC00, 0xE00, 0xF00, 0xF80, 0xFC0, 0xFE0, 0xFF0,
    0xFF8, 0xFC0, 0xEC0, 0xC60, 0x060, 0x060, 0x040, 0x000,
};

static void fill(int x, int y, int w, int h, u32 rgb) {
    int x1 = x + w, y1 = y + h, xx, yy;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x1 > SCR_W) x1 = SCR_W;
    if (y1 > SCR_H) y1 = SCR_H;
    for (yy = y; yy < y1; yy++)
        for (xx = x; xx < x1; xx++)
            g_fb[(u64)yy * g_stride + (u64)xx] = rgb;
}

static void glyph(int x, int y, unsigned char c, u32 fg, u32 bg) {
    int row, col;
    for (row = 0; row < 16; row++) {
        int yy = y + row;
        unsigned char bits;
        if (yy < 0 || yy >= SCR_H) continue;
        bits = font8x16_data[c][row];
        for (col = 0; col < 8; col++) {
            int xx = x + col;
            unsigned on;
            if (xx < 0 || xx >= SCR_W) continue;
            on = (bits >> (7 - col)) & 1;
            g_fb[(u64)yy * g_stride + (u64)xx] = on ? fg : bg;
        }
    }
}

static void text(int x, int y, const char *s, u32 fg, u32 bg) {
    while (*s) {
        glyph(x, y, (unsigned char)*s, fg, bg);
        x += 8;
        s++;
    }
}

static void term_put(const char *s) {
    int i;
    if (cur_row >= trows) {
        for (i = 0; i < trows - 1; i++)
            for (int c = 0; c < 80; c++)
                tbuf[i * 80 + c] = tbuf[(i + 1) * 80 + c];
        for (int c = 0; c < 80; c++) tbuf[(trows - 1) * 80 + c] = 0;
        cur_row = trows - 1;
    }
    for (i = 0; i < 79 && s[i]; i++) tbuf[cur_row * 80 + i] = s[i];
    tbuf[cur_row * 80 + i] = 0;
    cur_row++;
}

static void draw_all(u32 mx, u32 my) {
    int r, c, cx0 = win_x + BORDER, cy0 = win_y + TITLE_H;
    fill(0, 0, SCR_W, SCR_H, C_BG);
    /* Frame + title + client. */
    fill(win_x, win_y, WIN_W, TITLE_H, C_TITLE);
    fill(win_x, win_y + TITLE_H - 2, WIN_W, 2, C_FRAME);
    fill(win_x, win_y, BORDER, WIN_H, C_FRAME);
    fill(win_x + WIN_W - BORDER, win_y, BORDER, WIN_H, C_FRAME);
    fill(win_x, win_y + WIN_H - BORDER, WIN_W, BORDER, C_FRAME);
    fill(cx0, cy0, WIN_W - BORDER * 2, WIN_H - TITLE_H - BORDER, C_CLIENT);
    text(win_x + 6, win_y + 2, "MCT TERM", C_TTITLE, C_TITLE);
    /* Terminal rows + input line + cursor block. */
    for (r = 0; r < trows; r++) {
        for (c = 0; c < tcols; c++) {
            char ch = tbuf[r * 80 + c];
            if (ch) glyph(cx0 + c * 8, cy0 + r * 16, (unsigned char)ch,
                           C_TEXT, C_CLIENT);
        }
    }
    for (c = 0; c < iline_n && c < tcols; c++)
        glyph(cx0 + c * 8, cy0 + cur_row * 16, (unsigned char)iline[c],
              C_TEXT, C_CLIENT);
    if (cur_row < trows)
        fill(cx0 + iline_n * 8, cy0 + cur_row * 16, 8, 16, C_TEXT);
    /* Arrow cursor (transparent elsewhere). */
    for (r = 0; r < 16; r++) {
        int yy = (int)my + r;
        u16 bits;
        if (yy < 0 || yy >= SCR_H) continue;
        bits = arrow[r];
        for (c = 0; c < 12; c++) {
            int xx = (int)mx + c;
            if ((bits & (0x800u >> c)) && xx >= 0 && xx < SCR_W)
                g_fb[(u64)yy * g_stride + (u64)xx] = C_CURSOR;
        }
    }
}

static void submit(dline_t *l, const char *line) {
    if (line[0] == 'c' && line[1] == 'l' && line[2] == 'e' &&
        line[3] == 'a' && line[4] == 'r' && !line[5]) {
        for (int i = 0; i < trows * 80; i++) tbuf[i] = 0;
        cur_row = 0;
        return;
    }
    if (line[0] == 'h' && line[1] == 'e' && line[2] == 'l' &&
        line[3] == 'p' && !line[4]) {
        term_put("cmds: clear exit");
        term_put("type + Enter echoes");
        return;
    }
    term_put(line);
    dl_s(l, "WIN-LINE ");
    dl_s(l, line);
    dl_nl(l);
}

void args_main(int argc, const char **argv) {
    DLINE(l);
    fbinfo_t fi;
    long v;
    u32 mx = 512, my = 383, lseq = 0;
    int running = 1;
    (void)argc;
    (void)argv;
    if (d_fbinfo(&fi) || fi.w != SCR_W || fi.h != SCR_H || fi.bpp != 32) {
        dl_s(&l, "WIN-NOFB");
        dl_nl(&l);
        return;
    }
    if (d_fbmap() != (long)fi.map_va) {
        dl_s(&l, "WIN-MAP-FAIL");
        dl_nl(&l);
        return;
    }
    tcols = (WIN_W - BORDER * 2) / 8;
    trows = (WIN_H - TITLE_H - BORDER) / 16;
    tbuf = (char *)umalloc((u64)trows * 80);
    if (!tbuf) {
        dl_s(&l, "WIN-ALLOC-FAIL");
        dl_nl(&l);
        d_fbunmap();
        return;
    }
    for (int i = 0; i < trows * 80; i++) tbuf[i] = 0;
    cur_row = 0;
    iline_n = 0;
    g_fb = (volatile u32 *)fi.map_va;
    g_stride = (u32)(fi.pitch / 4);
    term_put("MCT terminal - type help");
    draw_all(mx, my);
    u64 last_draw = (u64)d_ticks();
    int pending = 0;
    dl_s(&l, "WIN-READY ");
    dl_u(&l, fi.w);
    dl_s(&l, "x");
    dl_u(&l, fi.h);
    dl_nl(&l);
    while (running) {
        int changed = 0;
        long k;
        v = d_getmouse();
        {
            u32 nx = (u64)v & 0xFFF, ny = ((u64)v >> 12) & 0xFFF;
            u32 nb = ((u64)v >> 24) & 7, sq = (u64)v >> 32;
            if (sq != lseq) {
                lseq = sq;
                if (nb & 1) {
                    if (!dragging && (int)nx >= win_x &&
                        (int)nx < win_x + WIN_W && (int)ny >= win_y &&
                        (int)ny < win_y + TITLE_H) {
                        dragging = 1;
                        grab_x = (int)nx - win_x;
                        grab_y = (int)ny - win_y;
                    }
                    if (dragging) {
                        win_x = (int)nx - grab_x;
                        win_y = (int)ny - grab_y;
                        if (win_x < 0) win_x = 0;
                        if (win_y < 0) win_y = 0;
                        if (win_x > SCR_W - WIN_W) win_x = SCR_W - WIN_W;
                        if (win_y > SCR_H - WIN_H) win_y = SCR_H - WIN_H;
                    }
                } else if (dragging) {
                    dragging = 0;
                    dl_s(&l, "WIN-DRAG ");
                    dl_u(&l, (u64)win_x);
                    dl_s(&l, ",");
                    dl_u(&l, (u64)win_y);
                    dl_nl(&l);
                }
                mx = nx;
                my = ny;
                changed = 1;
            }
        }
        while ((k = d_getchar()) >= 0) {
            changed = 1;
            if (k == '\n') {
                iline[iline_n] = 0;
                if (iline_n == 4 && iline[0] == 'e' && iline[1] == 'x' &&
                    iline[2] == 'i' && iline[3] == 't') {
                    running = 0;
                    break;
                }
                submit(&l, iline);
                iline_n = 0;
            } else if (k == '\b') {
                if (iline_n > 0) iline_n--;
            } else if (iline_n < 79 && k >= 32 && k < 127) {
                iline[iline_n++] = (char)k;
            }
        }
        if (changed || pending) {
            /* G4 pacing: full redraws at most every 50ms (a drag storm
             * of packets must not spiral into back-to-back 3MB UC
             * copies); pending input still flushes within one quantum. */
            u64 now = (u64)d_ticks();
            if (now - last_draw >= 5) {
                draw_all(mx, my);
                last_draw = now;
                pending = 0;
            } else if (changed) {
                pending = 1;
            }
        }
        if (running) d_sleep(2);
    }
    d_fbunmap();
    dl_s(&l, "WIN-EXIT");
    dl_nl(&l);
}

/* Plain spawn has no display contract; point at args_main usage. */
void demo_main(void) {
    DLINE(l);
    dl_s(&l, "winsrv: use `run winsrv`");
    dl_nl(&l);
}
