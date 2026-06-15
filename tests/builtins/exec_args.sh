# exec must replace the process with the command AND pass its arguments
# correctly (the argv shift). Regression for M2-Planet char** pointer
# arithmetic (argv+1) not scaling.
exec echo execed a b c
echo "UNREACHED"
