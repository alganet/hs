printf 'hi\n' | { read __l <&0; printf '%s\n' "$__l"; }
echo end
