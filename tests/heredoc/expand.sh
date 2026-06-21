x=VAL
n=42
while IFS= read -r __l; do printf "%s\n" "$__l"; done <<EOF
var=$x braced=${n} end
EOF
