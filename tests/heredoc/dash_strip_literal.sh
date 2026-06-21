x=VAL
while IFS= read -r __l; do printf "%s\n" "$__l"; done <<-'EOF'
	lit $x
	EOF
