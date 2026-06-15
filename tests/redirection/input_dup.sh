printf hi | { cat <&0; }
printf '\n'
echo end
