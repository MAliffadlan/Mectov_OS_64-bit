/* Mectov OS 64-bit CPU layer (M2): shared types + declarations.
 *
 * Dependency-free on purpose: nothing from src/include (those headers are
 * still 32-bit and get widened in M3-M4). Shared by the k64 layer and
 * kernel64.c.
 */
#ifndef CPU64_H
#define CPU64_H

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;

#define COM1 0x3F8u

/* Serial output (defined in kernel64.c, used by k64/isr64.c too). */
void s_putc(char c);
void s_puts(const char *s);
void s_hex64(u64 v);
void s_hex32(u32 v);
void s_dec64(u64 v);
void s_printf(const char *fmt, ...); /* M7: whole line, one lock hold */
void s_write(const char *buf, u64 len); /* M7: ditto, raw buffer */
void s_rawc(char c);  /* M7: lock-free serial (fatal dumps) */
void s_raws(const char *s);
void s_rawx(u64 v);
void s_rawu(u64 v);

/* VGA-1 framebuffer text console (k64/cons64.c). cons_* run lock-free
 * internally: callers must hold serial_lock (s_putc_locked does). */
int cons_init(u64 addr, u32 pitch, u32 w, u32 h, u32 bpp);
int cons_live(void);
void cons_putc(char c);

/* 64-bit interrupt frame built by k64/entry64.asm isr64_common.
 * Field order MUST match the stub push order:
 *   push vec, push err, push rax..r15  ->  rdi = rsp points at r15.
 * Then CPU frame: rip, cs, rflags, rsp, ss (always all five in 64-bit mode).
 */
typedef struct {
    u64 r15, r14, r13, r12, r11, r10, r9, r8;
    u64 rbp, rdi, rsi, rdx, rcx, rbx, rax;
    u64 vec, err;
    u64 rip, cs, rflags, rsp, ss;
} regs64_t;

/* Init + IRQ (defined in k64/). */
void gdt64_init(void);
void gdt64_ap_load(int idx, u64 rsp0); /* M6: AP's own TR + shared GDTR */
void tss64_set_rsp0(u64 rsp0);
void tss64_set_rsp0_cpu(int cpu, u64 rsp0); /* M7: per-CPU RSP0 */
void idt64_init(void);
void idt64_load(void); /* M6: reload the shared IDT on an AP */
void pic_remap_mask_timer_kbd_mouse(void);
void pit_init_hz(u32 hz);
u64 isr64_handler(regs64_t *r);
u64 k64_ticks(void);

/* M4 task + syscall layer (defined in k64/task64.c, k64/syscall64.c).
 *
 * M4 ABI0 (via int $0x80, numbers mirror the 32-bit kernel where equal):
 *   EAX = number, EBX/ECX/EDX/ESI/EDI = args (full 64-bit regs, pointers
 *   are canonical user VAs). Returns in RAX (negative = -errno).
 *     1 PRINT  RBX=ptr RCX=len (<=2048, must be US-mapped) -> bytes
 *     8 GET_TICKS -> PIT ticks
 *     9 YIELD  -> 0 (voluntary reschedule)
 *    10 EXIT   RBX=status (noreturn; M5: zombie until reaped)
 *    19 SLEEP  RBX=ticks to sleep (M5: BLOCKED until deadline) -> 0
 *    20 GET_PID -> task id
 *    71 FORK   (M5: COW address-space clone) -> child id / 0
 *    72 WAITPID RBX=pid(-1 any) RCX=status_ptr RDX=WNOHANG -> pid/0/-ECHILD
 *    76 EXEC   RBX=name_ptr (M5: embedded image name) -> 0 (noreturn-ish)
 *   104 CLONE  RBX=func_va (child shares the address space, M4) -> child id
 * M5: every user task owns a private PML4 (kernel half shared by pointer);
 * fork() COW-shares user pages, exec() replaces the image (MCT64/ELF64).
 */
#define SYS64_PRINT 1
#define SYS64_TICKS 8
#define SYS64_YIELD 9
#define SYS64_EXIT 10
#define SYS64_SLEEP 19
#define SYS64_PID 20
#define SYS64_FORK 71
#define SYS64_WAITPID 72
#define SYS64_EXEC 76
#define SYS64_CLONE 104
#define SYS64_MEMINFO 131 /* M7.2: RBX=ptr{total,free u64} -> 0 */
#define SYS64_GETBASE 132 /* M7.3: -> own image base (ASLR proof) */
#define SYS64_GETCPU 133  /* M7.2: -> current cpu index */
#define SYS64_PS 134      /* M7.2: RBX=ptr RCX=max -> count filled */
#define SYS64_GETCHAR 135 /* M7.2: nonblocking key, -1 if empty */
#define SYS64_SPAWN 136   /* M7.2: RBX=name RCX=argc RDX=argv -> id */
#define SYS64_GETMOUSE 137 /* G1: -> packed x|y<<12|btn<<24|seq<<32 */
#define SYS64_FB_INFO 138  /* G2: RBX=ptr{fbinfo_t} -> 0/-errno */
#define SYS64_FB_MAP 139   /* G2: map display to caller -> va/-errno */
#define SYS64_FB_UNMAP 140 /* G2: release display map -> 0/-errno */
#define SYS64_WIN_CREATE 141 /* D1: RBX=w RCX=h RDX=title[16] -> id/-errno */
#define SYS64_WIN_CLOSE 142  /* D1: RBX=id -> 0/-errno */
#define SYS64_WIN_FILL 143   /* D1: RBX=id RCX=x RDX=y RSI=w RDI=h R8=rgb */
#define SYS64_WIN_TEXT 144   /* D1: RBX=id RCX=x RDX=y RSI=ptr RDI=len R8=fg R9=bg */
#define SYS64_WIN_SETPOS 145 /* D1: RBX=id RCX=x RDX=y -> 0/-errno */
#define SYS64_WIN_PRESENT 146 /* D1: flush dirty bands -> 0 */
#define SYS64_WIN_GETEVENT 147 /* D2: RBX=id RCX=ptr RDX=max -> count */
#define SYS64_WIN_FOCUS 148    /* D2: RBX=id (-1 clears) -> 0/-errno */
#define SYS64_WIN_LIST 149     /* D2: RBX=ptr RCX=max -> count */
#define SYS64_WIN_RAISE 150    /* D3: RBX=id -> topmost -> 0/-errno */
#define SYS64_REBOOT 151       /* D3: reset via 8042, noreturn */
#define SYS64_BRK 120     /* M7.3: RBX=new_brk (0 = query) -> brk */

/* ps/meminfo shared layouts (kernel + demos/libc, fixed sizes). */
typedef struct {
    int id, state, parent, cpu;
    char name[16];
} ps_entry_t;
typedef struct {
    u64 total_frames, free_frames;
} meminfo_t;
/* G2 display geometry (kernel + demos/libc, fixed sizes). */
typedef struct {
    u64 addr, pitch, w, h, bpp, size, map_va;
} fbinfo_t;
/* D2 window events + listing (kernel + demos/libc, fixed sizes). */
typedef struct {
    u32 type, d0, d1, d2;
} winev_t;
#define WEV_KEY 1   /* d0=ascii */
#define WEV_MOVE 2  /* d0=x d1=y */
#define WEV_BTN 3   /* d0=x d1=y d2=buttons */
#define WEV_ENTER 4 /* d0=x d1=y */
#define WEV_LEAVE 5 /* d0=x d1=y */
#define WEV_FOCUS 6 /* d0=1 gained, 0 lost */
typedef struct {
    u32 id, x, y, w, h;
    int owner;
    char title[16];
} wininfo_t;
#define SYS64_PRINT 1
#define SYS64_TICKS 8
#define SYS64_YIELD 9
#define SYS64_EXIT 10
#define SYS64_PID 20
#define SYS64_CLONE 104

/* Ring-3 layout (M5: every task owns its PML4; user ranges never alias the
 * low identity map, so shared kernel tables are never written by tasks). */
#define USTACK_BASE 0x50000000ULL /* per-task user stacks live here */
#define USTACK_SLOT 0x5000ULL     /* 16KB stack + 4KB unmapped guard hole */
#define USTACK_SIZE 0x4000ULL
#define DEMO_THREAD_EXIT_OFF 0x40ULL /* _thread_exit offset in demo images */
#define DEMO_START_ARGS_OFF 0x80ULL  /* M7.2 _start_args (argc/argv) offset */

/* Task states. */
#define T_FREE 0
#define T_READY 1
#define T_RUNNING 2
#define T_ZOMBIE 3   /* M5: exited, exit_code kept until waitpid reaps */
#define T_BLOCKED 4  /* M5: waitpid (waiting_for) or sleep (wakeup_tick) */
#define T_NEW 5      /* M7: slot reserved, not yet runnable (SMP publish) */

/* M6: hard cap for per-CPU tables (GDT TSS slots, AP stacks, ack slots).
 * qemu/run64 boots -smp 4; larger -smp values park the extras unstarted. */
#define NCPU_MAX 4

typedef struct task64 {
    u64 rsp;          /* saved kernel rsp (points at a regs64_t frame) */
    u64 kstack_top;
    u64 user_base;    /* demo image base (clone inherits for thread_exit) */
    u64 cr3;          /* M5: private PML4 phys (task[0] = boot PML4) */
    int id;
    int state;
    int parent;       /* M5: waiter id (-1 none) */
    int waiting_for;  /* M5: waitpid target (-1 any, -2 none) */
    u64 wakeup_tick;  /* M5: sleep deadline (0 = waitpid-blocked) */
    int exit_code;
    int last_cpu;     /* M7: cpu that last ran it (ps display) */
    u64 heap_base;    /* M7.3: demand-heap [base, brk) */
    u64 heap_brk;
    char name[16];
    u8 fx[512] __attribute__((aligned(16))); /* eager FPU image */
} task64_t;

void task64_init(void);
int task64_spawn_image(const char *kname); /* kernel-resident name */
int task64_clone(u64 func);
int task64_fork(regs64_t *r);
void task64_exit(int status);
u64 task64_waitpid(int pid, u64 status_ptr, int wnohang, regs64_t *r);
u64 task64_sleep(u64 ticks, regs64_t *r);
u64 task64_brk(u64 nw, regs64_t *r);      /* M7.3 */
int task64_demand(u64 va);                /* M7.3, fault context */
u64 task64_kill_fault(regs64_t *r, const char *kind); /* M7.3: exit 139 */
u64 task64_schedule(regs64_t *r);
u64 task64_on_tick(regs64_t *r);
u64 task64_ap_tick(regs64_t *r); /* M7: AP timer (schedule only, no ticks) */
void sched_set_running(void);    /* M7: shootdown gate (mem64.c) */
int task64_current_id(void);
task64_t *task64_self(void); /* current TCB (for base/getpid paths) */
void kbd_init(void);
void kbd_push(u8 sc);
int kbd_translate(u8 sc); /* D2: scancode -> ASCII/-1 (vec33 routing) */
void kbd_push_raw(int c); /* D2: buffer a translated char (legacy path) */
int kbd_try_get(void);
void mouse_init(void);   /* G1: PS/2 aux, safe no-op when absent */
void mouse_push(u8 b);   /* IRQ12 byte (caller holds serial_lock) */
u64 mouse_get(void);     /* packed poll for SYS64_GETMOUSE */
void mouse_cursor_state(u32 *x, u32 *y, int *shown);
u64 s_lock_hold(void);   /* export serial_lock for IRQ multi-op paths */
void s_lock_drop(u64 f);
/* Multiboot2 framebuffer geometry (kernel64.c; consumed by cons/fb). */
extern u64 g_fb_addr;
extern u32 g_fb_pitch, g_fb_w, g_fb_h, g_fb_bpp;
/* G2 display ownership (k64/fb64.c): single-mapper model. */
long fb_info(fbinfo_t *out);
long fb_map_current(void);
long fb_unmap_current(void);
void fb_owner_release(task64_t *t);
/* D1 window slots (k64/win64.c): kernel-composited, owner-checked. */
long win_create(u32 w, u32 h, const char *title);
long win_close(int id);
long win_fill(int id, u32 x, u32 y, u32 w, u32 h, u32 rgb);
long win_text(int id, u32 x, u32 y, const char *s, u64 len, u32 fg, u32 bg);
long win_setpos(int id, u32 x, u32 y);
void win_owner_release(task64_t *t);
void win_composite_scanline(u32 y, u32 *drow, u32 fb_w);
/* D2 input routing (rings are IRQ-writer/syscall-reader SPSC). */
int win_route_key(int c); /* 1 = consumed by focus, 0 = legacy ring */
void win_route_mouse(u32 x, u32 y, u32 btn);
long win_getevent(int id, winev_t *out, int max);
long win_focus_set(int id);
long win_list(wininfo_t *out, int max);
long win_raise(int id);   /* D3: stacking is server-managed, any task */
void sys_reboot(void);    /* D3: 8042 reset pulse, noreturn */
int task64_spawn_args(const char *kname, int argc, char **kargv);
int task64_ps(ps_entry_t *out, int max);
u64 task64_exec(const char *uname, regs64_t *r); /* user-space name pointer */
void exec_register(const char *name, const void *data, u64 len);
const void *exec_lookup(const char *name, u64 *len_out);
void aslr_seed(u64 s); /* M7.3: seed ELF slide PRNG (TSC at boot) */
int loader_map_image(u64 target, const u8 *img, u64 len, u64 *entry_out,
                     u64 *base_out, u64 *end_out);
u64 syscall64_dispatch(regs64_t *r);
/* M7 CR3 forensic ring (mem64.c): every CR3 load/teardown traced; the FATAL
 * PF handler dumps it without touching LAPIC or task state. */
void trace_cr3(char ev, int task, u64 cr3);
void trace_cr3_dump(void);
/* M6 SMP (defined in k64/smp64.c). Test/TLB IPI vectors + spurious. */
#define VEC_IPI_TEST 96
#define VEC_IPI_TLB 97
#define VEC_AP_TIMER 80
#define VEC_SPURIOUS 255
void smp_init(void);
void smp_wake_aps(void); /* M7: release parked APs into the scheduler */
int smp_selftest(void);
int smp_shootdown(u64 va); /* single VA on all APs; ~0ULL = full flush */
void smp_shootdown_all(void);
void smp_halt_others(void); /* NMI-freeze others for a clean fatal dump */
int smp_in_shootdown(void); /* nonzero while this CPU waits out a shoot */
int smp_cpu_by_stack(u64 rsp); /* LAPIC-free stack->cpu guess for dumps */
void lapic_eoi(void);
int smp_cpu_count(void);
int smp_cpu_index(void); /* LAPIC ID -> 0..ncpus-1, -1 unknown */
void smp_ack_test(void);
void smp_ack_tlb(u64 va);
void smp_ack_spurious(void);

/* M3 physical memory + paging (defined in k64/mem64.c). */
void mem64_init(u64 mb_info);
void mem64_selftest(void);
u64 pmm_alloc(void);
void pmm_free(u64 pa);
u64 mem_lock_acquire(void); /* hold across multi-step wiring sequences */
void mem_lock_release(u64 f);
u64 __pmm_alloc(void);      /* mem_lock held by caller */
int __frame_ref_put(u64 pa); /* mem_lock held by caller */
void pmm_free_raw(u64 pa); /* page-table frames: bitmap only, no refcount */
void frame_ref_inc(u64 pa);
int frame_ref_put(u64 pa); /* -1 ref, bitmap-free at 0, returns remainder */
u64 pmm_free_frames(void);
u64 pmm_total_frames(void);
void vmm_set_root(u64 *root); /* mapping root for vmm_map/unmap */
u64 *vmm_get_root(void);
int __vmm_map_page(u64 va, u64 pa, u64 flags); /* caller holds mem_lock */
int __vmm_unmap_page(u64 va);                    /* caller holds mem_lock */
void vmm_publish(void); /* flush APs after wiring (lock-free) */
u64 vmm_clone_space(u64 src_pml4); /* M5: COW-clone an address space */
u64 vmm_share_space(u64 src_pml4); /* M5: table-copy, pages truly shared */
void vmm_teardown_space(u64 pml4_pa); /* M5: free a task's private space */
int vmm_cow_resolve(u64 va); /* M5: resolve a COW write fault, 0 = ok */
int vmm_map_page(u64 va, u64 pa, u64 flags);
int vmm_unmap_page(u64 va);
u64 vmm_translate(u64 va);
u64 __vmm_translate(u64 va); /* caller holds mem_lock, walks live CR3 */
int vmm_probe(u64 va, u64 *flags); /* P|RW|US|(NX as bit3), -1 unmapped */
int vmm_user_ok(u64 va, u64 len);
int vmm_is_canonical(u64 va);
int cpu_nx_enabled(void);
void paging_enable_nxe_ap(void); /* M7: APs need NXE for user NX pages */

/* Flag bits for vmm_map_page callers (translated to PTE bits inside). */
#define VMM_RW  (1ULL << 0)  /* writable (default read-only) */
#define VMM_US  (1ULL << 1)  /* user-accessible (default supervisor) */
#define VMM_NX  (1ULL << 2)  /* no-execute (needs EFER.NXE) */
#define VMM_UC  (1ULL << 3)  /* uncacheable MMIO (PCD|PWT) */

/* Small CPU readers shared by k64 C files. */
static inline u64 cpu_read_efer(void) {
    u32 lo, hi;
    __asm__ __volatile__("mov $0xC0000080, %%ecx\n\trdmsr"
                         : "=a"(lo), "=d"(hi) : : "ecx");
    return ((u64)hi << 32) | lo;
}
static inline u64 cpu_read_cr2(void) {
    u64 v;
    __asm__ __volatile__("mov %%cr2, %0" : "=r"(v));
    return v;
}
static inline u64 cpu_read_cr3(void) {
    u64 v;
    __asm__ __volatile__("mov %%cr3, %0" : "=r"(v));
    return v;
}
static inline void cpu_load_cr3(u64 cr3) {
    __asm__ __volatile__("mov %0, %%cr3" :: "r"(cr3) : "memory");
}

#endif
