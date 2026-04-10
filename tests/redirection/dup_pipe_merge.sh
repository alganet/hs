f() { echo stdout; echo stderr 1>&2; }
f 2>&1 | cat
