set -- outer1 outer2
showargs() {
    echo "fn1: $1"
    echo "fn2: $2"
    echo "fn#: $#"
}
showargs inner1 inner2 inner3
echo "after1: $1"
echo "after#: $#"
