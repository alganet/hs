emit_err() { echo oops 1>&2; }
emit_err 2> /tmp/hs-err-only
cat /tmp/hs-err-only
rm -f /tmp/hs-err-only
