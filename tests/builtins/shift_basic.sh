set -- a b c d
echo "$1 $2 $3 $4"
shift
echo "$1 $2 $3"
shift 2
echo "$1"
