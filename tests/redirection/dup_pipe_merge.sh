f() { echo stdout; echo stderr 1>&2; }
f 2>&1 | while IFS= read -r __l; do printf "%s\n" "$__l"; done
