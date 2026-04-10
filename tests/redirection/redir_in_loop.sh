rm -f /tmp/hs-loop-out
for n in 1 2 3 4 5; do
    echo "line $n" >> /tmp/hs-loop-out
done
cat /tmp/hs-loop-out
rm -f /tmp/hs-loop-out
