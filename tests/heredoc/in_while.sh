n=0
while [ "$n" -lt 2 ]; do
while IFS= read -r __l; do printf "%s\n" "$__l"; done <<EOF
n is $n
EOF
n=$((n + 1))
done
