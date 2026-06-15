w='$(am_libgnu_a_OBJECTS)'
echo "[${w#\$[\(\{]}]"
echo "[${w%[\)\}]}]"
v='libgnu_a-glob.$(OBJEXT)'
echo "[${v%.\$[\(\{]*}]"
case '(x' in (\$[\(\{]*) echo paren ;; (\(*) echo lit-paren ;; esac
p='foo)'
echo "[${p%\)}]"
