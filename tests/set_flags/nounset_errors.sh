set -u
echo "${defined:-ok-default}"
defined=hello
echo "$defined"
