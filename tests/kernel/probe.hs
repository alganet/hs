# SPDX-FileCopyrightText: 2026 Alexandre Gomes Gaigalas <alganet@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Master probe runner. Runs every case as an isolated child of hs itself, so a
# case that hard-fails (exits non-zero, e.g. the fd-allocation bug aborting a
# command substitution) is absorbed by `||` and the sweep continues to the next
# probe. A single kernel boot therefore surfaces every incompatibility at once.
# kaem runs this as `./hs ./probe.hs`; it always exits 0 so kaem never aborts.
#
# Each `=== name ===` marker is printed before the case runs, so even a probe
# that crashes hs without output is pinned by the last marker. A case prints
# `[ok] <name>` on success or `HS-KERNEL-FAIL <name>` on a wrong-but-survivable
# result; a hard failure shows up as `HS-PROBE-HARDFAIL <name>` here.

echo HS-PROBE BEGIN

for c in expansion control_flow arithmetic param_ops heredoc big_alloc exec_status cd_pwd test_file filetype file_io cmdsub pipe redirection extern_redir
do
	echo "=== $c ==="
	./hs "./cases/$c.hs" || echo "HS-PROBE-HARDFAIL $c"
done

echo HS-PROBE END
exit 0
