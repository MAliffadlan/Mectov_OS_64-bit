/* D4 asset blobs: named read-only data embedded at build time (desktop
 * QOI wallpaper/icons via objcopy). SYS_BLOBREAD copies out bounded;
 * maxlen==0 probes the size. Immutable after boot registration: no locks.
 * (Mirrors the exec registry shape in loader64.c, kept separate: these
 * are data, never executed.) */
#include "cpu64.h"

#define NBLOB 8

static struct {
    char name[16];
    const u8 *data;
    u64 len;
} blobs[NBLOB];
static int nblob = 0;

void blob_register(const char *name, const void *data, u64 len) {
    int i;
    if (nblob >= NBLOB || !name || !data || !len) return;
    i = 0;
    for (; i < 15 && name[i]; i++) blobs[nblob].name[i] = name[i];
    blobs[nblob].name[i] = '\0';
    blobs[nblob].data = (const u8 *)data;
    blobs[nblob].len = len;
    nblob++;
}

/* kname is a kernel-side bounded copy. Returns bytes copied, or -errno. */
long blob_read(const char *kname, void *dst, u64 maxlen) {
    const u8 *data = 0;
    u64 len = 0;
    int i;
    for (i = 0; i < nblob; i++) {
        int j = 0;
        while (j < 16 && blobs[i].name[j] == kname[j]) {
            if (!kname[j]) {
                data = blobs[i].data;
                len = blobs[i].len;
                break;
            }
            j++;
        }
        if (data) break;
    }
    if (!data) return -2; /* ENOENT */
    if (!maxlen) return (long)len; /* size probe */
    if (maxlen > len) maxlen = len;
    {
        volatile u8 *d = (volatile u8 *)dst;
        for (u64 k = 0; k < maxlen; k++) d[k] = data[k];
    }
    return (long)maxlen;
}
