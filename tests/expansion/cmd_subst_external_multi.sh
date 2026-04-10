printf 'a\nb\nc\n' > /tmp/hs-cs-multi
lines=$(cat /tmp/hs-cs-multi)
echo "[$lines]"
rm -f /tmp/hs-cs-multi
