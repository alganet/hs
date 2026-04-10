fib() {
    if [ $1 -le 1 ]; then
        echo $1
        return
    fi
    a=$(fib $(($1 - 1)))
    b=$(fib $(($1 - 2)))
    echo $((a + b))
}
for n in 0 1 2 3 4 5 6 7 8; do
    printf '%s ' "$(fib $n)"
done
echo
