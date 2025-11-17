#!/bin/sh
set -e

# --- 1. CLEANUP (Optional) ---
# Ensure we have a clean state
echo "Cleaning previous build..."
rm -rf isodir/
rm -rf kernel/jimir.kernel
# (We don't run the full clean.sh, just enough to reset)


# --- 2. BUILD ---
echo "Building the kernel..."
# This assumes 'headers.sh' sets $PROJECTS and $MAKE
. ./headers.sh 
for PROJECT in $PROJECTS; do
  (cd $PROJECT && DESTDIR="$SYSROOT" $MAKE install)
done
echo "Kernel build complete: kernel/jimir.kernel"


# --- 3. PREPARE ISO DIRECTORY ---
echo "Preparing ISO directory..."
# Build user programs
echo "Building user/ (userprog.elf, ush.elf)"
(cd user && ${MAKE:-make} clean && ${MAKE:-make} all)

# Create all directories needed for GRUB and the kernel
mkdir -p isodir/boot/grub/

# Copy the kernel and modules into the ISO directory
echo "Copying kernel and modules to isodir..."
cp kernel/jimir.kernel isodir/boot/
cp user/userprog.elf isodir/boot/
cp user/ush.elf isodir/boot/
if [ -f rootfs.ext2 ]; then
	cp rootfs.ext2 isodir/boot/
	ROOTFS_IMAGE="rootfs.ext2"
	HAVE_ROOTFS=1
elif [ -f rootfs.img ]; then
	cp rootfs.img isodir/boot/rootfs.ext2
	ROOTFS_IMAGE="rootfs.img"
	HAVE_ROOTFS=1
else
	ROOTFS_IMAGE=""
	HAVE_ROOTFS=0
fi

# Create the grub.cfg file inside the ISO directory
echo "Creating grub.cfg..."
cat > isodir/boot/grub/grub.cfg << EOF
set timeout=0
set default=0

# Mirror GRUB messages to serial and screen
serial --unit=0 --speed=115200 --word=8 --parity=no --stop=1
terminal_input console serial
terminal_output console serial
set pager=1

menuentry "jimirOS" {
	echo "Loading jimir.kernel via Multiboot..."
	multiboot /boot/jimir.kernel
	module /boot/userprog.elf userprog.elf
	module /boot/ush.elf ush.elf
EOF

# Add rootfs module line if available
if [ "$HAVE_ROOTFS" = "1" ]; then
	echo "	module /boot/rootfs.ext2 rootfs.ext2" >> isodir/boot/grub/grub.cfg
fi

# Close the menuentry block
cat >> isodir/boot/grub/grub.cfg << EOF
	boot
}
EOF
echo "ISO directory prepared."


# --- 4. CREATE ISO ---
echo "Creating jimir.iso..."
grub-mkrescue -o jimir.iso isodir/
echo "ISO creation complete."


# --- 5. RUN QEMU ---
echo "Booting QEMU..."

QEMU_BASE="-M pc -cdrom jimir.iso -boot d -m 32M -serial stdio -k en-us -enable-kvm"

# We are removing the explicit USB flags to fall back to the default PS/2 keyboard.
USB_FLAGS=""

# Display mode selection
# Default: GUI window mode - CLICK THE WINDOW then type!
DISPLAY_FLAGS="-display gtk,zoom-to-fit=on"

# Optional: Different scale factors
# Use: SCALE=3 ./qemu.sh for 3x size, SCALE=4 for 4x, etc.
if [ -n "$SCALE" ]; then
	DISPLAY_FLAGS="-display gtk,zoom-to-fit=on,scale=$SCALE"
fi

# Optional: Terminal mode (type in terminal instead of GUI)
# Use: NOGUI=1 ./qemu.sh
if [ "${NOGUI}" = "1" ]; then
	DISPLAY_FLAGS="-nographic"
fi

# Optional headless/curses mode to see VGA text in the terminal
# Use: CURSES=1 ./qemu.sh
if [ "${CURSES}" = "1" ]; then
	DISPLAY_FLAGS="-display curses"
fi

QEMU_FLAGS="$QEMU_BASE $DISPLAY_FLAGS $USB_FLAGS"

if [ -n "$ROOTFS_IMAGE" ]; then
	if [ "${LEGACY_IDE}" = "1" ]; then
		echo "Using legacy IDE disk attachment"
		QEMU_FLAGS="$QEMU_FLAGS -drive file=$ROOTFS_IMAGE,format=raw,if=ide,index=0,media=disk"
	else
		echo "Using AHCI disk attachment"
		QEMU_FLAGS="$QEMU_FLAGS -drive id=disk,file=$ROOTFS_IMAGE,format=raw,if=none -device ich9-ahci,id=ahci -device ide-hd,drive=disk,bus=ahci.0"
	fi
else
	echo "Warning: no rootfs image found; running without persistent disk."
fi

# Optional debug tracing (interrupts and CPU reset), keep VM open on faults
# Use: DEBUG=1 ./qemu.sh
if [ "${DEBUG}" = "1" ]; then
	QEMU_FLAGS="$QEMU_FLAGS -d int,cpu_reset -no-reboot -no-shutdown"
fi

qemu-system-$(./target-triplet-to-arch.sh $HOST) $QEMU_FLAGS