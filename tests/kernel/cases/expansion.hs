# SPDX-FileCopyrightText: 2026 Alexandre Gomes Gaigalas <alganet@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Word/parameter expansion on a bare kernel. Self-checking: prints
# HS-KERNEL-FAIL <name> on mismatch and always exits 0 (so the strict
# kaem driver only aborts on a real hs crash, not on intentional checks).

x=world
greeting="hello $x"
if [ "$greeting" != "hello world" ]; then echo HS-KERNEL-FAIL expansion-concat; fi

if [ "${y:-def}" != def ]; then echo HS-KERNEL-FAIL expansion-default; fi

z=set
if [ "${z:+yes}" != yes ]; then echo HS-KERNEL-FAIL expansion-altvalue; fi

word=abcdef
if [ "${#word}" != 6 ]; then echo HS-KERNEL-FAIL expansion-length; fi

set -- a b c
if [ "$#" != 3 ]; then echo HS-KERNEL-FAIL expansion-argc; fi
if [ "$2" != b ]; then echo HS-KERNEL-FAIL expansion-arg2; fi

echo "[ok] expansion"
exit 0
