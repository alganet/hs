MY_VAR=outer
export MY_VAR
. tests/lib/env_helper.sh
echo "from-helper: $HELPER_SAW"
echo "outer-still: $MY_VAR"
