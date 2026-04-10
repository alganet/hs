cat /nonexistent-xyz-file-sxzyq 2> /tmp/hs-err-test
echo "exit: $?"
# The file should exist because 2> always creates it.
if [ -f /tmp/hs-err-test ]; then
    echo "err-file-created"
fi
rm -f /tmp/hs-err-test
