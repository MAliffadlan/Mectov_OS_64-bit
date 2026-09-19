/* F2b fswrite: file write path proof (create/truncate/write/reread/
 * lseek/close). Idempotent (O_CREAT|O_TRUNC, fixed 3000B pattern), so
 * boot-spawning every boot is safe. */
#include "sys64.h"

#define WN_FULL 20000 /* 20 blocks: 12 direct + 8 single */
#define WN_DIRECT 12288 /* direct only (bisect) */
static long wn_bytes = WN_FULL;

static void wbyte(char *b, long i) { *b = (char)((i * 31 + 7) & 0xFF); }

static int verify(const char *b, long off, long n) {
    for (long i = 0; i < n; i++) {
        char e;
        wbyte(&e, off + i);
        if (b[i] != e) return 0;
    }
    return 1;
}

static void fswrite_main(void) {
    DLINE(l);
    static char buf[1024];
    long fd, r, i;
    fd = d_open("/wtest.txt", O_CREAT | O_TRUNC | O_RDWR);
    if (fd < 0) {
        dl_s(&l, "FSWRITE-OPEN-FAIL ");
        dl_u(&l, (u64)(-fd));
        dl_nl(&l);
        return;
    }
    for (i = 0; i < wn_bytes; i += 1024) {
        long n = wn_bytes - i > 1024 ? 1024 : wn_bytes - i;
        for (long k = 0; k < n; k++) wbyte(&buf[k], i + k);
        r = d_write(fd, buf, (u64)n);
        if (r != n) {
            dl_s(&l, "FSWRITE-WRITE-FAIL ");
            dl_u(&l, (u64)i);
            dl_s(&l, " r=");
            dl_u(&l, (u64)(r < 0 ? -r : r));
            dl_s(&l, r < 0 ? "neg" : "pos");
            dl_nl(&l);
            d_close(fd);
            return;
        }
    }
    if (d_lseek(fd, 0, 0) != 0) {
        dl_s(&l, "FSWRITE-SEEK-FAIL");
        dl_nl(&l);
        d_close(fd);
        return;
    }
    for (i = 0; i < wn_bytes; i += 1024) {
        long n = wn_bytes - i > 1024 ? 1024 : wn_bytes - i;
        r = d_read(fd, buf, 1024);
        if (r != n || !verify(buf, i, n)) {
            dl_s(&l, "FSWRITE-REREAD-MISMATCH ");
            dl_u(&l, (u64)i);
            dl_nl(&l);
            d_close(fd);
            return;
        }
    }
    if (d_lseek(fd, 1000, 0) != 1000 || d_read(fd, buf, 100) != 100 ||
        !verify(buf, 1000, 100)) {
        dl_s(&l, "FSWRITE-SEEKREAD-MISMATCH");
        dl_nl(&l);
        d_close(fd);
        return;
    }
    d_close(fd);
    /* Reopen read-only: persistence across opens (not just fd memory). */
    fd = d_open("/wtest.txt", O_RDONLY);
    dl_s(&l, "FSWRITE-REOPEN fd=");
    dl_u(&l, (u64)(fd < 0 ? -fd : fd));
    dl_s(&l, fd < 0 ? "neg" : "pos");
    dl_nl(&l);
    if (fd < 0) {
        dl_s(&l, "FSWRITE-REOPEN-MISMATCH open");
        dl_nl(&l);
        return;
    }
    r = d_read(fd, buf, 16);
    dl_s(&l, "FSWRITE-REOPEN r=");
    dl_u(&l, (u64)(r < 0 ? -r : r));
    dl_s(&l, r < 0 ? "neg" : "pos");
    dl_nl(&l);
    if (r != 16 || !verify(buf, 0, 16)) {
        dl_s(&l, "FSWRITE-REOPEN-MISMATCH data");
        dl_nl(&l);
        d_close(fd);
        return;
    }
    d_close(fd);
    dl_s(&l, "FSWRITE-DONE bytes=");
    dl_u(&l, (u64)wn_bytes);
    dl_nl(&l);
}

/* Both entries run the same body (boot spawn uses _start, `run` uses
 * _start_args). */
void demo_main(void) { fswrite_main(); }

void args_main(int argc, const char **argv) {
    if (argc >= 2 && argv[1][0] == 'd') wn_bytes = WN_DIRECT;
    fswrite_main();
}
