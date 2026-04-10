# SPDX-FileCopyrightText: 2026 Alexandre Gomes Gaigalas <alganet@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later

# hs test runner. Pure POSIX shell script: drives any POSIX shell
# (including hs itself) against the manifest.
#
# Usage:
#     <shell> tests/run.sh [--target PATH] [--filter SUBSTR]
#                          [--timeout SEC] [--update] [--verbose]
#
# Run from the project root.
#
# --target PATH    binary under test (default ./build/hs-gcc)
# --filter SUBSTR  only run tests whose manifest path contains SUBSTR
# --timeout SEC    per-test wall-clock limit (default 10s, 0 disables)
# --update         on mismatch, overwrite goldens with actual output
# --verbose, -v    print PASS lines (not only FAIL)
#
# Oracle: pass --target /bin/sh to verify the suite matches POSIX
# behavior. Any drift between hs and the host shell shows up here.

target="./build/hs-gcc"
filter=""
update=0
verbose=0
timeout_sec=10

while [ "$#" -gt 0 ]
do
    case "$1" in
        --target)  target="$2"; shift; shift ;;
        --filter)  filter="$2"; shift; shift ;;
        --timeout) timeout_sec="$2"; shift; shift ;;
        --update)  update=1;    shift ;;
        --verbose) verbose=1;   shift ;;
        -v)        verbose=1;   shift ;;
        -h)        printf '%s\n' "Usage: tests/run.sh [--target PATH] [--filter SUBSTR] [--timeout SEC] [--update] [--verbose]"; exit 0 ;;
        --help)    printf '%s\n' "Usage: tests/run.sh [--target PATH] [--filter SUBSTR] [--timeout SEC] [--update] [--verbose]"; exit 0 ;;
        *)         printf 'run.sh: unknown arg: %s\n' "$1"; exit 2 ;;
    esac
done

work="/tmp/hs-test-run"
rm -rf "$work"
mkdir -p "$work"
out_file="$work/out"
err_file="$work/err"

total=0
passed=0
failed=0
updated=0

# We deliberately read file contents via $(cat ...) instead of
# `read -r VAR < file`, because hs uses buffered FILE* I/O and the
# in-process redirection does not discard the FILE*'s internal buffer,
# corrupting subsequent reads. Command substitution forks, so the
# captured child has its own buffer.
run_one_test() {
    _test_path="$1"
    total=$((total + 1))

    if [ ! -f "$_test_path" ]; then
        printf 'MISS %s\n' "$_test_path"
        failed=$((failed + 1))
        return 0
    fi

    _base=${_test_path%.sh}
    _gold_out="${_base}.out"
    _gold_err="${_base}.err"
    _gold_exit="${_base}.exit"

    # stdin is /dev/null so the child cannot steal the runner's input
    # stream. timeout guards against hangs and runaways.
    if [ "$timeout_sec" != "0" ]; then
        timeout "${timeout_sec}s" "$target" "$_test_path" < /dev/null > "$out_file" 2> "$err_file"
    else
        "$target" "$_test_path" < /dev/null > "$out_file" 2> "$err_file"
    fi
    _actual_exit=$?

    _expected_exit=0
    if [ -f "$_gold_exit" ]; then
        _expected_exit=$(cat "$_gold_exit")
    fi

    if [ "$update" = "1" ]; then
        cp "$out_file" "$_gold_out"
        _err_bytes=$(wc -c < "$err_file")
        case "$_err_bytes" in
            *[1-9]*) cp "$err_file" "$_gold_err" ;;
            *)       rm -f "$_gold_err" ;;
        esac
        printf '%s\n' "$_actual_exit" > "$_gold_exit"
        updated=$((updated + 1))
        printf 'UPDT %s\n' "$_test_path"
        return 0
    fi

    _fail=""

    if [ ! -f "$_gold_out" ]; then
        _fail="missing golden .out"
    else
        cmp -s "$out_file" "$_gold_out"
        if [ "$?" != "0" ]; then _fail="stdout mismatch"; fi
    fi

    if [ -z "$_fail" ]; then
        if [ -f "$_gold_err" ]; then
            cmp -s "$err_file" "$_gold_err"
            if [ "$?" != "0" ]; then _fail="stderr mismatch"; fi
        fi
    fi

    if [ -z "$_fail" ]; then
        if [ "$_actual_exit" != "$_expected_exit" ]; then
            _fail="exit code want=${_expected_exit} got=${_actual_exit}"
        fi
    fi

    if [ -z "$_fail" ]; then
        passed=$((passed + 1))
        if [ "$verbose" = "1" ]; then
            printf 'PASS %s\n' "$_test_path"
        fi
    else
        failed=$((failed + 1))
        printf 'FAIL %s: %s\n' "$_test_path" "$_fail"
        case "$_fail" in
            "stdout mismatch")
                printf '  --- expected stdout ---\n'
                sed 's/^/  | /' "$_gold_out"
                printf '  --- actual stdout ---\n'
                sed 's/^/  | /' "$out_file"
                ;;
            "stderr mismatch")
                printf '  --- expected stderr ---\n'
                sed 's/^/  | /' "$_gold_err"
                printf '  --- actual stderr ---\n'
                sed 's/^/  | /' "$err_file"
                ;;
        esac
    fi
}

# Iterate the manifest one line at a time via sed. We avoid capturing
# the whole manifest into a single $() because expand_word's output
# buffer is sized from the input-word length, not the captured output
# length, and a multi-kilobyte capture would overflow it.

manifest_path="tests/MANIFEST"
if [ ! -f "$manifest_path" ]; then
    printf 'run.sh: manifest not found: %s\n' "$manifest_path"
    exit 2
fi

manifest_lines=$(wc -l < "$manifest_path")
i=1
while [ "$i" -le "$manifest_lines" ]
do
    entry=$(sed -n "${i}p" "$manifest_path")
    i=$((i + 1))

    if [ -z "$entry" ]; then continue; fi
    case "$entry" in
        "#"*) continue ;;
    esac

    if [ -n "$filter" ]; then
        case "$entry" in
            *"$filter"*) : ;;
            *) continue ;;
        esac
    fi

    run_one_test "$entry"
done

printf '\n'
printf '==========================================\n'
printf '  target:  %s\n' "$target"
printf '  total:   %s\n' "$total"
printf '  passed:  %s\n' "$passed"
printf '  failed:  %s\n' "$failed"
if [ "$update" = "1" ]; then
    printf '  updated: %s\n' "$updated"
fi
printf '==========================================\n'

if [ "$failed" = "0" ]; then
    exit 0
fi
exit 1
