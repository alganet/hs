i=0
while [ $i -lt 5 ]; do
    i=$((i + 1))
    if [ $i -eq 3 ]; then
        continue
    fi
    echo $i
done
