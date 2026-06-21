# `2>` redirects a builtin's stderr to a file. cd to a missing dir fails and
# writes a diagnostic to stderr; with 2> that lands in the file (not the
# console). (External commands are never redirected on hs -- their stderr is
# the console -- so this exercises the builtin path; see KERNEL.md.)
cd /nonexistent-xyz-dir-sxzyq 2> /tmp/hs-err-test
if [ $? -ne 0 ]; then
    echo "cmd-failed"
fi
# A non-empty file proves the builtin's stderr was captured to it.
if [ -s /tmp/hs-err-test ]; then
    echo "err-file-nonempty"
fi
rm -f /tmp/hs-err-test
