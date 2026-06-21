# SPDX-FileCopyrightText: 2026 Alexandre Gomes Gaigalas <alganet@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Here-documents feeding a builtin's stdin, with and without expansion.
# Uses `read` so no external tool is required. Self-checking.

read a <<EOF
hello
EOF
if [ "$a" != hello ]; then echo HS-KERNEL-FAIL heredoc-read; fi

name=bob
read line <<EOF
hi $name
EOF
if [ "$line" != "hi bob" ]; then echo HS-KERNEL-FAIL heredoc-expand; fi

read q <<'EOF'
no $name here
EOF
if [ "$q" != 'no $name here' ]; then echo HS-KERNEL-FAIL heredoc-quoted; fi

echo "[ok] heredoc"
exit 0
