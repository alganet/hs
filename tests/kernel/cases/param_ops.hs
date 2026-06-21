# SPDX-FileCopyrightText: 2026 Alexandre Gomes Gaigalas <alganet@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Prefix/suffix parameter expansion (pure string ops, no syscalls): isolates
# M2-Planet codegen from kernel behaviour. Self-checking.

p=/usr/local/bin
if [ "${p##*/}" != bin ]; then echo HS-KERNEL-FAIL param-basename; fi
if [ "${p%/*}" != /usr/local ]; then echo HS-KERNEL-FAIL param-dirname; fi
if [ "${p#/usr/}" != local/bin ]; then echo HS-KERNEL-FAIL param-prefix; fi

f=archive.tar.gz
if [ "${f%%.*}" != archive ]; then echo HS-KERNEL-FAIL param-stripall-suffix; fi
if [ "${f%.gz}" != archive.tar ]; then echo HS-KERNEL-FAIL param-strip-one-suffix; fi
if [ "${f#*.}" != tar.gz ]; then echo HS-KERNEL-FAIL param-strip-one-prefix; fi

echo "[ok] param_ops"
exit 0
