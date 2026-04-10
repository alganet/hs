if [ foo = foo -a bar = bar ]; then echo both; fi
if [ foo = foo -o bar = baz ]; then echo either; fi
if [ ! foo = bar ]; then echo not-eq; fi
if [ foo = bar -o baz = baz ]; then echo right-true; fi
