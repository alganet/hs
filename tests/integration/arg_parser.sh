set -- --verbose --count 5 file.txt
verbose=0
count=0
while [ $# -gt 0 ]; do
    case "$1" in
        --verbose) verbose=1; shift ;;
        --count)   count=$2; shift; shift ;;
        *)         echo "positional: $1"; shift ;;
    esac
done
echo "verbose=$verbose"
echo "count=$count"
