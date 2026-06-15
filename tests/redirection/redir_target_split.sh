# An unquoted redirect target expanded from a variable containing whitespace
# keeps its value verbatim (no field-splitting / marker byte in the filename),
# so a later quoted read finds the same file.
d=/tmp/hs_rtsplit_$$
rm -rf "$d"; mkdir -p "$d"; cd "$d"
f="x y"
echo hi > $f
cat "$f"
echo "names:"; ls
cd /; rm -rf "$d"
