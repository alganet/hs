if true; then
while IFS= read -r __l; do printf "%s\n" "$__l"; done <<EOF
inside if branch
EOF
fi
