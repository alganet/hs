x=world
cat <<EOF
a=`echo HELLO`
b=`printf "%s\n" "HAVE_dirent.h" | sed "y%.%_%"`
c=$(echo $x)
d=\`literal\`
e=`echo one; echo two`
EOF
