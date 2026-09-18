/* G3 winsrv: minimal single-window server, now on D1 kernel window slots.
 * Background + one draggable terminal window + static taskbar with clock;
 * the kernel composites, routes nothing yet (D2) — input still polled
 * directly here. Full slot redraw on change, flushed via WIN_PRESENT,
 * paced at 50ms (G4).
 *
 * Runs via shell `run winsrv`, hence args_main. Serial protocol for the
 * gate: WIN-READY, WIN-LINE <text>, WIN-DRAG x,y, WIN-EXIT. */
#include "sys64.h"
#include "umalloc.h"

#define SCR_W 1024
#define SCR_H 768
#define WIN_W 640
#define WIN_H 400
#define TITLE_H 20
#define BORDER 2
#define BAR_H 28

#define C_BG 0x001A2B3CU
#define C_FRAME 0x00C0C0C0U
#define C_TITLE 0x00000080U
#define C_CLIENT 0x00000000U
#define C_TEXT 0x00BBBBBBU
#define C_TTITLE 0x00FFFFFFU

static long id_bg = -1, id_term = -1, id_bar = -1;
static int win_x = 192, win_y = 150;
static int dragging = 0, grab_x = 0, grab_y = 0;

static int tcols, trows;
static char *tbuf; /* trows x 80 text buffer */
static int cur_row;
static char iline[80];
static int iline_n;
static u64 last_sec = (u64)-1;

static void wfill(long id, int x, int y, int w, int h, u32 rgb) {
    d_winfill(id, (u64)x, (u64)y, (u64)w, (u64)h, (u64)rgb);
}

static void wtext(long id, int x, int y, const char *s, u32 fg, u32 bg) {
    d_wintext(id, (u64)x, (u64)y, s, d_strlen(s), (u64)fg, (u64)bg);
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

static void draw_bg(void) { wfill(id_bg, 0, 0, SCR_W, SCR_H, C_BG); }

static void draw_term(void) {
    int r;
    wfill(id_term, 0, 0, WIN_W, TITLE_H, C_TITLE);
    wfill(id_term, 0, TITLE_H - 2, WIN_W, 2, C_FRAME);
    wfill(id_term, 0, 0, BORDER, WIN_H, C_FRAME);
    wfill(id_term, WIN_W - BORDER, 0, BORDER, WIN_H, C_FRAME);
    wfill(id_term, 0, WIN_H - BORDER, WIN_W, BORDER, C_FRAME);
    wfill(id_term, BORDER, TITLE_H, WIN_W - BORDER * 2,
          WIN_H - TITLE_H - BORDER, C_CLIENT);
    wtext(id_term, 6, 2, "MCT TERM", C_TTITLE, C_TITLE);
    for (r = 0; r < trows; r++) {
        const char *row = &tbuf[r * 80];
        if (row[0])
            wtext(id_term, BORDER, TITLE_H + r * 16, row, C_TEXT, C_CLIENT);
    }
    if (iline_n > 0 && cur_row < trows) {
        iline[iline_n] = 0;
        wtext(id_term, BORDER, TITLE_H + cur_row * 16, iline, C_TEXT,
              C_CLIENT);
    }
    if (cur_row < trows)
        wfill(id_term, BORDER + iline_n * 8, TITLE_H + cur_row * 16, 8, 16,
              C_TEXT);
}

static void draw_bar(void) {
    char clk[8];
    u64 secs = (u64)d_ticks() / 100;
    clk[0] = 'M';
    clk[1] = 'C';
    clk[2] = 'T';
    clk[3] = ' ';
    clk[4] = (char)('0' + (secs / 600) % 6);
    clk[5] = (char)('0' + (secs / 60) % 10);
    clk[6] = ':';
    clk[7] = '\0';
    wfill(id_bar, 0, 0, SCR_W, BAR_H, C_TITLE);
    wtext(id_bar, 8, 6, clk, C_TTITLE, C_TITLE);
    {
        char ss[3];
        ss[0] = (char)('0' + (secs / 10) % 6);
        ss[1] = (char)('0' + secs % 10);
        ss[2] = '\0';
        wtext(id_bar, 8 + 7 * 8, 6, ss, C_TTITLE, C_TITLE);
    }
    last_sec = secs;
}

static void draw_all(void) {
    draw_bg();
    draw_term();
    draw_bar();
    d_winpresent();
}

static void close_all(void) {
    if (id_term >= 0) d_winclose(id_term);
    if (id_bar >= 0) d_winclose(id_bar);
    if (id_bg >= 0) d_winclose(id_bg);
    id_term = id_bar = id_bg = -1;
    d_winpresent(); /* show the console underneath immediately */
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
    u64 last_draw = 0;
    int pending = 0;
    /* Boot mode (kernel "gui" cmdline passes argv[1]="boot"): exiting
     * spawns the text shell back instead of leaving a dead console. */
    int bootmode = (argc >= 2 && argv && argv[1] && argv[1][0] == 'b' &&
                    argv[1][1] == 'o' && argv[1][2] == 'o' &&
                    argv[1][3] == 't' && argv[1][4] == '\0');
    (void)argc;
    if (d_fbinfo(&fi) || fi.w != SCR_W || fi.h != SCR_H || fi.bpp != 32) {
        dl_s(&l, "WIN-NOFB");
        dl_nl(&l);
        return;
    }
    id_bg = d_wincreate(SCR_W, SCR_H, "desktop");
    id_term = d_wincreate(WIN_W, WIN_H, "MCT TERM");
    id_bar = d_wincreate(SCR_W, BAR_H, "taskbar");
    if (id_bg < 0 || id_term < 0 || id_bar < 0) {
        dl_s(&l, "WIN-NOSLOT");
        dl_nl(&l);
        close_all();
        return;
    }
    d_winsetpos(id_bg, 0, 0);
    d_winsetpos(id_term, (u64)win_x, (u64)win_y);
    d_winsetpos(id_bar, 0, SCR_H - BAR_H);
    tcols = (WIN_W - BORDER * 2) / 8;
    trows = (WIN_H - TITLE_H - BORDER) / 16;
    tbuf = (char *)umalloc((u64)trows * 80);
    if (!tbuf) {
        dl_s(&l, "WIN-ALLOC-FAIL");
        dl_nl(&l);
        close_all();
        return;
    }
    for (int i = 0; i < trows * 80; i++) tbuf[i] = 0;
    cur_row = 0;
    iline_n = 0;
    term_put("MCT terminal - type help");
    draw_all();
    last_draw = (u64)d_ticks();
    last_sec = (u64)-1; /* force clock paint right below */
    draw_bar();
    d_winpresent();
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
                        d_winsetpos(id_term, (u64)win_x, (u64)win_y);
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
                if (dragging) changed = 1;
            }
            (void)mx;
            (void)my;
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
        if ((u64)d_ticks() / 100 != last_sec) changed = 1; /* clock */
        if (changed || pending) {
            /* Paced flush (G4): slot writes are cheap cached RAM, but
             * each present copies UC — at most every 50ms. */
            u64 now = (u64)d_ticks();
            if (now - last_draw >= 5) {
                draw_all();
                last_draw = now;
                pending = 0;
            } else if (changed) {
                pending = 1;
            }
        }
        if (running) d_sleep(2);
    }
    close_all();
    dl_s(&l, "WIN-EXIT");
    dl_nl(&l);
    if (bootmode) {
        const char *av[1] = { "shell" };
        while (d_getchar() >= 0) {
        } /* drain GUI-session keys so the shell starts clean */
        d_spawn("shell", 1, av);
    }
}

/* Plain spawn has no display contract; point at args_main usage. */
void demo_main(void) {
    DLINE(l);
    dl_s(&l, "winsrv: use `run winsrv`");
    dl_nl(&l);
}
