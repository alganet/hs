while IFS= read -r __l; do printf "%s\n" "$__l"; done <<-EOF
	line a
		line b
	EOF
