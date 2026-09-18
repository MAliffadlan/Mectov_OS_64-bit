# Mectov OS 64-bit

A monolithic **x86-64** kernel written from scratch in C and Assembly — freestanding, no libc, no external dependencies. Boots via GRUB Multiboot2 straight into 64-bit long mode: 4-level paging, symmetric multiprocessing on 4 cores, preemptive multitasking with per-process address spaces, and a Unix-style process model running in Ring 3.

This is the 64-bit successor project, developed on its own branch history here. (The 32-bit i386 ancestor lives in a separate repository.)

## Highlights

- **Long-mode boot** — Multiboot2 header, PML4 identity map, EFER.LME/NXE, 64-bit GDT.
- **4-level paging + PMM** — frame-bitmap allocator, 2 MB identity map, uncachable framebuffer + MMIO windows, NX everywhere it matters.
- **True SMP** — INIT-SIPI-SIPI AP bring-up (16→32→64 trampoline), per-CPU TSS/GDT slots, LAPIC + PIT timers, fixed IPIs, TLB-shootdown plumbing, ticket-free spinlocks with strict lock ordering.
- **Preemptive multitasking** — per-CPU runqueues over one task table, COW `fork()`, `clone()`, `exec()` (MCT2/ELF64), `waitpid()` with zombies, `sleep()`, per-task eager FPU state, W^X loader, ASLR for ELF.
- **Demand paging** — `brk()` heap grows the pointer; pages materialize zero-filled on first touch, with guard-hole and canonical-address enforcement.
- **Ring-3 shell** — `mct>` prompt over PS/2 keyboard: `help ps run exec ticks mem echo sleep cpu exit`, foreground `run` with real `argc/argv`.
- **Framebuffer console + 2D** — 1024×768×32 text console on a 4 MB backbuffer (dirty-row present), persistent status strip (G0 tag, RGB bars, tick progress), `pixel/fill/blit` primitives; no mouse/GUI yet.
- **Syscall ABI** — `int $0x80` (kept deliberately for bring-up; `syscall/sysret` is future work), validated user pointers, per-syscall errno returns.

## Layout

```
boot64.asm     Multiboot2 header + long-mode entry stub
kernel64.c     kernel_main: init order, boot banner, idle loop
linker64.ld    ELF64 link script (kernel at 1 MiB)
run64.sh       QEMU launcher (q35, 4 cores, serial log, headless gate)
k64/           Kernel: gdt/idt/isr, mem/paging, tasks/sched, syscalls,
               MCT2+ELF64 loader, SMP/LAPIC, PS/2 keyboard, spinlocks
demos/         Ring-3 programs (MCT2, one ELF64): shell, hello, fpu,
               clone/fork/exec demos, brk/nx/aslr/smp/meminfo tests
scripts/       build_mct64.py, build_elf64.py, qmp.py, kbd_test.py,
               vga_test.py, stress.py
```

## Build, run, test

Requirements: `gcc (-m64)` · `nasm` · `ld` · `qemu-system-x86_64` · `python3` · `grub-mkrescue` (+ `/dev/kvm` for speed, TCG works too).

```bash
make && make iso64      # kernel + bootable ISO
./run64.sh              # interactive QEMU
./run64.sh --headless   # CI gate: boot + ~40 serial markers, zero faults
make check64            # headless gate + keyboard gate + framebuffer gate
```

`run64.sh --headless` asserts the full battery over the serial log: long-mode entry, GDT/IDT, PMM self-test, 4-CPU SMP + IPI/TLB, COW fork isolation with exact values, FPU isolation (SSE+x87), clone, exec (MCT2 + ELF64, ASLR bases distinct), shell/spawn/argv, brk demand paging (64 touched / ~65 frames), NX/RO kills (exit 139), per-CPU worker execution on all 4 cores — with zero `[FATAL]`/`[FAIL]` and no forbidden markers.

`MECTOV64_CMDLINE=noap ./run64.sh` keeps APs parked (BSP-only scheduling) for bisecting SMP-vs-core bugs. `MECTOV64_SMP=n` overrides the vCPU count.

## Syscall ABI (M4 ABI0, `int $0x80`)

`EAX` = number, `EBX/ECX/EDX/ESI/EDI` = args (full 64-bit regs, canonical user VAs), `RAX` = result (negative = `-errno`).

| # | Name | Args |
|---|------|------|
| 1 | PRINT | ptr, len (≤2048, US-mapped) |
| 8 | GET_TICKS | — |
| 9 | YIELD | — |
| 10 | EXIT | status (zombie until reaped) |
| 19 | SLEEP | ticks |
| 20 | GET_PID | — |
| 71 | FORK | COW clone → child id / 0 |
| 72 | WAITPID | pid (-1 any), status_ptr, WNOHANG |
| 76 | EXEC | embedded image name |
| 104 | CLONE | func VA (shared address space) |
| 120 | BRK | new_brk (0 = query) |
| 131 | MEMINFO | ptr{total,free} |
| 132 | GETBASE | own image base (ASLR proof) |
| 133 | GETCPU | current cpu index |
| 134 | PS | ptr, max → count filled |
| 135 | GETCHAR | nonblocking key, -1 if empty |
| 136 | SPAWN | name, argc, argv |

## Known issues / future work

- One intermittent wild-frame fault (~1/6 runs) under maximum migration churn; full forensics stay in-tree (`trace_cr3` ring, NMI freeze, raw serial dumps, `sched_owner_*` monitor hooks).
- Orphan zombies accumulate until reaped (no periodic init reaper yet); `RLIMIT`-style caps absent.
- TSC-seeded ASLR is weak entropy (a CSPRNG + `getrandom` is later work).
- No filesystem, network, audio, or GUI yet — the 32-bit ancestor's subsystems (VFS/ext2/FAT32, RTL8139 stack, SB16, window manager, DOOM) are port candidates, not yet ported.
- Shell has no job control, quotes, or background `&`.
- `syscall/sysret` fast path, higher-half kernel, 5-level paging: explicitly out of scope for this bring-up.

## License

See [LICENSE](LICENSE).
