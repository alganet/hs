printf 'one\ntwo\nthree\n' > /tmp/hs-pipe-left-in
cat /tmp/hs-pipe-left-in | cat
rm -f /tmp/hs-pipe-left-in
