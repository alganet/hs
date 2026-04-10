if test foo = foo; then echo eq; else echo ne; fi
if test foo = bar; then echo eq; else echo ne; fi
if [ "abc" = "abc" ]; then echo match; fi
if [ "abc" = "xyz" ]; then echo nope; else echo diff; fi
