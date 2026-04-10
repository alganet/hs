for c in a c f h j z; do
    case "$c" in
        [a-f]) echo "$c: lower-f" ;;
        [g-m]) echo "$c: g-m" ;;
        *)     echo "$c: other" ;;
    esac
done
