# SPDX-FileCopyrightText: 2026 Alexandre Gomes Gaigalas <alganet@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Boot a builder-hex0-arch kernel under QEMU, bootstrap a native stage0
# toolchain inside it, compile hs there, and run the kernel test cases.
# Pass = the serial console shows "HS-KERNEL-OK <arch>" and no
# "HS-KERNEL-FAIL".
#
# Usage: ARCH=riscv64|aarch64 [BOARD=virt] sh tests/kernel/run.sh
#
# Env:
#   ARCH          riscv64 or aarch64                      [required]
#   BOARD         board (only virt is supported)          [default: virt]
#   QEMU          qemu-system binary            [default: qemu-system-<arch>]
#   QEMU_TIMEOUT  seconds before QEMU is killed            [default: 1800]
#   BUILDER       builder-hex0-arch checkout    [default: ../builder-hex0-arch]
#   STAGE0        stage0-posix tree           [default: ./stage0-posix-1.9.1]

set -eu

ARCH="${ARCH:?set ARCH=riscv64, aarch64, or x86}"
# Default board per arch: riscv64/aarch64 boot the `virt` kernel via -kernel;
# x86 boots the `bios` kernel as an MBR.
case "$ARCH" in
    x86) BOARD="${BOARD:-bios}"; SEEDDIR=x86 ;;
    riscv64) BOARD="${BOARD:-virt}"; SEEDDIR=riscv64 ;;
    aarch64) BOARD="${BOARD:-virt}"; SEEDDIR=AArch64 ;;
    *) echo "run.sh: unsupported ARCH: $ARCH" >&2; exit 2 ;;
esac
QEMU_TIMEOUT="${QEMU_TIMEOUT:-1800}"

KERNELDIR="$(cd "$(dirname "$0")" && pwd)"
HSDIR="$(cd "${KERNELDIR}/../.." && pwd)"
BUILDER="${BUILDER:-${HSDIR}/../builder-hex0-arch}"
STAGE0="${STAGE0:-${HSDIR}/stage0-posix-1.9.1}"
# x86 has no qemu-system-x86; use the 64-bit emulator (it runs the 32-bit kernel).
case "$ARCH" in
    x86) QEMU="${QEMU:-qemu-system-x86_64}" ;;
    *)   QEMU="${QEMU:-qemu-system-${ARCH}}" ;;
esac

STAGE2="${BUILDER}/builder-hex0-${ARCH}-stage2.hex0"
[ -f "$STAGE2" ] || { echo "run.sh: missing kernel $STAGE2 (set BUILDER)" >&2; exit 1; }
if [ ! -d "$STAGE0" ]; then
    echo "==> fetching stage0-posix-1.9.1"
    ( cd "$HSDIR" && curl -LO https://github.com/oriansj/stage0-posix/releases/download/Release_1.9.1/stage0-posix-1.9.1.tar.gz \
        && tar -xzf stage0-posix-1.9.1.tar.gz )
fi
command -v "$QEMU" >/dev/null 2>&1 || { echo "run.sh: $QEMU not found" >&2; exit 1; }

OUT="${KERNELDIR}/out"
mkdir -p "$OUT"

# --- Resolve stage1. builder-hex0-arch commits the stage1 .hex0 but gitignores
#     the assembled .bin, so we assemble it here from the .hex0. hex0 -> bin is an
#     architecture-independent transform (hex pairs -> raw bytes), so the
#     host-runnable i386 hex0-seed produces the aarch64/riscv64 stage1 byte-for-
#     byte the same as a target-native seed would. A prebuilt .bin (e.g. from a
#     local `make`) is used as-is when present. -------------------------------
STAGE1="${OUT}/${ARCH}-stage1-${BOARD}.bin"
STAGE1_HEX0="${BUILDER}/builder-hex0-${ARCH}-stage1-${BOARD}.hex0"
STAGE1_BIN="${BUILDER}/builder-hex0-${ARCH}-stage1-${BOARD}.bin"
if [ -f "$STAGE1_HEX0" ]; then
    "${STAGE0}/bootstrap-seeds/POSIX/x86/hex0-seed" "$STAGE1_HEX0" "$STAGE1"
elif [ -f "$STAGE1_BIN" ]; then
    STAGE1="$STAGE1_BIN"
else
    echo "run.sh: missing $STAGE1_HEX0 (set BUILDER)" >&2; exit 1
fi
SRC="${OUT}/${ARCH}-${BOARD}.src"
IMG="${OUT}/${ARCH}-${BOARD}.img"
LOG="${OUT}/${ARCH}-${BOARD}.qemu.log"

# --- 1. Pack the provisioning script ----------------------------------------
echo "==> packing provisioning image for ${ARCH}/${BOARD}"
ARCH="$ARCH" STAGE0="$STAGE0" HSDIR="$HSDIR" KERNELDIR="$KERNELDIR" \
    sh "${KERNELDIR}/pack.sh" > "$SRC"

# --- 2. Assemble the disk image. virt: stage2.hex0 at sector 0 (stage1 is loaded
#     via -kernel). bios: stage1 MBR + stage2.hex0 at sector 0, then src. -------
src_len="$(wc -c < "$SRC" | tr -d ' ')"
if [ "$BOARD" = bios ]; then
    head="${OUT}/${ARCH}-${BOARD}.head"
    cat "$STAGE1" "$STAGE2" > "$head"
    head_len="$(wc -c < "$head" | tr -d ' ')"
    src_sector=$(( (head_len + 511) / 512 ))
    img_kb=$(( (src_sector * 512 + src_len) / 1024 + 2048 ))
    dd if=/dev/zero of="$IMG" bs=1024 count="$img_kb" 2>/dev/null
    dd if="$head" of="$IMG" bs=512 conv=notrunc 2>/dev/null
    rm -f "$head"
else
    stage2_len="$(wc -c < "$STAGE2" | tr -d ' ')"
    src_sector=$(( (stage2_len + 511) / 512 ))
    img_kb=$(( (src_sector * 512 + src_len) / 1024 + 2048 ))
    dd if=/dev/zero of="$IMG" bs=1024 count="$img_kb" 2>/dev/null
    dd if="$STAGE2" of="$IMG" bs=512 conv=notrunc 2>/dev/null
fi
dd if="$SRC" of="$IMG" seek="$src_sector" bs=512 conv=notrunc 2>/dev/null
echo "==> image ${IMG} (${img_kb} KiB; src-script ${src_len} bytes)"

# --- 3. Boot under QEMU, capturing the console. x86 uses KVM when available
#     (native speed); the cross arches run under TCG. Always wrap in timeout. ---
accel=""
if [ "$ARCH" = x86 ] && [ -w /dev/kvm ]; then accel="--enable-kvm"; fi
echo "==> booting (${QEMU} ${accel}, timeout ${QEMU_TIMEOUT}s)"
set +e
case "${ARCH}-${BOARD}" in
    riscv64-virt)
        timeout "$QEMU_TIMEOUT" "$QEMU" -machine virt -m 2G -nographic \
            -kernel "$STAGE1" \
            -drive file="$IMG",format=raw,if=none,id=hd0 \
            -device virtio-blk-device,drive=hd0 \
            --no-reboot 2>&1 | tee "$LOG"
        ;;
    aarch64-virt)
        timeout "$QEMU_TIMEOUT" "$QEMU" -machine virt -cpu cortex-a53 -m 2G -nographic \
            -kernel "$STAGE1" \
            -drive file="$IMG",format=raw,if=none,id=hd0 \
            -device virtio-blk-device,drive=hd0 \
            --no-reboot 2>&1 | tee "$LOG"
        ;;
    x86-bios)
        timeout "$QEMU_TIMEOUT" "$QEMU" $accel -m 2G -nographic \
            -drive file="$IMG",format=raw \
            --no-reboot 2>&1 | tee "$LOG"
        ;;
    *)
        echo "run.sh: unsupported ${ARCH}-${BOARD}" >&2; exit 2 ;;
esac
set -e

# --- 4. Verdict from the serial log -----------------------------------------
# The probe sweep (probe.hs) is a discovery tool: it runs every case and prints
# `HS-PROBE END` once it finishes. Absence of that marker means the boot, the
# in-kernel stage0 bootstrap, or the hs build did not complete -- a hard error.
# Otherwise we report the kernel incompatibilities the sweep surfaced (see
# KERNEL.md): `HS-KERNEL-FAIL <name>` (survivable mismatch) and
# `HS-PROBE-HARDFAIL <name>` (a case that aborted hs). Findings make the lane
# exit non-zero so CI flags them, but the run itself is "complete".
if ! grep -aq "HS-PROBE END" "$LOG"; then
    echo "ERROR: probe sweep did not complete (no HS-PROBE END) on ${ARCH}/${BOARD}" >&2
    echo "       boot, stage0 bootstrap, or hs build failed -- see ${LOG}" >&2
    echo "----- last 40 lines of ${LOG} -----" >&2
    tail -n 40 "$LOG" >&2
    exit 2
fi

echo "Probe sweep completed on ${ARCH}/${BOARD}. Outcomes:"
grep -aoE "=== [a-z_]+ ===|\[ok\] [a-z_]+|HS-KERNEL-FAIL [a-z-]+|HS-PROBE-HARDFAIL [a-z_]+" "$LOG" \
    | sed 's/^/    /'

# `grep -c` exits 1 when there are zero matches -- the success case here -- so
# guard it against `set -e`.
findings=$(grep -acE "HS-KERNEL-FAIL|HS-PROBE-HARDFAIL" "$LOG" || true)
if [ "$findings" -gt 0 ]; then
    echo "FINDINGS: ${findings} kernel incompatibility marker(s) -- see KERNEL.md" >&2
    exit 1
fi
echo "CLEAN: no kernel incompatibilities surfaced on ${ARCH}/${BOARD}"
exit 0
