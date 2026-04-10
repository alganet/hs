empty=""
full="x"
if [ -z "$empty" ]; then echo empty-is-empty; fi
if [ -n "$full" ]; then echo full-is-nonempty; fi
if [ -z "$full" ]; then echo wrong; else echo full-not-empty; fi
if [ -n "$empty" ]; then echo wrong; else echo empty-not-nonempty; fi
