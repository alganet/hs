true
echo "status=$?"
false
echo "status=$?"
retfn() { return 7; }
retfn
echo "status=$?"
