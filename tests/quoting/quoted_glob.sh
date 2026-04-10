case "*" in
    "*") echo matched-literal ;;
    *)   echo matched-wild ;;
esac
case "abc" in
    "*") echo literal ;;
    *)   echo wild ;;
esac
