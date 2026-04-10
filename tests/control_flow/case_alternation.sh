for d in Mon Tue Wed Sat Sun; do
    case "$d" in
        Sat|Sun)     echo "$d: weekend" ;;
        Mon|Tue|Wed|Thu|Fri) echo "$d: weekday" ;;
    esac
done
