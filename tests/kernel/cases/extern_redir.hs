# SPDX-FileCopyrightText: 2026 Alexandre Gomes Gaigalas <alganet@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Redirecting or capturing an external command's output is unsupported on these
# kernels (fd 0/1/2 are the console, no dup2 -- KERNEL.md K1). hs must REFUSE it
# (error + nonzero status, external not run), not run it with the redirect
# silently ineffective. Re-execs hs itself -- the only in-kernel external --
# under a redirect and a capture; both must fail and write nothing. Builtins are
# unaffected (cmdsub/pipe/redirection probes cover those). Self-checking.

hs="${HS:-./hs}"
f=/tmp/hs-extredir

# Output redirect on an external: refused (nonzero), file left empty.
"$hs" -c 'echo hi' > "$f"
if [ "$?" = 0 ]; then echo HS-KERNEL-FAIL extern-redir-status; fi
if [ -s "$f" ]; then echo HS-KERNEL-FAIL extern-redir-wrote; fi

# Capture of an external: refused, comes back empty.
v=$("$hs" -c 'echo hi')
if [ -n "$v" ]; then echo HS-KERNEL-FAIL extern-capture-nonempty; fi

# An external with NO redirect still runs (exit status propagates).
"$hs" -c 'exit 0'
if [ "$?" != 0 ]; then echo HS-KERNEL-FAIL extern-plain-runs; fi

rm -f "$f"
echo "[ok] extern_redir"
exit 0
