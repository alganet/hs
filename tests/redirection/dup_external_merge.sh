printf 'data\n' > /tmp/hs-extmerge-in
cat /tmp/hs-extmerge-in > /tmp/hs-extmerge-out 2>&1
cat /tmp/hs-extmerge-out
rm -f /tmp/hs-extmerge-in /tmp/hs-extmerge-out
