while IFS= read -r __l; do printf "%s\n" "$__l"; done <<EOF > /tmp/hs-hd-combo
captured line
EOF
while IFS= read -r __l; do printf "%s\n" "$__l"; done < /tmp/hs-hd-combo
rm -f /tmp/hs-hd-combo
