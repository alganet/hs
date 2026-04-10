if ! false; then echo neg-false; fi
if ! true; then echo wrong; else echo neg-true; fi
! true
echo "status: $?"
! false
echo "status: $?"
