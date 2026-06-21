# SPDX-FileCopyrightText: 2026 Alexandre Gomes Gaigalas <alganet@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
#
# fork + execve + waitpid status propagation, WITHOUT any fd remapping or
# capture. Isolates the process-control path from the fd-allocation bug that
# breaks command substitution: if this passes but cmdsub fails, the fault is
# squarely fd allocation, not fork/exec/wait. Re-execs hs itself (the only
# external program in-kernel). Self-checking.

hs="${HS:-./hs}"

"$hs" -c 'exit 0'
if [ "$?" != 0 ]; then echo HS-KERNEL-FAIL exec-status-0; fi

"$hs" -c 'exit 7'
if [ "$?" != 7 ]; then echo HS-KERNEL-FAIL exec-status-7; fi

"$hs" -c 'true'
if [ "$?" != 0 ]; then echo HS-KERNEL-FAIL exec-true; fi

"$hs" -c 'false'
if [ "$?" != 1 ]; then echo HS-KERNEL-FAIL exec-false; fi

echo "[ok] exec_status"
exit 0
