if cd /definitely-not-a-real-directory-xyzqrs 2>/dev/null; then
    echo "should not reach"
else
    echo "cd failed"
fi
echo still-here
