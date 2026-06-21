# SPDX-FileCopyrightText: 2026 Alexandre Gomes Gaigalas <alganet@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Grow a string across many iterations to exercise the allocator / brk under
# load on the kernel. Modest size to stay quick under TCG. Self-checking.

s=
i=0
while [ "$i" -lt 500 ]; do
	s="${s}0123456789"
	i=$((i + 1))
done
if [ "${#s}" != 5000 ]; then echo HS-KERNEL-FAIL big-alloc-len; fi

# tail still intact after all the growth
if [ "${s%0123456789}" = "$s" ]; then echo HS-KERNEL-FAIL big-alloc-tail; fi

echo "[ok] big_alloc"
exit 0
