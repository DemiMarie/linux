#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# run_qemu_test.sh - Run virtio-pmem DAX self-tests inside a QEMU VM
#
# This script:
#  1. Creates a temporary file to back the virtio-pmem device.
#  2. Boots a QEMU VM with that file as a virtio-pmem device.
#  3. Runs virtio_pmem_dax_test inside the VM (via the guest init).
#  4. Reports pass/fail based on the guest exit code.
#
# Prerequisites (must be installed on the host):
#   - qemu-system-x86_64 (or QEMU_BIN env var pointing to the binary)
#   - A bootable kernel image (KERNEL_IMAGE env var, defaults to
#     /boot/vmlinuz-$(uname -r) or bzImage in the kernel tree)
#   - A root filesystem image with ndctl installed
#     (ROOTFS_IMAGE env var, or the script will build a tiny initrd)
#
# Usage:
#   ./run_qemu_test.sh [options]
#
# Options:
#   -k <kernel>    Path to kernel bzImage (default: auto-detect)
#   -r <rootfs>    Path to root filesystem image (default: build tiny initrd)
#   -m <mem_mb>    Guest RAM in MiB (default: 512)
#   -p <pmem_mb>   virtio-pmem device size in MiB (default: 64)
#   -v             Verbose QEMU output
#   -h             Show this help message
#
# The script exits with:
#   0  All tests passed
#   1  One or more tests failed
#   4  Tests skipped (missing prerequisites)

set -e

# ---------- defaults ----------
QEMU_BIN="${QEMU_BIN:-qemu-system-x86_64}"
MEM_MB="${MEM_MB:-512}"
PMEM_MB="${PMEM_MB:-64}"
VERBOSE=0
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
KERNEL_SRC_ROOT="$(cd "$SCRIPT_DIR/../../../../.." && pwd)"

# ---------- helpers ----------
info()  { echo "# $*"; }
skip()  { echo "# SKIP: $*"; exit 4; }
fail()  { echo "# FAIL: $*"; exit 1; }

usage() {
	sed -n 's/^# //p' "$0" | sed -n '/^Usage:/,/^[^ ]/p' | head -n -1
	exit 0
}

# ---------- argument parsing ----------
while getopts "k:r:m:p:vh" opt; do
	case $opt in
	k) KERNEL_IMAGE="$OPTARG" ;;
	r) ROOTFS_IMAGE="$OPTARG" ;;
	m) MEM_MB="$OPTARG" ;;
	p) PMEM_MB="$OPTARG" ;;
	v) VERBOSE=1 ;;
	h) usage ;;
	*) usage ;;
	esac
done

# ---------- locate QEMU ----------
if ! command -v "$QEMU_BIN" >/dev/null 2>&1; then
	skip "QEMU not found: $QEMU_BIN  (set QEMU_BIN or install qemu-system-x86_64)"
fi

# ---------- locate kernel ----------
if [ -z "$KERNEL_IMAGE" ]; then
	# Try the kernel build tree first, then the running kernel.
	for candidate in \
		"$KERNEL_SRC_ROOT/arch/x86/boot/bzImage" \
		"/boot/vmlinuz-$(uname -r)" \
		"/boot/vmlinuz"; do
		if [ -r "$candidate" ]; then
			KERNEL_IMAGE="$candidate"
			break
		fi
	done
fi
if [ -z "$KERNEL_IMAGE" ] || [ ! -r "$KERNEL_IMAGE" ]; then
	skip "No kernel image found.  Set KERNEL_IMAGE=/path/to/bzImage"
fi
info "Kernel: $KERNEL_IMAGE"

# ---------- build or locate initrd ----------
TMPDIR="$(mktemp -d /tmp/virtio_pmem_test.XXXXXX)"
trap 'rm -rf "$TMPDIR"' EXIT

if [ -n "$ROOTFS_IMAGE" ]; then
	if [ ! -r "$ROOTFS_IMAGE" ]; then
		fail "ROOTFS_IMAGE not readable: $ROOTFS_IMAGE"
	fi
	INITRD="$ROOTFS_IMAGE"
	info "Root filesystem: $ROOTFS_IMAGE"
else
	# Build a minimal initrd that:
	#   1. Sets up the nd (nvdimm) and dax subsystems.
	#   2. Creates a DAX namespace over the virtio-pmem region.
	#   3. Runs virtio_pmem_dax_test.
	#   4. Powers off the machine with the test exit code.
	info "Building minimal initrd in $TMPDIR ..."

	TEST_BIN="$SCRIPT_DIR/virtio_pmem_dax_test"
	if [ ! -x "$TEST_BIN" ]; then
		skip "Test binary not found: $TEST_BIN  (run 'make' in $SCRIPT_DIR first)"
	fi

	# Check for ndctl (needed to create the dax namespace)
	NDCTL_BIN="$(command -v ndctl 2>/dev/null || true)"
	if [ -z "$NDCTL_BIN" ]; then
		skip "ndctl not found.  Install ndctl and retry."
	fi

	INITRD_DIR="$TMPDIR/initrd"
	mkdir -p "$INITRD_DIR"/{bin,sbin,dev,proc,sys,run,tmp,lib,lib64,usr/lib}

	# Copy the test binary and ndctl
	cp "$TEST_BIN" "$INITRD_DIR/bin/"
	cp "$NDCTL_BIN" "$INITRD_DIR/bin/"

	# Copy required shared libraries
	copy_libs() {
		local bin="$1"
		ldd "$bin" 2>/dev/null | grep -o '/[^ ]*\.so[^ ]*' | while read -r lib; do
			[ -f "$lib" ] || continue
			dest="$INITRD_DIR${lib}"
			mkdir -p "$(dirname "$dest")"
			cp -n "$lib" "$dest" 2>/dev/null || true
		done
		# Copy the dynamic linker
		local interp
		interp=$(readelf -l "$bin" 2>/dev/null | awk '/interpreter/ {gsub(/[\\[\\]]/, ""); print $NF}')
		if [ -n "$interp" ] && [ -f "$interp" ]; then
			dest="$INITRD_DIR${interp}"
			mkdir -p "$(dirname "$dest")"
			cp -n "$interp" "$dest" 2>/dev/null || true
		fi
	}
	copy_libs "$TEST_BIN"
	copy_libs "$NDCTL_BIN"

	# Busybox for shell utilities (optional but very helpful)
	BUSYBOX="$(command -v busybox 2>/dev/null || true)"
	if [ -n "$BUSYBOX" ]; then
		cp "$BUSYBOX" "$INITRD_DIR/bin/"
		chroot "$INITRD_DIR" /bin/busybox --install -s /bin 2>/dev/null || true
	else
		# Minimal shell replacement: use the host's sh
		cp "$(command -v sh)" "$INITRD_DIR/bin/" 2>/dev/null || true
		copy_libs "$(command -v sh)"
	fi

	# Write the init script
	cat > "$INITRD_DIR/init" << 'INIT_SCRIPT'
#!/bin/sh
set -e

mount -t proc     none /proc
mount -t sysfs    none /sys
mount -t devtmpfs none /dev 2>/dev/null || \
    mknod /dev/null c 1 3

# Load required modules (if not built-in)
for mod in virtio virtio_ring virtio_pci virtio_pmem libnvdimm nd_pmem dax_pmem; do
    modprobe "$mod" 2>/dev/null || true
done

# Give udev/ueventd time to populate /dev and /sys
sleep 1

# Find the virtio-pmem region
REGION=""
for r in /sys/bus/nd/devices/region*; do
    [ -f "$r/provider" ] || continue
    if [ "$(cat "$r/provider")" = "virtio-pmem" ]; then
        REGION="$(basename "$r")"
        break
    fi
done

if [ -z "$REGION" ]; then
    echo "ERROR: no virtio-pmem region found"
    echo 1 > /proc/sysrq-trigger 2>/dev/null || true
    poweroff -f
fi

echo "Found region: $REGION"

# Create a DAX namespace over the region
ndctl create-namespace -m dax -r "$REGION" || {
    echo "ERROR: ndctl create-namespace failed"
    poweroff -f
}

# Run the test
/bin/virtio_pmem_dax_test
RC=$?

echo "Test exit code: $RC"
# Power off: the host reads the exit code from the serial log
echo "VIRTIO_PMEM_TEST_RC=$RC" > /dev/ttyS1 2>/dev/null || true
poweroff -f
INIT_SCRIPT

	chmod +x "$INITRD_DIR/init"

	# Pack the initrd
	INITRD="$TMPDIR/initrd.cpio.gz"
	(cd "$INITRD_DIR" && find . | cpio --quiet -o -H newc | gzip -9 > "$INITRD")
	info "Initrd: $INITRD ($(du -sh "$INITRD" | cut -f1))"
fi

# ---------- backing file for virtio-pmem ----------
PMEM_FILE="$TMPDIR/virtio-pmem.img"
PMEM_SIZE_BYTES=$(( PMEM_MB * 1024 * 1024 ))
truncate -s "$PMEM_SIZE_BYTES" "$PMEM_FILE"
info "virtio-pmem backing: $PMEM_FILE ($PMEM_MB MiB)"

# ---------- serial log ----------
SERIAL_LOG="$TMPDIR/serial.log"
SERIAL1_LOG="$TMPDIR/serial1.log"

# ---------- run QEMU ----------
info "Booting QEMU ..."

QEMU_EXTRA_DISPLAY="-display none"
if [ "$VERBOSE" -eq 1 ]; then
	QEMU_EXTRA_DISPLAY="-display none -monitor stdio"
fi

# Use host CPU if KVM is available, otherwise fall back to emulated max
if [ -r /dev/kvm ]; then
	QEMU_CPU=host
else
	QEMU_CPU=max
fi

# Memory backend for virtio-pmem (share=on required by QEMU)
QEMU_CMD=(
	"$QEMU_BIN"
	-machine q35,accel=kvm:tcg
	-cpu "$QEMU_CPU"
	-m "${MEM_MB}M,maxmem=$(( MEM_MB + PMEM_MB + 64 ))M"
	-kernel "$KERNEL_IMAGE"
	-initrd "$INITRD"
	-append "console=ttyS0 earlyprintk=serial,ttyS0 panic=1 quiet"
	-object "memory-backend-file,id=mem_pmem,share=on,mem-path=$PMEM_FILE,size=${PMEM_MB}M"
	-device "virtio-pmem-pci,memdev=mem_pmem,id=pmem0"
	-serial "file:$SERIAL_LOG"
	-serial "file:$SERIAL1_LOG"
	$QEMU_EXTRA_DISPLAY
	-no-reboot
)

QEMU_EXIT=0
"${QEMU_CMD[@]}" 2>"$TMPDIR/qemu_stderr.log" || QEMU_EXIT=$?

# ---------- parse result ----------
GUEST_RC=""
if [ -f "$SERIAL1_LOG" ]; then
	GUEST_RC=$(grep -o 'VIRTIO_PMEM_TEST_RC=[0-9]*' "$SERIAL1_LOG" | \
		   tail -1 | cut -d= -f2)
fi

info "--- Guest serial output ---"
cat "$SERIAL_LOG" || true
info "---------------------------"

if [ -n "$GUEST_RC" ]; then
	info "Guest test exit code: $GUEST_RC"
	exit "$GUEST_RC"
else
	info "QEMU exit code: $QEMU_EXIT"
	# Inspect the serial log for TAP output
	if grep -q "^ok " "$SERIAL_LOG" 2>/dev/null; then
		if grep -q "^not ok " "$SERIAL_LOG" 2>/dev/null; then
			fail "One or more guest tests failed"
		else
			info "All guest tests passed"
			exit 0
		fi
	else
		fail "Could not determine guest test result (QEMU exit=$QEMU_EXIT)"
	fi
fi
