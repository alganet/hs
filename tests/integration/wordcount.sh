count_words() {
    n=0
    for w in $1; do
        n=$((n + 1))
    done
    echo $n
}
count_words "one two three"
count_words "a b c d e f g"
count_words "single"
