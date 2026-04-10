for s in a ab abc abcd; do
    case "$s" in
        ?)    echo "$s: one" ;;
        ??)   echo "$s: two" ;;
        ???)  echo "$s: three" ;;
        *)    echo "$s: many" ;;
    esac
done
