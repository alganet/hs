for s in foo_bar foo_xyz_bar baz_bar; do
    case "$s" in
        foo_*_bar) echo "$s: sandwich" ;;
        foo_*)     echo "$s: foo-prefix" ;;
        *)         echo "$s: other" ;;
    esac
done
