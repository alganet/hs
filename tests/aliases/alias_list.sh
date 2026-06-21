alias a1='echo one'
alias a2='echo two'
alias | { __n=0; while IFS= read -r __l; do __n=$((__n+1)); done; echo "$__n"; }
