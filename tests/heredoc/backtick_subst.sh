x=world
while IFS= read -r __l; do printf "%s\n" "$__l"; done <<EOF
a=`echo HELLO`
c=$(echo $x)
d=\`literal\`
e=`echo one; echo two`
EOF
