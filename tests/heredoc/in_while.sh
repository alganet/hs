n=0
while [ "$n" -lt 2 ]; do
cat <<EOF
n is $n
EOF
n=$((n + 1))
done
