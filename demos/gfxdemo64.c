/* G2 gfxdemo: first Ring-3 graphics — map the display, draw a gradient +
 * shapes + a umalloc'd sprite, hold for the gate, unmap (console
 * restores). Runs via shell `run gfxdemo`, hence args_main. */
#include "sys64.h"
#include "umalloc.h"

void args_main(int argc, const char **argv) {
    DLINE(l);
    fbinfo_t fi;
    volatile u32 *fb;
    u32 stride, w, h, x, y;
    u32 *spr;
    (void)argc;
    (void)argv;
    if (d_fbinfo(&fi) || fi.w != 1024 || fi.h != 768 || fi.bpp != 32) {
        dl_s(&l, "GFX-NOFB");
        dl_nl(&l);
        return;
    }
    if (d_fbmap() != (long)fi.map_va) {
        dl_s(&l, "GFX-MAP-FAIL");
        dl_nl(&l);
        return;
    }
    spr = (u32 *)umalloc(64 * 64 * 4);
    if (!spr) {
        dl_s(&l, "GFX-ALLOC-FAIL");
        dl_nl(&l);
        d_fbunmap();
        return;
    }
    for (y = 0; y < 64; y++)
        for (x = 0; x < 64; x++)
            spr[y * 64 + x] =
                (((x / 8) + (y / 8)) & 1) ? 0x00FFFFFFUL : 0x00000000UL;
    fb = (volatile u32 *)fi.map_va;
    stride = (u32)(fi.pitch / 4);
    w = (u32)fi.w;
    h = (u32)fi.h;
    /* Gradient + white 4px border + RGB squares + sprite. */
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            u32 edge = (x < 4 || y < 4 || x >= w - 4 || y >= h - 4);
            u32 r = (x >= 100 && x < 200 && y >= 100 && y < 200);
            u32 g = (x >= 250 && x < 350 && y >= 100 && y < 200);
            u32 b = (x >= 400 && x < 500 && y >= 100 && y < 200);
            u32 v;
            if (edge)
                v = 0x00FFFFFFUL;
            else if (r)
                v = 0x00FF0000UL;
            else if (g)
                v = 0x0000FF00UL;
            else if (b)
                v = 0x000000FFUL;
            else
                v = ((x * 255 / w) << 16) | ((y * 255 / h) << 8) | 0x80;
            fb[(u64)y * stride + x] = v;
        }
    }
    for (y = 0; y < 64; y++)
        for (x = 0; x < 64; x++)
            fb[(u64)(500 + y) * stride + 700 + x] = spr[y * 64 + x];
    dl_s(&l, "GFX-DONE ");
    dl_u(&l, fi.w);
    dl_s(&l, "x");
    dl_u(&l, fi.h);
    dl_nl(&l);
    d_sleep(1500); /* hold the art for the gate (~15s) */
    if (d_fbunmap())
        dl_s(&l, "GFX-UNMAP-FAIL");
    else
        dl_s(&l, "GFX-UNMAP-OK");
    dl_nl(&l);
}

/* Plain spawn has no display contract; point at args_main usage. */
void demo_main(void) {
    DLINE(l);
    dl_s(&l, "gfxdemo: use `run gfxdemo`");
    dl_nl(&l);
}
