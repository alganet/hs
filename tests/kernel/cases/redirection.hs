# SPDX-FileCopyrightText: 2026 Alexandre Gomes Gaigalas <alganet@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
#
# File redirection. hs implements `>`/`<` without dup2 (close fd then open),
# the exact pattern a minimal kernel must support. Self-checking.

f=/tmp/hs-kernel-redir

echo hello > "$f"
read got < "$f"
if [ "$got" != hello ]; then echo HS-KERNEL-FAIL redir-write-read; fi

echo world >> "$f"
n=0
while read l < "$f"; do
	n=$((n + 1))
	break
done
# append then read first line still works
read first < "$f"
if [ "$first" != hello ]; then echo HS-KERNEL-FAIL redir-append; fi

# redirect a whole block
{
	echo one
	echo two
} > "$f"
count=0
while IFS= read -r _line; do
	count=$((count + 1))
done < "$f"
if [ "$count" != 2 ]; then echo HS-KERNEL-FAIL redir-block; fi

echo "[ok] redirection"
exit 0
