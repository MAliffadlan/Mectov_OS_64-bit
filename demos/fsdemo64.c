/* F2a fsdemo: read-only FS proof from userspace (READDIR only; file
 * DATA goes through the kernel selftest until F2b adds OPEN/READ).
 * Lists / and /sub, asserts hello.txt is present. Graceful without a
 * formatted disk (FSDEMO-NODISK). */
#include "sys64.h"

static int has_name(fsdirent_t *e, long n, const char *want) {
    for (long i = 0; i < n; i++) {
        const char *a = e[i].name, *b = want;
        while (*a && *a == *b) {
            a++;
            b++;
        }
        if (*a == *b) return 1;
    }
    return 0;
}

static void fsdemo_main(void) {
    DLINE(l);
    static fsdirent_t ent[16];
    long n;
    n = d_readdir("/", ent, 16);
    if (n < 0) {
        dl_s(&l, "FSDEMO-NODISK ");
        dl_u(&l, (u64)(-n));
        dl_nl(&l);
        return;
    }
    dl_s(&l, "FSDEMO-ROOT n=");
    dl_u(&l, (u64)n);
    dl_nl(&l);
    for (long i = 0; i < n; i++) {
        dl_s(&l, "FSDEMO-ENT ");
        dl_s(&l, ent[i].name);
        dl_nl(&l);
    }
    if (!has_name(ent, n, "hello.txt")) {
        dl_s(&l, "FSDEMO-LIST-MISMATCH root");
        dl_nl(&l);
        return;
    }
    n = d_readdir("/sub", ent, 16);
    if (n < 0) {
        dl_s(&l, "FSDEMO-NOSUB");
        dl_nl(&l);
        return;
    }
    if (has_name(ent, n, "nested.txt")) {
        dl_s(&l, "FSDEMO-LIST-OK");
        dl_nl(&l);
    } else {
        dl_s(&l, "FSDEMO-LIST-MISMATCH sub");
        dl_nl(&l);
    }
}

/* Both entries run the same body (boot spawn uses _start, `run` uses
 * _start_args). */
void demo_main(void) { fsdemo_main(); }

void args_main(int argc, const char **argv) {
    (void)argc;
    (void)argv;
    fsdemo_main();
}
