/* G1 PS/2 auxiliary (mouse) device: 3-byte packet assembler + state.
 *
 * BSP-only like the keyboard (PIC IRQ12 routes to the BSP): the IRQ writer
 * is single, readers (any CPU via SYS_GETMOUSE) only read aligned u32s —
 * no lock needed for state. The visible cursor is NOT drawn here; the
 * console composites the sprite in cons_present(), so text/strip/scroll
 * can never corrupt it. mouse_push() only nudges dirty bands via
 * cons_cursor_moved() (lock-free u32 stores, same pattern as
 * cons_status_tick()).
 */
#include "cpu64.h"
#include "cons64.h"

static inline u8 m_inb(u16 port) {
    u8 v;
    __asm__ __volatile__("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static inline void m_outb(u16 port, u8 v) {
    __asm__ __volatile__("outb %0, %1" : : "a"(v), "Nd"(port));
}

/* Bounded busy-waits (called pre-tasks with IF=0; no sleep available). */
static int m_wait_send(void) {
    for (int i = 0; i < 1000000; i++)
        if (!(m_inb(0x64) & 2)) return 0;
    return -1;
}

static int m_wait_recv(void) {
    for (int i = 0; i < 1000000; i++)
        if (m_inb(0x64) & 1) return 0;
    return -1;
}

static void m_flush(void) {
    while (m_inb(0x64) & 1) (void)m_inb(0x60);
}

/* Send one byte to the AUX device (0xD4 prefix), expect 0xFA back. */
static int m_aux_cmd(u8 b) {
    if (m_wait_send()) return -1;
    m_outb(0x64, 0xD4);
    if (m_wait_send()) return -1;
    m_outb(0x60, b);
    if (m_wait_recv()) return -1;
    return m_inb(0x60) == 0xFA ? 0 : -1;
}

static volatile u32 mouse_x = 512, mouse_y = 384;
static volatile u32 mouse_btn = 0;
static volatile u32 mouse_seq = 0;
static volatile int mouse_live = 0;
static u32 mouse_max_x = 1023, mouse_max_y = 767;
static u8 pkt[3];
static int pkt_i = 0;

void mouse_init(void) {
    u8 cfg;
    m_flush();
    /* Controller command byte: keep keyboard IRQ + translation, enable
     * AUX IRQ (bit 1), enable both clocks (clear bits 4,5). */
    if (m_wait_send()) goto absent;
    m_outb(0x64, 0x20);
    if (m_wait_recv()) goto absent;
    cfg = m_inb(0x60);
    cfg |= 0x02;
    cfg &= ~(u8)0x30;
    if (m_wait_send()) goto absent;
    m_outb(0x64, 0x60);
    if (m_wait_send()) goto absent;
    m_outb(0x60, cfg);
    /* Enable the AUX port, then reset/defaults/enable the mouse. */
    if (m_wait_send()) goto absent;
    m_outb(0x64, 0xA8);
    if (m_aux_cmd(0xFF)) goto absent; /* reset */
    if (m_wait_recv() || m_inb(0x60) != 0xAA) goto absent; /* self-test OK */
    if (m_wait_recv() || m_inb(0x60) != 0x00) goto absent; /* mouse ID */
    if (m_aux_cmd(0xF6)) goto absent;                     /* defaults */
    if (m_aux_cmd(0xF4)) goto absent; /* enable data reporting */
    /* Drain anything queued during init (stray ACKs / SeaBIOS-era motion
     * bytes): a single 0xFA here would otherwise poison the first real
     * packet's framing (bit 3 set, so the resync check can't catch it). */
    for (int i = 0; i < 128 && (m_inb(0x64) & 1); i++) (void)m_inb(0x60);
    pkt_i = 0;
    {
        u32 w = 0, h = 0;
        cons_dims(&w, &h);
        if (w) mouse_max_x = w - 1;
        if (h) mouse_max_y = h - 1;
        mouse_x = mouse_max_x / 2;
        mouse_y = mouse_max_y / 2;
    }
    mouse_live = 1;
    s_puts("[K64] mouse: PS/2 live\n");
    return;
absent:
    s_puts("[K64] mouse: absent (no PS/2 aux)\n");
}

/* Called from the IRQ12 stub (vec 44) with one controller byte. The ISR
 * wrapper holds serial_lock and presents after this returns. */
void mouse_push(u8 b) {
    u32 ox, oy;
    int dx, dy;
    if (!mouse_live) return;
    if (pkt_i == 0 && !(b & 0x08)) return; /* resync on bit 3 */
    pkt[pkt_i++] = b;
    if (pkt_i < 3) return;
    pkt_i = 0;
    ox = mouse_x;
    oy = mouse_y;
    mouse_btn = pkt[0] & 7;
    if (!(pkt[0] & 0xC0)) {
        /* No overflow: sign-extend the 9-bit deltas (screen Y is down). */
        dx = (int)pkt[1] - ((pkt[0] & 0x10) ? 256 : 0);
        dy = (int)pkt[2] - ((pkt[0] & 0x20) ? 256 : 0);
        if (dx || dy) {
            int nx = (int)ox + dx, ny = (int)oy - dy;
            if (nx < 0) nx = 0;
            if (ny < 0) ny = 0;
            if ((u32)nx > mouse_max_x) nx = (int)mouse_max_x;
            if ((u32)ny > mouse_max_y) ny = (int)mouse_max_y;
            mouse_x = (u32)nx;
            mouse_y = (u32)ny;
        }
    }
    /* Refresh on every packet (buttons-only changes included); the ISR
     * wrapper presents right after, so the cursor tracks in real time. */
    mouse_seq++;
    cons_cursor_moved(ox, oy, mouse_x, mouse_y);
    win_route_mouse(mouse_x, mouse_y, mouse_btn); /* D2: may be no slots */
}

/* Packed Ring-3 poll: x | y<<12 | buttons<<24 | seq<<32. Never blocks. */
u64 mouse_get(void) {
    return (u64)mouse_x | ((u64)mouse_y << 12) | ((u64)mouse_btn << 24) |
           ((u64)mouse_seq << 32);
}

void mouse_cursor_state(u32 *x, u32 *y, int *shown) {
    if (x) *x = mouse_x;
    if (y) *y = mouse_y;
    if (shown) *shown = mouse_live;
}
