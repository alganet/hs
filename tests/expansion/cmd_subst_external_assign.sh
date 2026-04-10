printf 'hello\n' > /tmp/hs-cs-assign
x=$(cat /tmp/hs-cs-assign)
y=$(cat /tmp/hs-cs-assign)
echo "$x $y"
rm -f /tmp/hs-cs-assign
