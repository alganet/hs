f() { echo present; }
f
unset -f f
f() { echo restored; }
f
unset -f f
f() { echo final; }
f
