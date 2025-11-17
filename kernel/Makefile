# --- 1. Define Variables ---
DEFAULT_HOST!=../default-host.sh
HOST?=DEFAULT_HOST
HOSTARCH!=../target-triplet-to-arch.sh $(HOST)

CFLAGS?=-O2 -g
CPPFLAGS?=
LDFLAGS?=
LIBS?=

DESTDIR?=
PREFIX?=/usr/local
EXEC_PREFIX?=$(PREFIX)
BOOTDIR?=$(EXEC_PREFIX)/boot
INCLUDEDIR?=$(PREFIX)/include

# --- 2. Set Kernel-Specific Flags ---
CFLAGS:=$(CFLAGS) -ffreestanding -Wall -Wextra -fstack-protector-strong
CPPFLAGS:=$(CPPFLAGS) -D__is_kernel -Iinclude
LDFLAGS:=$(LDFLAGS)
LIBS:=$(LIBS) -nostdlib -lk -lgcc

ARCHDIR=arch/$(HOSTARCH)

# --- 3. Include Arch-Specific Config ---
include $(ARCHDIR)/make.config

# --- 4. Append Arch-Specific Flags ---
CFLAGS:=$(CFLAGS) $(KERNEL_ARCH_CFLAGS)
CPPFLAGS:=$(CPPFLAGS) $(KERNEL_ARCH_CPPFLAGS)
LDFLAGS:=$(LDFLAGS) $(KERNEL_ARCH_LDFLAGS)
LIBS:=$(LIBS) $(KERNEL_ARCH_LIBS)

# --- 5. Define Object Files ---
# This list correctly points to your C files in 'kernel/kernel/'
KERNEL_OBJS=\
$(KERNEL_ARCH_OBJS) \
core/kernel.o \
core/stdio.o \
core/gdt.o \
core/idt.o \
core/irq.o \
core/panic.o \
core/shell.o \
core/kdbg.o \
core/system.o \
core/bootinfo.o \
core/userdemo.o \
core/ssp.o \
proc/proc.o \
proc/process.o \
proc/syscall.o \
proc/proc_thunk.o \
sched/sched.o \
sched/htas.o \
sched/htas_benchmark.o \
fs/fs.o \
fs/ext2.o \
fs/elf.o \
storage/block.o \
mm/pmm.o \
mm/vmm.o \
mm/heap.o \
drivers/ata.o \
drivers/ahci.o \
drivers/pci.o \
drivers/keyboard.o \
drivers/usb_uhci.o \
drivers/usb_keyboard.o \


OBJS=\
$(ARCHDIR)/crti.o \
$(KERNEL_OBJS) \
$(ARCHDIR)/switch.o \
$(ARCHDIR)/crtn.o \

LINK_LIST=\
$(LDFLAGS) \
$(ARCHDIR)/crti.o \
$(KERNEL_OBJS) \
$(ARCHDIR)/switch.o \
$(LIBS) \
$(ARCHDIR)/crtn.o \

# --- 6. The Fix: Clear Suffixes & Define Pattern Rules ---

# THIS IS THE CRITICAL FIX:
# Clear all old suffix rules so 'make' uses our new rules.
.SUFFIXES:

# This rule knows how to build "path/to/foo.o" from "path/to/foo.c"
%.o: %.c
	$(CC) -MD -c $< -o $@ -std=gnu11 $(CFLAGS) $(CPPFLAGS)

# This rule knows how to build "path/to/foo.o" from "path/to/foo.S"
%.o: %.S
	$(CC) -MD -c $< -o $@ $(CFLAGS) $(CPPFLAGS)

# --- 7. Define Build Targets ---
.PHONY: all clean install install-headers install-kernel

all: jimir.kernel

jimir.kernel: $(OBJS) $(ARCHDIR)/linker.ld
	$(CC) -T $(ARCHDIR)/linker.ld -o $@ $(CFLAGS) $(LINK_LIST)
	grub-file --is-x86-multiboot jimir.kernel

	# crtbegin.o and crtend.o removed; not needed for freestanding kernel

clean:
	rm -f jimir.kernel
	rm -f $(OBJS) *.o */*.o */*/*.o
	rm -f $(OBJS:.o=.d) *.d */*.d */*/*.d

install: install-headers install-kernel

install-headers:
	mkdir -p $(DESTDIR)$(INCLUDEDIR)
	cp -R --preserve=timestamps include/. $(DESTDIR)$(INCLUDEDIR)/.

install-kernel: jimir.kernel
	mkdir -p $(DESTDIR)$(BOOTDIR)
	cp jimir.kernel $(DESTDIR)$(BOOTDIR)

# --- 8. Include Dependencies ---
-include $(OBJS:.o=.d)