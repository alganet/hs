f() { echo two; echo three 1>&2; }
echo one > /tmp/hs-append-dup
f >> /tmp/hs-append-dup 2>&1
cat /tmp/hs-append-dup
rm -f /tmp/hs-append-dup
