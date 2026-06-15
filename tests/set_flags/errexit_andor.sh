# set -e must NOT exit when a non-final && / || operand fails (short-circuit),
# but MUST exit when the final operand fails.
( set -e; false && echo unreached; echo a )
( set -e; grep zz /dev/null && echo unreached; echo b )
( set -e; false || true; echo c )
f() { set -e; grep z /dev/null >/dev/null && return 0; echo d; }
( f )
( set -e; true && false; echo unreached ) ; echo "e=$?"
( set -e; false || false; echo unreached ) ; echo "f=$?"
