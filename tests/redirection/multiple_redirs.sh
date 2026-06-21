printf 'input\n' > /tmp/hs-multi-in
while IFS= read -r __l; do printf "%s\n" "$__l"; done > /tmp/hs-multi-out < /tmp/hs-multi-in
while IFS= read -r __l; do printf "%s\n" "$__l"; done < /tmp/hs-multi-out
rm -f /tmp/hs-multi-in /tmp/hs-multi-out
