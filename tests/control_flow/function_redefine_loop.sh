i=0
while [ "$i" -lt 50 ]
do
    eval "f() { echo iter-$i; }"
    f
    i=$((i + 1))
done
