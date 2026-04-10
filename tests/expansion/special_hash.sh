set -- a b c d e
echo "count=$#"
shift
echo "after-shift=$#"
set --
echo "empty=$#"
