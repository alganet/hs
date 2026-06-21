# SPDX-FileCopyrightText: 2026 Alexandre Gomes Gaigalas <alganet@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Emit a builder-hex0 "src protocol" provisioning script to stdout.
#
# The script loads, into the booted kernel's in-memory filesystem, everything
# needed to bootstrap a native stage0 toolchain and compile + test hs:
#   - the stage0-posix M2libc, mescc-tools, the target arch tree, and the
#     target-native bootstrap seeds (mirrored at the kernel FS root, so the
#     stock mescc-tools-{seed,mini}-kaem.kaem relative paths work unchanged);
#   - hs.c and the in-kernel driver/build/test kaem scripts and *.hs cases.
# The final line execs the seed kaem on /driver.kaem; a trailing NUL ends
# provisioning so the kernel flushes and reboots (QEMU exits via --no-reboot).
#
# Usage: ARCH=riscv64|aarch64 sh tests/kernel/pack.sh > out.src
#
# Env:
#   ARCH      target arch (riscv64 or aarch64)            [required]
#   STAGE0    path to stage0-posix-1.9.1 tree    [default: ./stage0-posix-1.9.1]
#   HSDIR     path to hs repo root               [default: .]
#   KERNELDIR path to tests/kernel               [default: $HSDIR/tests/kernel]

set -eu

ARCH="${ARCH:?set ARCH=riscv64 or aarch64}"
HSDIR="${HSDIR:-.}"
STAGE0="${STAGE0:-${HSDIR}/stage0-posix-1.9.1}"
KERNELDIR="${KERNELDIR:-${HSDIR}/tests/kernel}"

# stage0 uses a CamelCase dir name for aarch64; riscv64/x86 are lowercase.
case "$ARCH" in
    riscv64) ARCHDIR=riscv64 ;;
    aarch64) ARCHDIR=AArch64 ;;
    x86)     ARCHDIR=x86 ;;
    *) echo "pack.sh: unsupported ARCH: $ARCH" >&2; exit 2 ;;
esac

SEEDKAEM="/bootstrap-seeds/POSIX/${ARCHDIR}/kaem-optional-seed"

# --- 1. Collect (host-path, dest-path) file pairs into a temp manifest ------
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
files="${work}/files"   # lines: <hostpath>\t<destpath>
: > "$files"

# stage0 subtrees, mirrored at root. Prune the GAS/ and Development/ alt
# implementations under the arch dir: the mini bootstrap never touches them.
( cd "$STAGE0" && find M2libc mescc-tools bootstrap-seeds/POSIX/"$ARCHDIR" -type f ) \
    | while IFS= read -r rel; do printf '%s\t/%s\n' "${STAGE0}/${rel}" "$rel"; done >> "$files"
( cd "$STAGE0" && find "$ARCHDIR" -type f \
        ! -path "${ARCHDIR}/GAS/*" ! -path "${ARCHDIR}/Development/*" ) \
    | while IFS= read -r rel; do printf '%s\t/%s\n' "${STAGE0}/${rel}" "$rel"; done >> "$files"
# M2-Planet sources: the mini chain builds M2-Planet itself from ./M2-Planet/cc*.c.
# Prune its bundled test/ and M2libc/ (the top-level M2libc above is used instead).
( cd "$STAGE0" && find M2-Planet -type f \
        ! -path 'M2-Planet/test/*' ! -path 'M2-Planet/M2libc/*' ) \
    | while IFS= read -r rel; do printf '%s\t/%s\n' "${STAGE0}/${rel}" "$rel"; done >> "$files"

# hs source + in-kernel scripts (driver/build are arch-specific; probe is not).
printf '%s\t/hs.c\n'        "${HSDIR}/hs.c"                          >> "$files"
printf '%s\t/driver.kaem\n' "${KERNELDIR}/driver.${ARCH}.kaem"       >> "$files"
printf '%s\t/build.kaem\n'  "${KERNELDIR}/build.${ARCH}.kaem"        >> "$files"
printf '%s\t/probe.kaem\n'  "${KERNELDIR}/probe.kaem"               >> "$files"
printf '%s\t/probe.hs\n'    "${KERNELDIR}/probe.hs"                 >> "$files"
for c in "${KERNELDIR}"/cases/*.hs; do
    printf '%s\t/cases/%s\n' "$c" "$(basename "$c")"                 >> "$files"
done

# --- 2. Directory set: every ancestor of every dest, plus empty build dirs --
dirs="${work}/dirs"
{
    # ancestors of each file's destination directory
    cut -f2 "$files" | while IFS= read -r dest; do
        d="${dest%/*}"
        while [ -n "$d" ]; do printf '%s\n' "$d"; d="${d%/*}"; done
    done
    # the root itself (the kernel boots with cwd=/ but no "/" file-table entry,
    # so `chdir("/")` fails until one exists -- KERNEL.md K3), the stock-empty
    # output dirs the bootstrap writes into, /cases, and /tmp (hs writes
    # capture/redirection temp files under /tmp/hs-cap-*).
    printf '/\n/%s/artifact\n/%s/bin\n/cases\n/tmp\n' "$ARCHDIR" "$ARCHDIR"
} | LC_ALL=C sort -u > "$dirs"

# --- 3. Emit: dirs first (parents precede children via lexical sort) --------
while IFS= read -r d; do
    printf 'src 0 %s\n' "$d"
done < "$dirs"

# --- 4. Emit each file: header line then exactly N raw bytes ----------------
while IFS="$(printf '\t')" read -r host dest; do
    n="$(wc -c < "$host" | tr -d ' ')"
    printf 'src %s %s\n' "$n" "$dest"
    cat "$host"
done < "$files"

# --- 5. Exec the seed kaem on the driver; NUL terminates provisioning -------
printf '%s /driver.kaem\n' "$SEEDKAEM"
printf '\000'
