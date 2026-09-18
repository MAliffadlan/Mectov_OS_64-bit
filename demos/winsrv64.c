/* D2 winsrv: pure window server. Background + static taskbar with clock;
 * the terminal now lives in the separate `term` process (own slot, own
 * event loop). This server spawns it, discovers its slot via WIN_LIST,
 * positions + focuses it, drags it, and winds the GUI session down when
 * its slot vanishes (exit -> console restore, shell respawn in bootmode).
 *
 * Runs via shell `run winsrv`, hence args_main. Serial protocol for the
 * gate: WIN-READY, WIN-DRAG x,y, WIN-EXIT (terminal lines are TERM-*). */
#include "sys64.h"

#define SCR_W 1024
#define SCR_H 768
#define BAR_H 28

#define C_BG 0x001A2B3CU
#define C_TITLE 0x00000080U
#define C_TTITLE 0x00FFFFFFU

#define TERM_W 640
#define TERM_H 400
#define TERM_X0 192
#define TERM_Y0 150

static long id_bg = -1, id_bar = -1;
static long term_id = -1;
static int term_x = TERM_X0, term_y = TERM_Y0;
static int dragging = 0, grab_x = 0, grab_y = 0;
static u64 last_sec = (u64)-1;

static void wfill(long id, int x, int y, int w, int h, u32 rgb) {
    d_winfill(id, (u64)x, (u64)y, (u64)w, (u64)h, (u64)rgb);
}

static void wtext(long id, int x, int y, const char *s, u32 fg, u32 bg) {
    d_wintext(id, (u64)x, (u64)y, s, d_strlen(s), (u64)fg, (u64)bg);
}

static void draw_bar(void) {
    char clk[8];
    char ss[3];
    u64 secs = (u64)d_ticks() / 100;
    clk[0] = 'M';
    clk[1] = 'C';
    clk[2] = 'T';
    clk[3] = ' ';
    clk[4] = (char)('0' + (secs / 600) % 6);
    clk[5] = (char)('0' + (secs / 60) % 10);
    clk[6] = ':';
    clk[7] = '\0';
    ss[0] = (char)('0' + (secs / 10) % 6);
    ss[1] = (char)('0' + secs % 10);
    ss[2] = '\0';
    wfill(id_bar, 0, 0, SCR_W, BAR_H, C_TITLE);
    wtext(id_bar, 8, 6, clk, C_TTITLE, C_TITLE);
    wtext(id_bar, 8 + 7 * 8, 6, ss, C_TTITLE, C_TITLE);
    last_sec = secs;
}

static void draw_all(void) {
    wfill(id_bg, 0, 0, SCR_W, SCR_H, C_BG);
    draw_bar();
    d_winpresent();
}

static void close_all(void) {
    if (id_bar >= 0) d_winclose(id_bar);
    if (id_bg >= 0) d_winclose(id_bg);
    id_bar = id_bg = -1;
    d_winpresent(); /* show the console underneath immediately */
}

/* Find the first slot that is not ours (the terminal client). */
static long find_client(void) {
    wininfo_t info[8];
    long n = d_winlist(info, 8);
    for (long i = 0; i < n; i++) {
        if ((long)info[i].id != id_bg && (long)info[i].id != id_bar)
            return (long)info[i].id;
    }
    return -1;
}

void args_main(int argc, const char **argv) {
    DLINE(l);
    fbinfo_t fi;
    long v;
    u32 lseq = 0, lbtn = 0;
    int running = 1;
    u64 last_draw = 0;
    int pending = 0;
    u64 gone_since = 0;
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
    id_bar = d_wincreate(SCR_W, BAR_H, "taskbar");
    if (id_bg < 0 || id_bar < 0) {
        dl_s(&l, "WIN-NOSLOT");
        dl_nl(&l);
        close_all();
        return;
    }
    d_winsetpos(id_bg, 0, 0);
    d_winsetpos(id_bar, 0, SCR_H - BAR_H);
    {
        const char *av[1] = { "term" };
        if (d_spawn("term", 1, av) < 0) {
            dl_s(&l, "WIN-SPAWN-FAIL");
            dl_nl(&l);
            close_all();
            return;
        }
    }
    for (int i = 0; i < 500 && term_id < 0; i++) {
        term_id = find_client();
        if (term_id < 0) d_sleep(2);
    }
    if (term_id < 0) {
        dl_s(&l, "WIN-NOTERM");
        dl_nl(&l);
        close_all();
        return;
    }
    d_winsetpos(term_id, TERM_X0, TERM_Y0);
    d_winfocus(term_id);
    draw_all();
    last_draw = (u64)d_ticks();
    last_sec = (u64)-1;
    draw_bar();
    d_winpresent();
    dl_s(&l, "WIN-READY ");
    dl_u(&l, fi.w);
    dl_s(&l, "x");
    dl_u(&l, fi.h);
    dl_nl(&l);
    while (running) {
        int changed = 0;
        v = d_getmouse();
        {
            u32 nx = (u64)v & 0xFFF, ny = ((u64)v >> 12) & 0xFFF;
            u32 nb = ((u64)v >> 24) & 7, sq = (u64)v >> 32;
            if (sq != lseq) {
                lseq = sq;
                /* Rising edge only: begin a title drag, else (re)focus.
                 * (Per-poll focus calls would spam the client's ring and
                 * crowd out real keys.) */
                if ((nb & 1) && !(lbtn & 1)) {
                    if ((int)nx >= term_x && (int)nx < term_x + TERM_W &&
                        (int)ny >= term_y && (int)ny < term_y + 20) {
                        dragging = 1;
                        grab_x = (int)nx - term_x;
                        grab_y = (int)ny - term_y;
                    } else {
                        d_winfocus(term_id);
                    }
                }
                if (dragging) {
                    if (nb & 1) {
                        term_x = (int)nx - grab_x;
                        term_y = (int)ny - grab_y;
                        if (term_x < 0) term_x = 0;
                        if (term_y < 0) term_y = 0;
                        if (term_x > SCR_W - TERM_W)
                            term_x = SCR_W - TERM_W;
                        if (term_y > SCR_H - TERM_H)
                            term_y = SCR_H - TERM_H;
                        d_winsetpos(term_id, (u64)term_x, (u64)term_y);
                        changed = 1;
                    } else {
                        dragging = 0;
                        dl_s(&l, "WIN-DRAG ");
                        dl_u(&l, (u64)term_x);
                        dl_s(&l, ",");
                        dl_u(&l, (u64)term_y);
                        dl_nl(&l);
                        changed = 1;
                    }
                }
                lbtn = nb;
            }
        }
        if ((u64)d_ticks() / 100 != last_sec) changed = 1; /* clock */
        /* Terminal gone (closed/crashed): wind the session down after a
         * 2s grace so a slow spawn is never mistaken for an exit. */
        if (find_client() < 0) {
            if (!gone_since) gone_since = (u64)d_ticks();
            if ((u64)d_ticks() - gone_since >= 200) running = 0;
        } else {
            gone_since = 0;
        }
        if (changed || pending) {
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
    d_winfocus(-1);
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
