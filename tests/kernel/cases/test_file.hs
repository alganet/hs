# SPDX-FileCopyrightText: 2026 Alexandre Gomes Gaigalas <alganet@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
#
# File predicates: test -e/-f/-d (faccessat/stat surface). Probes files that
# already exist in the kernel FS, so it is independent of the fd-allocation bug.
# Also checks the kernel distinguishes directory from regular file in stat.
# Paths are overridable so the same case validates on the host. Self-checking.

reg="${TESTFILE_REG:-/hs.c}"
dir="${TESTFILE_DIR:-/tmp}"

if test -e "$reg"; then :; else echo HS-KERNEL-FAIL test-e-existing; fi
if test -f "$reg"; then :; else echo HS-KERNEL-FAIL test-f-regular; fi
if test -e /this-does-not-exist-xyz; then echo HS-KERNEL-FAIL test-e-absent; fi

if test -d "$dir"; then :; else echo HS-KERNEL-FAIL test-d-directory; fi
if test -d "$reg"; then echo HS-KERNEL-FAIL test-d-on-regular; fi
if test -f "$dir"; then echo HS-KERNEL-FAIL test-f-on-directory; fi

echo "[ok] test_file"
exit 0
