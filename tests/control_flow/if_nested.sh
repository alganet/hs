a=1
b=2
if [ $a -eq 1 ]; then
    if [ $b -eq 2 ]; then
        echo both
    else
        echo only-a
    fi
fi
