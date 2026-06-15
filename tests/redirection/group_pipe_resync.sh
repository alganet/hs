# A pipe (external on the right) inside a multi-command group under a group
# redirect: the builtin writes after it must not overwrite the external's
# output. Regression for the M2 build's missing lseek re-sync.
{ echo A; echo X | sed 's/.*/[&]/'; echo B; } > /tmp/hs_grp_$$
cat /tmp/hs_grp_$$
rm -f /tmp/hs_grp_$$
