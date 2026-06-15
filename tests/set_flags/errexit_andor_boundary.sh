# A function-call (and subshell) boundary consumes the &&/|| short-circuit
# exemption: the construct's own status is judged fresh by set -e even when its
# body's last command was a short-circuited && operand. Brace groups and loops
# stay transparent and keep propagating the exemption.
( set -e; f() { false && true; }; f; echo unreached ) ; echo "fn=$?"
( set -e; g() { false; }; g; echo unreached ) ; echo "gn=$?"
( set -e; { false && true; }; echo brace ) ; echo "br=$?"
( set -e; for x in 1; do false && true; done; echo loop ) ; echo "lo=$?"
( set -e; ( false && true ); echo unreached ) ; echo "su=$?"
