for f in file.txt hello.c script.sh readme; do
    case "$f" in
        *.txt) echo "$f: text" ;;
        *.c)   echo "$f: source" ;;
        *.sh)  echo "$f: shell" ;;
        *)     echo "$f: other" ;;
    esac
done
