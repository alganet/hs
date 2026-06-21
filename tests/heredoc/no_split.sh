v="a   b"
while IFS= read -r __l; do printf "%s\n" "$__l"; done <<EOF
[$v]
EOF
