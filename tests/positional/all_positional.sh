set -- x y z
echo "1: $1"
echo "2: $2"
echo "3: $3"
echo "count: $#"
for a in "$@"; do
    echo "item: $a"
done
