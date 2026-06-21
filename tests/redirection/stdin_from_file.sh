printf 'first line\nsecond line\n' > /tmp/hs-stdin-test
while IFS= read -r __l; do printf "%s\n" "$__l"; done < /tmp/hs-stdin-test
rm -f /tmp/hs-stdin-test
