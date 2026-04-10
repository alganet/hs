for s in hello world hi abc; do
    case "$s" in
        h*)  echo "$s: starts-with-h" ;;
        *o)  echo "$s: ends-with-o" ;;
        *)   echo "$s: other" ;;
    esac
done
