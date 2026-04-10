f() { echo std; echo err 1>&2; }
f 2>&1 > /tmp/hs-trap
echo "file:"
cat /tmp/hs-trap
rm -f /tmp/hs-trap
