# Mectov OS 64-bit — x86-64 monolithic kernel (freestanding, no libc)
#
#   make            build the kernel (myos64.bin)
#   make iso64      build the bootable ISO (mectov64.iso, GRUB Multiboot2)
#   ./run64.sh                  boot in QEMU (q35, 4 cores, serial log)
#   ./run64.sh --headless       CI gate: boot + marker checks
#   make check64                headless gate + interactive keyboard gate
#   make clean / clean64        remove build artifacts
#
# Toolchain: gcc (-m64) · nasm · ld · qemu-system-x86_64 · python3 ·
#            grub-mkrescue. Optional: KVM (/dev/kvm) for speed.

CC64 = gcc
AS64 = nasm
LD64 = ld
CFLAGS64 = -m64 -std=gnu99 -ffreestanding -O2 -Wall -Wextra -g -march=x86-64 -mcmodel=kernel -mno-red-zone -fno-pie -fno-pic -MMD -MP
LDFLAGS64 = -m elf_x86_64 -T linker64.ld -z noexecstack
ASFLAGS64 = -f elf64
OBJ64_DIR = obj64
OBJS64 = $(OBJ64_DIR)/boot64.o $(OBJ64_DIR)/kernel64.o \
         $(OBJ64_DIR)/k64_gdt64.o $(OBJ64_DIR)/k64_idt64.o \
         $(OBJ64_DIR)/k64_isr64.o $(OBJ64_DIR)/k64_mem64.o \
         $(OBJ64_DIR)/k64_task64.o $(OBJ64_DIR)/k64_syscall64.o \
         $(OBJ64_DIR)/k64_loader64.o $(OBJ64_DIR)/k64_smp64.o \
         $(OBJ64_DIR)/k64_kbd64.o $(OBJ64_DIR)/k64_cons64.o \
         $(OBJ64_DIR)/k64_mouse64.o $(OBJ64_DIR)/k64_fb64.o \
         $(OBJ64_DIR)/font8x16.o \
         $(OBJ64_DIR)/entry64.o $(OBJ64_DIR)/tramp64_bin.o \
         $(OBJ64_DIR)/hello64_mct.o $(OBJ64_DIR)/fpu64_mct.o \
         $(OBJ64_DIR)/clone64_mct.o $(OBJ64_DIR)/forkdemo64_mct.o \
         $(OBJ64_DIR)/execdemo64_mct.o $(OBJ64_DIR)/execchild64_elf.o \
         $(OBJ64_DIR)/shell64_mct.o $(OBJ64_DIR)/argdemo64_mct.o \
         $(OBJ64_DIR)/shelltest64_mct.o $(OBJ64_DIR)/brkdemo64_mct.o \
         $(OBJ64_DIR)/nxtest64_mct.o $(OBJ64_DIR)/asldemo64_mct.o \
         $(OBJ64_DIR)/smptest64_mct.o $(OBJ64_DIR)/mousedemo64_mct.o \
         $(OBJ64_DIR)/gfxdemo64_mct.o $(OBJ64_DIR)/winsrv64_mct.o

all: myos64.bin

$(OBJ64_DIR):
	@mkdir -p $(OBJ64_DIR)

$(OBJ64_DIR)/boot64.o: boot64.asm | $(OBJ64_DIR)
	$(AS64) $(ASFLAGS64) $< -o $@

$(OBJ64_DIR)/kernel64.o: kernel64.c k64/cpu64.h | $(OBJ64_DIR)
	$(CC64) $(CFLAGS64) -c $< -o $@

$(OBJ64_DIR)/k64_%.o: k64/%.c k64/cpu64.h | $(OBJ64_DIR)
	$(CC64) $(CFLAGS64) -c $< -o $@

$(OBJ64_DIR)/entry64.o: k64/entry64.asm | $(OBJ64_DIR)
	$(AS64) $(ASFLAGS64) $< -o $@

$(OBJ64_DIR)/font8x16.o: k64/font8x16.c k64/font8x16.h | $(OBJ64_DIR)
	$(CC64) $(CFLAGS64) -c $< -o $@

# --- AP trampoline: 16-bit blob loaded at 0x8000 via SIPI ---
k64/tramp64.bin: k64/tramp64.asm
	nasm -f bin $< -o $@

$(OBJ64_DIR)/tramp64_bin.o: k64/tramp64.bin | $(OBJ64_DIR)
	objcopy -I binary -O elf64-x86-64 -B i386:x86-64 $< $@

# --- Ring-3 images: MCT2 (flat fixed-base, build_mct64.py) + one ELF64
# PIE (build_elf64.py, exec target proving the ELF loader). Raw images are
# embedded into the kernel; the loader parses them at spawn/exec time.
demos/hello64.mct: demos/hello64.c demos/sys64.h demos/entry.S scripts/build_mct64.py
	python3 scripts/build_mct64.py demos/hello64.c demos/hello64.mct 0x40000000

demos/fpu64.mct: demos/fpu64.c demos/sys64.h demos/entry.S scripts/build_mct64.py
	python3 scripts/build_mct64.py demos/fpu64.c demos/fpu64.mct 0x41000000

demos/clone64.mct: demos/clone64.c demos/sys64.h demos/entry.S scripts/build_mct64.py
	python3 scripts/build_mct64.py demos/clone64.c demos/clone64.mct 0x42000000

demos/forkdemo64.mct: demos/forkdemo64.c demos/sys64.h demos/entry.S scripts/build_mct64.py
	python3 scripts/build_mct64.py demos/forkdemo64.c demos/forkdemo64.mct 0x43000000

demos/execdemo64.mct: demos/execdemo64.c demos/sys64.h demos/entry.S scripts/build_mct64.py
	python3 scripts/build_mct64.py demos/execdemo64.c demos/execdemo64.mct 0x44000000

demos/shell64.mct: demos/shell64.c demos/sys64.h demos/entry.S scripts/build_mct64.py
	python3 scripts/build_mct64.py demos/shell64.c demos/shell64.mct 0x45000000

demos/argdemo64.mct: demos/argdemo64.c demos/sys64.h demos/entry.S scripts/build_mct64.py
	python3 scripts/build_mct64.py demos/argdemo64.c demos/argdemo64.mct 0x46000000

demos/shelltest64.mct: demos/shelltest64.c demos/sys64.h demos/entry.S scripts/build_mct64.py
	python3 scripts/build_mct64.py demos/shelltest64.c demos/shelltest64.mct 0x47000000

demos/brkdemo64.mct: demos/brkdemo64.c demos/sys64.h demos/entry.S scripts/build_mct64.py
	python3 scripts/build_mct64.py demos/brkdemo64.c demos/brkdemo64.mct 0x48000000

demos/nxtest64.mct: demos/nxtest64.c demos/sys64.h demos/entry.S scripts/build_mct64.py
	python3 scripts/build_mct64.py demos/nxtest64.c demos/nxtest64.mct 0x49000000

demos/asldemo64.mct: demos/asldemo64.c demos/sys64.h demos/entry.S scripts/build_mct64.py
	python3 scripts/build_mct64.py demos/asldemo64.c demos/asldemo64.mct 0x4A000000

demos/smptest64.mct: demos/smptest64.c demos/sys64.h demos/entry.S scripts/build_mct64.py
	python3 scripts/build_mct64.py demos/smptest64.c demos/smptest64.mct 0x4B000000

demos/mousedemo64.mct: demos/mousedemo64.c demos/sys64.h demos/entry.S scripts/build_mct64.py
	python3 scripts/build_mct64.py demos/mousedemo64.c demos/mousedemo64.mct 0x4C000000

demos/gfxdemo64.mct: demos/gfxdemo64.c demos/sys64.h demos/umalloc.h demos/entry.S scripts/build_mct64.py
	python3 scripts/build_mct64.py demos/gfxdemo64.c demos/gfxdemo64.mct 0x4D000000

demos/winsrv64.mct: demos/winsrv64.c demos/sys64.h demos/umalloc.h demos/entry.S scripts/build_mct64.py
	python3 scripts/build_mct64.py demos/winsrv64.c demos/winsrv64.mct 0x4E000000

demos/execchild64.elf: demos/execchild64.c demos/sys64.h demos/entry.S scripts/build_elf64.py
	python3 scripts/build_elf64.py demos/execchild64.c demos/execchild64.elf

$(OBJ64_DIR)/%64_mct.o: demos/%64.mct | $(OBJ64_DIR)
	objcopy -I binary -O elf64-x86-64 -B i386:x86-64 $< $@

$(OBJ64_DIR)/execchild64_elf.o: demos/execchild64.elf | $(OBJ64_DIR)
	objcopy -I binary -O elf64-x86-64 -B i386:x86-64 $< $@

-include $(OBJS64:.o=.d)

myos64.bin: $(OBJS64)
	$(LD64) $(LDFLAGS64) $(OBJS64) -o myos64.bin

iso64: myos64.bin
	@mkdir -p iso64/boot/grub
	cp myos64.bin iso64/boot/
	@printf 'set timeout=0\nset default=0\nmenuentry "Mectov OS 64" {\n    multiboot2 /boot/myos64.bin $(MECTOV64_CMDLINE)\n    boot\n}\n' > iso64/boot/grub/grub.cfg
	grub-mkrescue -o mectov64.iso iso64

clean64:
	rm -rf $(OBJ64_DIR) myos64.bin mectov64.iso iso64 serial64.log
	rm -f demos/*64.o demos/*64.elf demos/*64.bin demos/*64.mct demos/entry64.o
	rm -f k64/tramp64.bin

clean: clean64

check64: iso64
	./run64.sh --headless && python3 scripts/kbd_test.py && python3 scripts/vga_test.py && python3 scripts/mouse_test.py && python3 scripts/gfx_test.py && python3 scripts/win_test.py

check: check64

.PHONY: all clean check iso64 myos64.bin clean64 check64
