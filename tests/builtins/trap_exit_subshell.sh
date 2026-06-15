# A subshell exiting (even via explicit exit) must NOT run the parent's
# EXIT trap. autoconf relies on this: its `( ...; exit )` compile probes
# would otherwise fire `trap '... rm -f confdefs* ...' 0` and delete files.
trap 'echo PARENT_EXIT' EXIT
echo keep > trap_marker.txt
( echo in-subshell; exit 7 )
echo "subshell rc=$?"
( exit 0 )
echo "clean subshell rc=$?"
x=$(exit 3; echo unreached)
echo "cmdsub got=[$x]"
test -f trap_marker.txt && echo "marker survived"
rm -f trap_marker.txt
echo end-of-script
