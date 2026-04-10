result=""
for x in apple banana cherry date; do
    if [ -z "$result" ]; then
        result="$x"
    else
        result="$result,$x"
    fi
done
echo "$result"
