f() { echo first 1>&2; echo second; }
f 2> /tmp/hs-s2e
echo "captured:"
cat /tmp/hs-s2e
rm -f /tmp/hs-s2e
