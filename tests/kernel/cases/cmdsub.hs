# SPDX-FileCopyrightText: 2026 Alexandre Gomes Gaigalas <alganet@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Command substitution. hs has no pipe()/dup2(): captures go through
# temp files in /tmp and fork/exec. This exercises that path on a bare
# kernel, so it doubles as a /tmp + fork/wait smoke test. Self-checking.

v=$(echo foo)
if [ "$v" != foo ]; then echo HS-KERNEL-FAIL cmdsub-echo; fi

w=$(echo $(echo bar))
if [ "$w" != bar ]; then echo HS-KERNEL-FAIL cmdsub-nested; fi

multi=$(printf 'a\nb\nc\n')
if [ "$multi" != "a
b
c" ]; then echo HS-KERNEL-FAIL cmdsub-multiline; fi

prefix=$(echo hi)-tail
if [ "$prefix" != hi-tail ]; then echo HS-KERNEL-FAIL cmdsub-concat; fi

echo "[ok] cmdsub"
exit 0
