for i in 1 2 3; do
while IFS= read -r __l; do printf "%s\n" "$__l"; done <<EOF
iteration $i
EOF
done
