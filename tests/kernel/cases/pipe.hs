# SPDX-FileCopyrightText: 2026 Alexandre Gomes Gaigalas <alganet@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Pipelines. hs has no pipe() syscall: a `a | b` is emulated with a temp file
# and fork, so it shares the fd-allocation surface that breaks command
# substitution. Self-checking; uses only builtins on both sides.

# left side feeds the right side's stdin; the right runs in a subshell.
# The check happens inside the subshell (the only place the piped value is
# visible); a wrong value prints the FAIL marker there.
printf 'hello\n' | { read x; if [ "$x" != hello ]; then echo HS-KERNEL-FAIL pipe-read; fi; }

# multi-record pipe consumed by a counting loop (builtins only); just ensure
# the pipeline runs to completion without aborting hs.
printf 'a\nb\nc\n' | while IFS= read -r _l; do : ; done

echo "[ok] pipe"
exit 0
