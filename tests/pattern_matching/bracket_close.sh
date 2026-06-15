case "]" in []]) echo m1;; *) echo no;; esac
case "a" in []a]) echo m2;; *) echo no;; esac
case "b" in [!]]) echo m3;; *) echo no;; esac
case "]" in [!]]) echo no;; *) echo m4;; esac
