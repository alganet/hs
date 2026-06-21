greet() {
while IFS= read -r __l; do printf "%s\n" "$__l"; done <<EOF
hi $1
EOF
}
greet alice
greet bob
