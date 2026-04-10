printf 'data\n' > /tmp/hs-exttrap-in
cat /tmp/hs-exttrap-in 2>&1 > /tmp/hs-exttrap-out
echo "file:"
cat /tmp/hs-exttrap-out
rm -f /tmp/hs-exttrap-in /tmp/hs-exttrap-out
