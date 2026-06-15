# A child terminated by a signal reports 128 + signo (not 0), so set -e and
# `|| handler` observe the failure. SIGSEGV=11 -> 139, SIGKILL=9 -> 137.
#
# Only stdout + exit are golden-compared here: dash (the oracle) also prints a
# job-death diagnostic ("Segmentation fault (core dumped)" / "Killed") to its
# real stderr when it reaps a signal-killed foreground child, which hs does not
# implement. That diagnostic is unavoidable on the target's own stderr, so this
# test ships no .err golden (the runner skips stderr comparison when it is
# absent). If you regenerate goldens with --update, delete the recreated .err.
sh -c 'kill -SEGV $$'; echo "seg=$?"
( sh -c 'kill -KILL $$' ); echo "kil=$?"
sh -c 'exit 7'; echo "ok=$?"
