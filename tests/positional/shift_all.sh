set -- a b c d
while [ $# -gt 0 ]; do
    echo "$1"
    shift
done
echo "done: $#"
