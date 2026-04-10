x=global
f() {
    local x=local
    echo "in-fn: $x"
}
f
echo "out: $x"
