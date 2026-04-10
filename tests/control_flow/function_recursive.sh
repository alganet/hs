count_down() {
    if [ $1 -le 0 ]; then
        echo "done"
        return 0
    fi
    echo $1
    count_down $(($1 - 1))
}
count_down 3
