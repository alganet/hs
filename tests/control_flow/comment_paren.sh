# An unbalanced paren in an ordinary comment must not merge the following
# commands into one chunk. Only autoconf's `#(` balancing marker affects the
# subshell paren depth.
: # note (see below)
echo one
echo two
echo three   # trailing comment with a lone ) paren
echo four
