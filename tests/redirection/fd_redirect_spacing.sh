# A digit word SEPARATED by whitespace from a redirect operator is a normal
# argument, not an fd specifier -- POSIX only treats a digit as an fd when it is
# ATTACHED (`2>file`, no space). Attached fd redirects must still work.
d=/tmp/hs_fdsp_$$
rm -rf "$d"; mkdir -p "$d"; cd "$d"
echo 5 > a; cat a                 # spaced: "5" is echo's argument
printf '%s\n' 12 34 > b; cat b     # spaced digit args, redirect after
echo hello 9 > c; cat c            # trailing digit arg, not an fd
echo two 2>err; echo "fd2-attached-ok"
echo one 1>d; cat d                # attached fd 1
echo dup 2>&1 1>e; cat e           # attached fd + dup
cd /; rm -rf "$d"
# N<&M / N>&M fd-dup forms (autoconf's `exec 7<&0` idiom) must parse as
# redirects, not leak the digit as a command name.
( exec 0</dev/null; echo "exec-in-redir-ok" )
( exec 7<&0 </dev/null; echo "in-dup-ok" )
( exec 3>&1; echo "out-dup-ok" )
