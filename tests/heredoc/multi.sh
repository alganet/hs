while IFS= read -r __l; do printf "%s\n" "$__l"; done <<A <<B
first
A
second
B
