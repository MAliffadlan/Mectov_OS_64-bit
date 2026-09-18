/* F1 AHCI/SATA block driver: port of the 32-bit ahci.c (v38.50) to k64.
 *
 * Same deliberately small model: slot 0 only, one PRD into a 64K-aligned
 * bounce buffer, polled PxCI completion, LBA48 READ DMA EXT (writes come
 * with F2/VFS — ahci64_write exists but the gate only exercises reads).
 * Drives are numbered AHCI64_DRIVE_BASE + slot (4+, matching the 32-bit
 * unified 0-3 IDE / 4-7 AHCI scheme for the F2 dispatcher).
 *
 * 64-bit adaptations: all DMA addresses are asserted <4GB (bounce + port
 * memory live in low kernel .bss, identity-mapped); BAR5 outside the
 * static [0xFE,0xFF) MMIO window is mapped pa->pa UC on demand (q35 QEMU
 * usually lands inside, so this is a bare-metal fallback path).
 */
#include "cpu64.h"
#include "spin64.h"

#define AHCI64_DRIVE_BASE 4
#define AHCI64_MAX_PORTS 8
#define AHCI64_BATCH_MAX 128

/* HBA / port register offsets (bytes from BAR5). */
#define HBA_CAP 0x00
#define HBA_GHC 0x04
#define HBA_PI 0x0C
#define PX_REGS 0x100
#define PX_STRIDE 0x80
#define PX_CLB 0x00
#define PX_FB 0x08
#define PX_IS 0x10
#define PX_IE 0x14
#define PX_CMD 0x18
#define PX_TFD 0x20
#define PX_SIG 0x24
#define PX_SSTS 0x28
#define PX_SERR 0x30
#define PX_CI 0x38

#define GHC_AE (1u << 31)
#define CMD_ST (1u << 0)
#define CMD_SUD (1u << 1)
#define CMD_FRE (1u << 4)
#define CMD_CR (1u << 15)
#define CMD_FR (1u << 14)
#define IS_TFES (1u << 30)
#define SSTS_DET (0x3)

static spin64_t ahci_lock = SPIN64_INIT; /* order: ahci -> serial only */

static inline void a_outb(u16 port, u8 v) {
    __asm__ __volatile__("outb %0, %1" : : "a"(v), "Nd"(port));
}

static inline void a_outl(u16 port, u32 v) {
    __asm__ __volatile__("outl %0, %1" : : "a"(v), "Nd"(port));
}

static inline u32 a_inl(u16 port) {
    u32 v;
    __asm__ __volatile__("inl %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static void a_memset(void *d, int c, u64 n) {
    volatile u8 *p = (volatile u8 *)d;
    for (u64 i = 0; i < n; i++) p[i] = (u8)c;
}

static void a_memcpy(void *d, const void *s, u64 n) {
    volatile u8 *p = (volatile u8 *)d;
    const volatile u8 *q = (const volatile u8 *)s;
    for (u64 i = 0; i < n; i++) p[i] = q[i];
}

/* PCI config access (type 1, bus 0 only — covers QEMU + typical bare metal
 * AHCI placement; multifunction slots scanned fully). */
static u32 pci_cfg(u8 slot, u8 func, u8 off) {
    a_outl(0xCF8, (1u << 31) | ((u32)slot << 11) | ((u32)func << 8) |
                      (off & 0xFC));
    return a_inl(0xCFC);
}

static void pci_cfg_w(u8 slot, u8 func, u8 off, u32 v) {
    a_outl(0xCF8, (1u << 31) | ((u32)slot << 11) | ((u32)func << 8) |
                      (off & 0xFC));
    a_outl(0xCFC, v);
}

typedef struct {
    u16 flags;
    u16 prdtl;
    u32 prdbc;
    u32 ctba;
    u32 ctbau;
    u32 reserved[4];
} __attribute__((packed)) ahci_cmd_header_t;

typedef struct {
    u32 dba;
    u32 dbau;
    u32 reserved;
    u32 dbc_i;
} __attribute__((packed)) ahci_prdt_entry_t;

typedef struct {
    u8 cfis[64];
    u8 acmd[16];
    u8 reserved[48];
    ahci_prdt_entry_t prdt[1];
} __attribute__((packed, aligned(128))) ahci_cmd_table_t;

typedef struct {
    ahci_cmd_header_t clist[32] __attribute__((aligned(1024)));
    u8 rfis[256] __attribute__((aligned(256)));
    ahci_cmd_table_t table __attribute__((aligned(4096)));
} ahci_port_mem_t;

static ahci_port_mem_t ahci_mem[AHCI64_MAX_PORTS] __attribute__((aligned(4096)));
static u8 ahci_bounce[65536] __attribute__((aligned(65536)));

static volatile u32 *ahci_mmio = 0;
static int ahci_ports[AHCI64_MAX_PORTS];
static int ahci_ndrives = 0;

static volatile u32 *px(int port, int reg) {
    return (volatile u32 *)((u8 *)ahci_mmio + PX_REGS + port * PX_STRIDE +
                            reg);
}

static int ahci_port_start(int port) {
    volatile u32 *cmd = px(port, PX_CMD);
    int t;
    u64 mem_phys;
    *cmd &= ~(CMD_ST | CMD_FRE);
    t = 200000;
    while ((*cmd & (CMD_CR | CMD_FR)) && --t > 0) {
    }
    if (t == 0) {
        s_puts("[AHCI] port engine never stopped\n");
        return -1;
    }
    mem_phys = (u64)&ahci_mem[port];
    if (mem_phys >= 0x100000000ULL) return -1; /* DMA needs <4GB */
    *px(port, PX_CLB) = (u32)mem_phys;
    *px(port, PX_CLB + 4) = 0;
    *px(port, PX_FB) = (u32)mem_phys + 1024;
    *px(port, PX_FB + 4) = 0;
    *px(port, PX_IS) = 0xFFFFFFFF;
    *px(port, PX_SERR) = 0xFFFFFFFF;
    *px(port, PX_IE) = 0;
    *cmd |= CMD_SUD | CMD_FRE | CMD_ST;
    return 0;
}

void ahci64_init(void) {
    int i;
    for (i = 0; i < AHCI64_MAX_PORTS; i++) ahci_ports[i] = -1;
    for (u8 slot = 0; slot < 32; slot++) {
        for (u8 func = 0; func < 8; func++) {
            u32 id = pci_cfg(slot, func, 0);
            u32 cls;
            u32 bar5, base, pcicmd, pi;
            if (id == 0xFFFFFFFF) continue;
            cls = pci_cfg(slot, func, 0x08);
            /* class 01 / subclass 06 / prog-if 01 = AHCI. */
            if (((cls >> 24) & 0xFF) != 0x01 || ((cls >> 16) & 0xFF) != 0x06 ||
                ((cls >> 8) & 0xFF) != 0x01)
                continue;
            bar5 = pci_cfg(slot, func, 0x24);
            if (bar5 & 1) continue;
            base = bar5 & ~0xF;
            if (base < 0xFE000000 || base >= 0xFF000000) {
                /* Outside the static window: map 8KB (all 32 ports' regs
                 * fit in 0x1100) pa->pa UC. Identity never covers MMIO
                 * (only low RAM), so no large-page split is possible. */
                if (base < 0x100000000ULL - 8192 && base >= 0x1000000) {
                    if (vmm_map_page(base, base, VMM_RW | VMM_UC)) return;
                    if (vmm_map_page(base + 4096, base + 4096,
                                     VMM_RW | VMM_UC))
                        return;
                } else {
                    continue;
                }
            }
            pcicmd = pci_cfg(slot, func, 0x04);
            pci_cfg_w(slot, func, 0x04, pcicmd | 0x6);
            ahci_mmio = (volatile u32 *)(u64)base;
            *(volatile u32 *)((u8 *)ahci_mmio + HBA_GHC) |= GHC_AE;
            pi = *(volatile u32 *)((u8 *)ahci_mmio + HBA_PI);
            s_puts("[AHCI] controller @ ");
            s_hex64(base);
            s_puts(" PI=");
            s_hex64(pi);
            s_puts("\n");
            for (int p = 0; p < 32 && ahci_ndrives < AHCI64_MAX_PORTS; p++) {
                u32 ssts, sig;
                if (!(pi & (1u << p))) continue;
                ssts = *px(p, PX_SSTS);
                if ((ssts & SSTS_DET) != SSTS_DET) continue;
                sig = *px(p, PX_SIG);
                if ((sig >> 16) == 0xEB14) continue; /* ATAPI: skip */
                if (ahci_port_start(p) < 0) continue;
                ahci_ports[ahci_ndrives] = p;
                s_puts("[AHCI] port ");
                s_dec64((u64)p);
                s_puts(" SATA -> drive ");
                s_dec64((u64)(AHCI64_DRIVE_BASE + ahci_ndrives));
                s_puts("\n");
                ahci_ndrives++;
            }
            s_puts(ahci_ndrives ? "[AHCI] ready\n" : "[AHCI] no disks\n");
            return;
        }
    }
    s_puts("[AHCI] no AHCI controller (PCI)\n");
}

int ahci64_present(void) { return ahci_ndrives > 0; }
int ahci64_drive_count(void) { return ahci_ndrives; }

static int ahci_issue(int port, int is_write, u32 lba, int count,
                      u8 *kbuf) {
    ahci_port_mem_t *m = &ahci_mem[port];
    int len = count * 512;
    u64 buf_phys = (u64)kbuf;
    u8 *f = m->table.cfis;
    int idle, t;
    u32 is;
    if (buf_phys >= 0x100000000ULL) return -1;
    m->clist[0].flags = (u16)(5 | (1u << 10) | (is_write ? (1u << 6) : 0));
    m->clist[0].prdtl = 1;
    m->clist[0].ctba = (u32)(u64)&m->table;
    m->clist[0].ctbau = 0;
    a_memset(f, 0, 64);
    f[0] = 0x27;
    f[1] = 0x80;
    f[2] = is_write ? 0x35 : 0x25;
    f[4] = (u8)lba;
    f[5] = (u8)(lba >> 8);
    f[6] = (u8)(lba >> 16);
    f[7] = 0x40;
    f[8] = (u8)(lba >> 24);
    f[9] = 0;
    f[10] = 0;
    f[12] = (u8)count;
    m->table.prdt[0].dba = (u32)buf_phys;
    m->table.prdt[0].dbau = 0;
    m->table.prdt[0].reserved = 0;
    m->table.prdt[0].dbc_i = ((u32)len - 1) | (1u << 31);
    *px(port, PX_IS) = 0xFFFFFFFF;
    *px(port, PX_SERR) = 0xFFFFFFFF;
    idle = 200000;
    while ((*px(port, PX_TFD) & 0x88) && --idle > 0) {
    }
    if (idle == 0) {
        s_puts("[AHCI] port not idle (TFD=");
        s_hex64(*px(port, PX_TFD));
        s_puts(")\n");
        return -1;
    }
    *px(port, PX_CI) = 1;
    t = 5000000;
    while (--t > 0) {
        u32 ci = *px(port, PX_CI);
        u32 s = *px(port, PX_IS);
        if (s & IS_TFES) break;
        if ((ci & 1) == 0) break;
    }
    is = *px(port, PX_IS);
    if (t == 0 || (is & IS_TFES) || (*px(port, PX_CI) & 1)) {
        s_puts("[AHCI] transfer error IS=");
        s_hex64(is);
        s_puts("\n");
        *px(port, PX_IS) = 0xFFFFFFFF;
        *px(port, PX_SERR) = 0xFFFFFFFF;
        *px(port, PX_CMD) |= CMD_ST;
        return -1;
    }
    return 0;
}

/* Clamp to 128 sectors and the 128-sector LBA boundary (32-bit rule). */
static int batch_limit(u32 lba, int count) {
    int boundary;
    if (count > AHCI64_BATCH_MAX) count = AHCI64_BATCH_MAX;
    boundary = 128 - (int)(lba & 127);
    if (count > boundary) count = boundary;
    return (count < 1) ? 1 : count;
}

int ahci64_read(int drive, u32 lba, int count, u8 *buf) {
    int slot = drive - AHCI64_DRIVE_BASE;
    int rc;
    u64 f;
    if (!ahci_mmio || slot < 0 || slot >= AHCI64_MAX_PORTS ||
        ahci_ports[slot] < 0)
        return -1;
    if (count < 1) return -1;
    count = batch_limit(lba, count);
    f = spin64_lock_irqsave(&ahci_lock);
    rc = ahci_issue(ahci_ports[slot], 0, lba, count, ahci_bounce);
    if (rc == 0) a_memcpy(buf, ahci_bounce, (u64)count * 512);
    spin64_unlock_irqrestore(&ahci_lock, f);
    return rc;
}

int ahci64_write(int drive, u32 lba, int count, const u8 *buf) {
    int slot = drive - AHCI64_DRIVE_BASE;
    int rc;
    u64 f;
    if (!ahci_mmio || slot < 0 || slot >= AHCI64_MAX_PORTS ||
        ahci_ports[slot] < 0)
        return -1;
    if (count < 1) return -1;
    count = batch_limit(lba, count);
    f = spin64_lock_irqsave(&ahci_lock);
    a_memcpy(ahci_bounce, buf, (u64)count * 512);
    rc = ahci_issue(ahci_ports[slot], 1, lba, count, ahci_bounce);
    spin64_unlock_irqrestore(&ahci_lock, f);
    return rc;
}
