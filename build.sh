#!/bin/sh

# SPDX-FileCopyrightText: 2026 Alexandre Gomes Gaigalas <alganet@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later

# Build hs using the stage0-posix M2-Planet toolchain
set -e

ARCH="amd64"
S0="$(cd "$(dirname "$0")" && pwd)/stage0-posix-1.9.1"
M2="${S0}/AMD64/bin/M2-Planet"
M1="${S0}/AMD64/bin/M1"
HEX2="${S0}/AMD64/bin/hex2"
BLOOD="${S0}/AMD64/bin/blood-elf"
M2LIBC="${S0}/M2libc"
SRCDIR="$(cd "$(dirname "$0")" && pwd)"
OUTDIR="${SRCDIR}/build"
mkdir -p "${OUTDIR}"

# Verify tools exist
for tool in "$M2" "$M1" "$HEX2" "$BLOOD"; do
    if ! test -x "$tool"; then
        curl -LO https://github.com/oriansj/stage0-posix/releases/download/Release_1.9.1/stage0-posix-1.9.1.tar.gz
        tar -xzf stage0-posix-1.9.1.tar.gz -C "$(dirname "$0")"
        cd stage0-posix-1.9.1 && ./bootstrap-seeds/POSIX/AMD64/kaem-optional-seed
        cd ..
    fi
done

# Parse flags. --arch <name> cross-compiles via M2-Planet for a little-endian
# 64-bit M2libc target (amd64 default, riscv64, aarch64); the host
# M2-Planet/M1/hex2 tools target it via --architecture, with the --64 and
# --little-endian flags below hardcoded for that class. Cross outputs go to
# build/hs-<arch> so build/hs (amd64) is never clobbered.
CC=""
while [ $# -gt 0 ]; do
    case "$1" in
    --gcc)     CC="gcc";;
    --tcc)     CC="tcc";;
    --chibicc) CC="chibicc";;
    --arch)
        if [ $# -lt 2 ] || [ -z "$2" ]; then
            echo "build.sh: --arch requires an architecture name (e.g. riscv64, aarch64)" >&2
            exit 2
        fi
        ARCH="$2"; shift;;
    --arch=*)
        ARCH="${1#--arch=}"
        if [ -z "$ARCH" ]; then
            echo "build.sh: --arch= requires a non-empty architecture name" >&2
            exit 2
        fi;;
    esac
    shift
done
BIN="hs"
test "$ARCH" = "amd64" || BIN="hs-${ARCH}"

# Standard compiler path (GCC, tcc, chibicc)
if test -n "$CC"; then
    echo "==> ${CC}: compiling hs.c"
    case "$CC" in
    gcc)
        gcc -static -O2 -o "${OUTDIR}/hs-gcc" "${SRCDIR}/hs.c" "${SRCDIR}/bootstrappable.c"
        ;;
    tcc)
        tcc -o "${OUTDIR}/hs-tcc" "${SRCDIR}/hs.c" "${SRCDIR}/bootstrappable.c"
        ;;
    chibicc)
        chibicc -o "${OUTDIR}/hs-chibicc" "${SRCDIR}/hs.c" "${SRCDIR}/bootstrappable.c"
        ;;
    esac
    chmod 755 "${OUTDIR}/hs-${CC}"
    echo "==> Built: ${OUTDIR}/hs-${CC}"
    exit 0
fi

echo "==> M2-Planet: compiling hs.c"
"$M2" --architecture ${ARCH} \
    -f "${M2LIBC}/sys/types.h" \
    -f "${M2LIBC}/stddef.h" \
    -f "${M2LIBC}/${ARCH}/linux/fcntl.c" \
    -f "${M2LIBC}/fcntl.c" \
    -f "${M2LIBC}/sys/utsname.h" \
    -f "${M2LIBC}/${ARCH}/linux/unistd.c" \
    -f "${M2LIBC}/${ARCH}/linux/sys/stat.c" \
    -f "${M2LIBC}/ctype.c" \
    -f "${M2LIBC}/stdlib.c" \
    -f "${M2LIBC}/stdarg.h" \
    -f "${M2LIBC}/stdio.h" \
    -f "${M2LIBC}/stdio.c" \
    -f "${M2LIBC}/string.c" \
    -f "${M2LIBC}/bootstrappable.c" \
    -f "${SRCDIR}/hs.c" \
    --debug \
    -o "${OUTDIR}/${BIN}.M1"

echo "==> blood-elf: generating debug stubs"
"$BLOOD" --64 --little-endian \
    -f "${OUTDIR}/${BIN}.M1" \
    -o "${OUTDIR}/${BIN}-footer.M1"

echo "==> M1: assembling"
"$M1" --architecture ${ARCH} \
    --little-endian \
    -f "${M2LIBC}/${ARCH}/${ARCH}_defs.M1" \
    -f "${M2LIBC}/${ARCH}/libc-full.M1" \
    -f "${OUTDIR}/${BIN}.M1" \
    -f "${OUTDIR}/${BIN}-footer.M1" \
    -o "${OUTDIR}/${BIN}.hex2"

echo "==> hex2: linking"
"$HEX2" --architecture ${ARCH} \
    --little-endian \
    --base-address 0x00600000 \
    -f "${M2LIBC}/${ARCH}/ELF-${ARCH}-debug.hex2" \
    -f "${OUTDIR}/${BIN}.hex2" \
    -o "${OUTDIR}/${BIN}"

chmod 755 "${OUTDIR}/${BIN}"
echo "==> Built: ${OUTDIR}/${BIN}"
