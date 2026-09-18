/* D3 winsrv: window server with Start menu + live taskbar. Background,
 * taskbar (Start button, per-window buttons, clock), popup menu with
 * launcher (Terminal/GfxDemo), Reboot and Exit-to-text. Terminals remain
 * separate `term` processes; this server spawns, discovers (WIN_LIST),
 * positions, focuses, raises and drags them.
 *
 * Runs via shell `run winsrv`, hence args_main. Serial protocol for the
 * gate: WIN-READY, WIN-DRAG x,y, WIN-EXIT, WIN-MENU open/launch markers
 * (terminal lines are TERM-*). */
#include "sys64.h"

#define SCR_W 1024
#define SCR_H 768
#define BAR_H 28

#define C_BG 0x001A2B3CU
#define C_TITLE 0x00000080U
#define C_TTITLE 0x00FFFFFFU
#define C_MENU_BG 0x00202530U
#define C_FRAME 0x00C0C0C0U
#define C_BTN 0x00004080U

#define MENU_W 220
#define MENU_N 4
#define MENU_ROW 24
#define MENU_H (8 + MENU_N * MENU_ROW)
#define MENU_X 0
#define MENU_Y (SCR_H - BAR_H - MENU_H)

static const char *menu_entries[MENU_N] = {
    "Terminal", "GfxDemo", "Reboot", "Exit to text",
};

static long id_bg = -1, id_bar = -1, menu_id = -1;
static long drag_id = -1;
static int drag_x = 0, drag_y = 0, grab_x = 0, grab_y = 0;
static u64 last_sec = (u64)-1;

static void wfill(long id, int x, int y, int w, int h, u32 rgb) {
    d_winfill(id, (u64)x, (u64)y, (u64)w, (u64)h, (u64)rgb);
}

static void wtext(long id, int x, int y, const char *s, u32 fg, u32 bg) {
    d_wintext(id, (u64)x, (u64)y, s, d_strlen(s), (u64)fg, (u64)bg);
}

/* Window list snapshot (index order = bottom-to-top z). */
static long snap_list(wininfo_t *info) { return d_winlist(info, 8); }

static long launch_wait(const char *name, long *known, long nknown);

static int is_chrome(long id) {
    return id == id_bg || id == id_bar || id == menu_id;
}

/* Topmost client slot containing (x,y), or -1. */
static long hit_client(wininfo_t *info, long n, int x, int y) {
    for (long i = n - 1; i >= 0; i--) {
        if (is_chrome((long)info[i].id)) continue;
        if (x >= (int)info[i].x && x < (int)(info[i].x + info[i].w) &&
            y >= (int)info[i].y && y < (int)(info[i].y + info[i].h))
            return (long)info[i].id;
    }
    return -1;
}

static u64 list_sig(wininfo_t *info, long n) {
    u64 s = (u64)n;
    for (long i = 0; i < n; i++)
        s = s * 33 + info[i].id + info[i].x + info[i].y;
    return s;
}

static void draw_bar(wininfo_t *info, long n) {
    char clk[8];
    char ss[3];
    u64 secs = (u64)d_ticks() / 100;
    int bx = 90, bi = 0;
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
    wfill(id_bar, 2, 4, 76, 20, C_BTN);
    wtext(id_bar, 8, 6, "Start", C_TTITLE, C_BTN);
    for (long i = 0; i < n && bi < 6; i++) {
        char t[15];
        int k;
        if (is_chrome((long)info[i].id)) continue;
        for (k = 0; k < 14 && info[i].title[k]; k++) t[k] = info[i].title[k];
        t[k] = '\0';
        wfill(id_bar, bx, 4, 120, 20, C_BTN);
        wtext(id_bar, bx + 6, 6, t, C_TTITLE, C_BTN);
        bx += 125;
        bi++;
    }
    wtext(id_bar, 8, 6, " ", C_TTITLE, C_TITLE);
    wtext(id_bar, 8, 6, clk, C_TTITLE, C_TITLE);
    wtext(id_bar, 8 + 7 * 8, 6, ss, C_TTITLE, C_TITLE);
    last_sec = secs;
}

static void draw_menu(void) {
    wfill(menu_id, 0, 0, MENU_W, MENU_H, C_MENU_BG);
    wfill(menu_id, 0, 0, MENU_W, 2, C_FRAME);
    wfill(menu_id, 0, 0, 2, MENU_H, C_FRAME);
    wfill(menu_id, MENU_W - 2, 0, 2, MENU_H, C_FRAME);
    wfill(menu_id, 0, MENU_H - 2, MENU_W, 2, C_FRAME);
    for (int i = 0; i < MENU_N; i++)
        wtext(menu_id, 12, 4 + i * MENU_ROW + 5, menu_entries[i], C_TTITLE,
              C_MENU_BG);
}

static void open_menu(void) {
    if (menu_id >= 0) return;
    menu_id = d_wincreate(MENU_W, MENU_H, "menu");
    if (menu_id < 0) return;
    d_winsetpos(menu_id, MENU_X, MENU_Y);
    d_winraise(menu_id);
    draw_menu();
    d_winpresent();
}

static void close_menu(void) {
    if (menu_id < 0) return;
    d_winclose(menu_id);
    menu_id = -1;
    d_winpresent();
}

/* Returns 1 when the server itself should exit (Exit entry). */
static int menu_action(dline_t *l, int entry) {
    if (entry == 0) {
        wininfo_t info[8];
        long n = snap_list(info), known[8];
        for (long i = 0; i < n && i < 8; i++) known[i] = (long)info[i].id;
        close_menu();
        if (launch_wait("term", known, n < 8 ? n : 8) < 0) {
            dl_s(l, "WIN-SPAWN-FAIL");
            dl_nl(l);
        }
        return 0;
    }
    if (entry == 1) {
        const char *av[1] = { "gfxdemo" };
        close_menu();
        d_spawn("gfxdemo", 1, av); /* fullscreen, returns on its own */
        return 0;
    }
    if (entry == 2) {
        dl_s(l, "WIN-REBOOT");
        dl_nl(l);
        d_reboot();
        return 0; /* noreturn in practice */
    }
    close_menu(); /* entry 3: Exit to text */
    return 1;
}

static void draw_all(wininfo_t *info, long n) {
    wfill(id_bg, 0, 0, SCR_W, SCR_H, C_BG);
    draw_bar(info, n);
    if (menu_id >= 0) draw_menu();
    d_winpresent();
}

static void close_all(void) {
    close_menu();
    if (id_bar >= 0) d_winclose(id_bar);
    if (id_bg >= 0) d_winclose(id_bg);
    id_bar = id_bg = -1;
    d_winpresent(); /* show the console underneath immediately */
}

/* First slot that is not ours (initial terminal discovery). */
static long find_client(void) {
    wininfo_t info[8];
    long n = snap_list(info);
    for (long i = 0; i < n; i++) {
        long id = (long)info[i].id;
        if (id != id_bg && id != id_bar && id != menu_id) return id;
    }
    return -1;
}

/* Spawn an app, wait for its new slot (not in `known`), focus + raise it. */
static long launch_wait(const char *name, long *known, long nknown) {
    const char *av[1];
    av[0] = name;
    if (d_spawn(name, 1, av) < 0) return -1;
    for (int i = 0; i < 500; i++) {
        wininfo_t info[8];
        long n = snap_list(info);
        for (long k = 0; k < n; k++) {
            long id = (long)info[k].id;
            int seen = (id == id_bg || id == id_bar || id == menu_id);
            for (long j = 0; !seen && j < nknown; j++)
                if (known[j] == id) seen = 1;
            if (!seen) {
                d_winfocus(id);
                d_winraise(id);
                return id;
            }
        }
        d_sleep(2);
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
    wininfo_t info[8];
    long ninfo = 0;
    u64 sig = 0;
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
    for (int i = 0; i < 500; i++) {
        if (find_client() >= 0) break;
        d_sleep(2);
    }
    {
        long tid = find_client();
        if (tid >= 0) {
            d_winsetpos(tid, 192, 150);
            d_winfocus(tid);
        }
    }
    draw_all(info, ninfo);
    last_draw = (u64)d_ticks();
    last_sec = (u64)-1;
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
                /* Rising edge: menu / buttons / drag-start / focus. */
                if ((nb & 1) && !(lbtn & 1)) {
                    ninfo = snap_list(info);
                    if (menu_id >= 0) {
                        if ((int)nx >= MENU_X &&
                            (int)nx < MENU_X + MENU_W &&
                            (int)ny >= MENU_Y &&
                            (int)ny < MENU_Y + MENU_H) {
                            int e = ((int)ny - MENU_Y - 4) / MENU_ROW;
                            if (e >= 0 && e < MENU_N &&
                                menu_action(&l, e))
                                running = 0;
                        } else if (!((int)nx < 80 &&
                                     (int)ny >= SCR_H - BAR_H)) {
                            close_menu(); /* dismiss */
                        }
                        /* else: Start toggle handled below (menu open) */
                        if (menu_id >= 0 && (int)nx < 80 &&
                            (int)ny >= SCR_H - BAR_H)
                            close_menu();
                    } else if ((int)nx < 80 && (int)ny >= SCR_H - BAR_H) {
                        open_menu();
                        dl_s(&l, "WIN-MENU open");
                        dl_nl(&l);
                    } else if ((int)ny >= SCR_H - BAR_H) {
                        /* Taskbar band: buttons focus+raise their slot. */
                        long k = 0;
                        for (long i = 0; i < ninfo && k < 6; i++) {
                            long bid = (long)info[i].id;
                            if (is_chrome(bid)) continue;
                            if ((int)nx >= 90 + (int)k * 125 &&
                                (int)nx < 90 + (int)k * 125 + 120) {
                                d_winfocus(bid);
                                d_winraise(bid);
                                break;
                            }
                            k++;
                        }
                    } else {
                        long hit =
                            hit_client(info, ninfo, (int)nx, (int)ny);
                        int hit_y = 0;
                        if (hit >= 0) {
                            for (long i = 0; i < ninfo; i++)
                                if ((long)info[i].id == hit)
                                    hit_y = (int)info[i].y;
                            d_winfocus(hit);
                            d_winraise(hit);
                            /* Title grab starts a drag. */
                            if ((int)ny < hit_y + 20) {
                                drag_id = hit;
                                grab_x = (int)nx;
                                grab_y = (int)ny;
                                for (long i = 0; i < ninfo; i++)
                                    if ((long)info[i].id == hit) {
                                        grab_x -= (int)info[i].x;
                                        grab_y -= (int)info[i].y;
                                    }
                            }
                        }
                    }
                }
                if (drag_id >= 0) {
                    if (nb & 1) {
                        wininfo_t cur[8];
                        long nc = snap_list(cur);
                        u32 ww = 640, hh = 400;
                        for (long i = 0; i < nc; i++)
                            if ((long)cur[i].id == drag_id) {
                                ww = cur[i].w;
                                hh = cur[i].h;
                            }
                        drag_x = (int)nx - grab_x;
                        drag_y = (int)ny - grab_y;
                        if (drag_x < 0) drag_x = 0;
                        if (drag_y < 0) drag_y = 0;
                        if (drag_x > SCR_W - (int)ww)
                            drag_x = SCR_W - (int)ww;
                        if (drag_y > SCR_H - (int)hh)
                            drag_y = SCR_H - (int)hh;
                        d_winsetpos(drag_id, (u64)drag_x, (u64)drag_y);
                        changed = 1;
                    } else {
                        dl_s(&l, "WIN-DRAG ");
                        dl_u(&l, (u64)drag_x);
                        dl_s(&l, ",");
                        dl_u(&l, (u64)drag_y);
                        dl_nl(&l);
                        drag_id = -1;
                        changed = 1;
                    }
                } else if ((nb & 1) != (lbtn & 1)) {
                    changed = 1;
                }
                lbtn = nb;
            }
        }
        /* Taskbar buttons: click focuses+raises (topmost hit first). */
        ninfo = snap_list(info);
        if ((u64)d_ticks() / 100 != last_sec) changed = 1; /* clock */
        if (list_sig(info, ninfo) != sig) {
            sig = list_sig(info, ninfo);
            changed = 1;
        }
        /* No clients left: wind the session down after a 2s grace. */
        {
            int any = 0;
            for (long i = 0; i < ninfo; i++)
                if (!is_chrome((long)info[i].id)) any = 1;
            if (!any) {
                if (!gone_since) gone_since = (u64)d_ticks();
                if ((u64)d_ticks() - gone_since >= 200) running = 0;
            } else {
                gone_since = 0;
            }
        }
        if (changed || pending) {
            u64 now = (u64)d_ticks();
            if (now - last_draw >= 5) {
                draw_all(info, ninfo);
                last_draw = now;
                pending = 0;
            } else if (changed) {
                pending = 1;
            }
        }
        if (running) d_sleep(2);
    }
    if (menu_id >= 0) close_menu();
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

