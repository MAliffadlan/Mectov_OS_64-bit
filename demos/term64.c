/* D2 term: terminal as a separate GUI process. Owns one window slot,
 * renders its text grid via WIN_TEXT, reads keys via WIN_GETEVENT
 * (kernel routes by focus). Prints TERM-READY/TERM-LINE to serial for
 * the gate. `exit` closes the slot and exits (the server then winds the
 * whole GUI session down).
 *
 * Runs via shell `run term` (or spawned by winsrv), hence args_main. */
#include "sys64.h"
#include "umalloc.h"

#define T_W 640
#define T_H 400
#define T_BORDER 2
#define T_TITLE 20

#define C_FRAME 0x00C0C0C0U
#define C_TITLE 0x00000080U
#define C_CLIENT 0x00000000U
#define C_TEXT 0x00BBBBBBU
#define C_TTITLE 0x00FFFFFFU

static long wid = -1;
static int tcols, trows;
static char *tbuf;
static int cur_row;
static char iline[80];
static int iline_n;

static void wfill(int x, int y, int w, int h, u32 rgb) {
    d_winfill(wid, (u64)x, (u64)y, (u64)w, (u64)h, (u64)rgb);
}

static void wtext(int x, int y, const char *s, u32 fg, u32 bg) {
    d_wintext(wid, (u64)x, (u64)y, s, d_strlen(s), (u64)fg, (u64)bg);
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

static void draw_all(void) {
    int r;
    wfill(0, 0, T_W, T_TITLE, C_TITLE);
    wfill(0, T_TITLE - 2, T_W, 2, C_FRAME);
    wfill(0, 0, T_BORDER, T_H, C_FRAME);
    wfill(T_W - T_BORDER, 0, T_BORDER, T_H, C_FRAME);
    wfill(0, T_H - T_BORDER, T_W, T_BORDER, C_FRAME);
    wfill(T_BORDER, T_TITLE, T_W - T_BORDER * 2, T_H - T_TITLE - T_BORDER,
          C_CLIENT);
    wtext(6, 2, "MCT TERM", C_TTITLE, C_TITLE);
    for (r = 0; r < trows; r++) {
        const char *row = &tbuf[r * 80];
        if (row[0]) wtext(T_BORDER, T_TITLE + r * 16, row, C_TEXT, C_CLIENT);
    }
    if (iline_n > 0) {
        iline[iline_n] = 0;
        wtext(T_BORDER, T_TITLE + cur_row * 16, iline, C_TEXT, C_CLIENT);
    }
    if (cur_row < trows)
        wfill(T_BORDER + iline_n * 8, T_TITLE + cur_row * 16, 8, 16, C_TEXT);
    d_winpresent();
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
    dl_s(l, "TERM-LINE ");
    dl_s(l, line);
    dl_nl(l);
}

void args_main(int argc, const char **argv) {
    DLINE(l);
    winev_t ev[8];
    int running = 1;
    (void)argc;
    (void)argv;
    wid = d_wincreate(T_W, T_H, "MCT TERM");
    if (wid < 0) {
        dl_s(&l, "TERM-NOSLOT");
        dl_nl(&l);
        return;
    }
    tcols = (T_W - T_BORDER * 2) / 8;
    trows = (T_H - T_TITLE - T_BORDER) / 16;
    tbuf = (char *)umalloc((u64)trows * 80);
    if (!tbuf) {
        dl_s(&l, "TERM-ALLOC-FAIL");
        dl_nl(&l);
        d_winclose(wid);
        return;
    }
    for (int i = 0; i < trows * 80; i++) tbuf[i] = 0;
    cur_row = 0;
    iline_n = 0;
    term_put("MCT terminal - type help");
    draw_all();
    dl_s(&l, "TERM-READY id=");
    dl_u(&l, (u64)wid);
    dl_nl(&l);
    while (running) {
        int changed = 0;
        long n = d_wingetevent(wid, ev, 8);
        for (long i = 0; i < n; i++) {
            long k;
            if (ev[i].type != WEV_KEY) continue;
            k = (long)ev[i].d0;
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
        if (changed) draw_all();
        if (running) d_sleep(2);
    }
    d_winclose(wid);
    dl_s(&l, "TERM-EXIT");
    dl_nl(&l);
}

/* Plain spawn has no display contract; point at args_main usage. */
void demo_main(void) {
    DLINE(l);
    dl_s(&l, "term: use `run term`");
    dl_nl(&l);
}
