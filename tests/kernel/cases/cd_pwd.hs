# SPDX-FileCopyrightText: 2026 Alexandre Gomes Gaigalas <alganet@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Working-directory control: chdir (cd) and getcwd (pwd builtin). hs's cd does
# not maintain $PWD, so this checks cd's exit status and relative resolution
# instead, plus a bare `pwd` to exercise getcwd. No fork, no fd remap.
# Self-checking.

dir="${CDDIR:-/tmp}"

if ! cd "$dir"; then echo HS-KERNEL-FAIL cd-absolute; fi
if ! cd /; then echo HS-KERNEL-FAIL cd-root; fi

# relative cd: only resolves correctly if cwd really became /
if ! cd "${dir#/}"; then echo HS-KERNEL-FAIL cd-relative; fi

# cd to a missing directory must fail (faccessat/chdir error path)
if cd /no-such-directory-xyz; then echo HS-KERNEL-FAIL cd-missing-should-fail; fi

# getcwd path: pwd must succeed
cd /
pwd
if [ "$?" != 0 ]; then echo HS-KERNEL-FAIL pwd-getcwd; fi

echo "[ok] cd_pwd"
exit 0
