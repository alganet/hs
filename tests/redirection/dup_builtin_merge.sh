f() { echo out; echo err 1>&2; }
f > /tmp/hs-dup-bm 2>&1
cat /tmp/hs-dup-bm
rm -f /tmp/hs-dup-bm
