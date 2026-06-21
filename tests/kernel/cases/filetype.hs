# SPDX-FileCopyrightText: 2026 Alexandre Gomes Gaigalas <alganet@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Data probe (not self-checking): report -e/-d/-f/-s for a directory, a
# non-empty file, an empty file, and a missing path, on whatever kernel runs
# it. The point is to compare the golden kernel (where a directory and an empty
# file both read as 0 bytes) against a POSIX host, to validate using readable
# size as the type signal for -d/-f. Emits `FILETYPE <name> e=.. d=.. f=.. s=..`.

dir="${FT_DIR:-/tmp}"
echo x > /tmp/ft-full
: > /tmp/ft-empty

for p in "$dir" /tmp/ft-full /tmp/ft-empty /tmp/ft-missing
do
	e=no; d=no; f=no; s=no
	if test -e "$p"; then e=yes; fi
	if test -d "$p"; then d=yes; fi
	if test -f "$p"; then f=yes; fi
	if test -s "$p"; then s=yes; fi
	echo "FILETYPE $p e=$e d=$d f=$f s=$s"
done

echo "[ok] filetype"
exit 0
