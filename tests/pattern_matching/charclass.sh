for c in a b c d z; do
    case "$c" in
        [abc]) echo "$c: in abc" ;;
        *)     echo "$c: not" ;;
    esac
done
