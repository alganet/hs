# SPDX-FileCopyrightText: 2026 Alexandre Gomes Gaigalas <alganet@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
#
# for / while / if-elif-else / case on a bare kernel. Self-checking.

total=0
for n in 1 2 3 4; do
	total=$((total + n))
done
if [ "$total" != 10 ]; then echo HS-KERNEL-FAIL cf-for-sum; fi

i=0
acc=
while [ "$i" -lt 3 ]; do
	acc="${acc}x"
	i=$((i + 1))
done
if [ "$acc" != xxx ]; then echo HS-KERNEL-FAIL cf-while; fi

classify=
for v in 1 2 9; do
	case "$v" in
	1) classify="${classify}a" ;;
	2) classify="${classify}b" ;;
	*) classify="${classify}z" ;;
	esac
done
if [ "$classify" != abz ]; then echo HS-KERNEL-FAIL cf-case; fi

grade=fail
score=75
if [ "$score" -ge 90 ]; then
	grade=A
elif [ "$score" -ge 70 ]; then
	grade=B
else
	grade=C
fi
if [ "$grade" != B ]; then echo HS-KERNEL-FAIL cf-elif; fi

echo "[ok] control_flow"
exit 0
