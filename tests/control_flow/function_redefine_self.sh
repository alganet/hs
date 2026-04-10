f() {
    echo f-v1
    f() { echo f-v2; }
}
f
f
f
