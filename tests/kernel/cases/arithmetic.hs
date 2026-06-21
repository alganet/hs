# SPDX-FileCopyrightText: 2026 Alexandre Gomes Gaigalas <alganet@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Arithmetic expansion on a bare kernel. Self-checking.

if [ "$(( 6 * 7 ))" != 42 ]; then echo HS-KERNEL-FAIL arith-mul; fi
if [ "$(( (2 + 3) * 4 - 1 ))" != 19 ]; then echo HS-KERNEL-FAIL arith-prec; fi
if [ "$(( 17 % 5 ))" != 2 ]; then echo HS-KERNEL-FAIL arith-mod; fi
if [ "$(( 1 < 2 ))" != 1 ]; then echo HS-KERNEL-FAIL arith-lt; fi
if [ "$(( 7 & 3 ))" != 3 ]; then echo HS-KERNEL-FAIL arith-and; fi
if [ "$(( 1 << 4 ))" != 16 ]; then echo HS-KERNEL-FAIL arith-shift; fi

n=10
n=$(( n + 5 ))
if [ "$n" != 15 ]; then echo HS-KERNEL-FAIL arith-var; fi

echo "[ok] arithmetic"
exit 0
