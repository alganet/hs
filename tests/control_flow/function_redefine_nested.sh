g() { echo g-original; }
f() {
    g() { echo g-from-f; }
}
g
f
g
g() { echo g-final; }
g
