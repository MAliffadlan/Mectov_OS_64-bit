/* G1 mousedemo: poll SYS_GETMOUSE, report movement/button changes.
 * Throttled (every 100px accumulated or any button change) so the serial
 * stays readable; exits after 4 reports for the gate. Runs via shell
 * `run mousedemo`, hence args_main (spawn_args enters _start_args). */
#include "sys64.h"

void args_main(int argc, const char **argv) {
    DLINE(l);
    (void)argc;
    (void)argv;
    long first = d_getmouse();
    u64 lx = (u64)first & 0xFFF, ly = ((u64)first >> 12) & 0xFFF;
    dl_s(&l, "MOUSE-START x=");
    dl_u(&l, lx);
    dl_s(&l, " y=");
    dl_u(&l, ly);
    dl_nl(&l);
    int reps = 0;
    u64 ax = lx, ay = ly;
    u64 lb = 0;
    for (int i = 0; i < 2000 && reps < 3; i++) {
        long v = d_getmouse();
        u64 x = (u64)v & 0xFFF, y = ((u64)v >> 12) & 0xFFF;
        u64 b = ((u64)v >> 24) & 7;
        u64 dx = x > ax ? x - ax : ax - x;
        u64 dy = y > ay ? y - ay : ay - y;
        if (b != lb || dx + dy >= 100) {
            dl_s(&l, "MOUSE x=");
            dl_u(&l, x);
            dl_s(&l, " y=");
            dl_u(&l, y);
            dl_s(&l, " btn=");
            dl_u(&l, b);
            dl_nl(&l);
            ax = x;
            ay = y;
            lb = b;
            reps++;
        }
        d_sleep(2);
    }
    dl_s(&l, "MOUSE-DONE reps=");
    dl_u(&l, (u64)reps);
    dl_nl(&l);
    /* Exact final position (unthrottled): the gate matches the on-screen
     * cursor against this, free of the 100px report threshold lag. */
    {
        long v = d_getmouse();
        dl_s(&l, "MOUSE-END x=");
        dl_u(&l, (u64)v & 0xFFF);
        dl_s(&l, " y=");
        dl_u(&l, ((u64)v >> 12) & 0xFFF);
        dl_nl(&l);
    }
}

/* Plain spawn (_start) has no argv path here; point at args_main usage. */
void demo_main(void) {
    DLINE(l);
    dl_s(&l, "mousedemo: use `run mousedemo`");
    dl_nl(&l);
}
