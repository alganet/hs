# SPDX-FileCopyrightText: 2026 Alexandre Gomes Gaigalas <alganet@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Input redirection of a REAL file into a builtin (`read < file`), as opposed to
# a here-doc. hs tracks the input fd rather than repositioning fd 0, so this is
# expected to work even where command substitution does not; confirming it
# narrows the fd bug to the capture/exec path. Reads a file the harness packed
# into the image. Self-checking.

src="${FILEIO_SRC:-/build.kaem}"
read first < "$src"
if [ -z "$first" ]; then echo HS-KERNEL-FAIL fileio-read-existing; fi

# the packed build.kaem begins with an SPDX comment line
case "$first" in
'# SPDX'*) : ;;
*) echo HS-KERNEL-FAIL fileio-read-content ;;
esac

echo "[ok] file_io"
exit 0
