i=0
while [ $i -lt 100 ]; do
    if [ $i -eq 3 ]; then
        break
    fi
    echo $i
    i=$((i + 1))
done
echo "after: $i"
