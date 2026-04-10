printf 'line1\nline2\n' > /tmp/hs-cs-ext
x=$(cat /tmp/hs-cs-ext)
echo "got: $x"
rm -f /tmp/hs-cs-ext
