fact() {
    if [ $1 -le 1 ]; then
        echo 1
        return
    fi
    prev=$(fact $(($1 - 1)))
    echo $(($1 * prev))
}
for n in 1 2 3 4 5 6; do
    printf '%s! = %s\n' "$n" "$(fact $n)"
done
