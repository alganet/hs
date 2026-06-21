echo "hello world" | while IFS= read -r __l; do printf "%s\n" "$__l"; done
printf 'a\nb\nc\n' | { __n=0; while IFS= read -r __l; do __n=$((__n+1)); done; echo "$__n"; }
