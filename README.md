<!--
SPDX-FileCopyrightText: 2026 Alexandre Gomes Gaigalas <alganet@gmail.com>

SPDX-License-Identifier: GPL-3.0-or-later
-->

# hs

A small, bootstrappable POSIX-ish shell in a single C file, designed to
build under [M2-Planet](https://github.com/oriansj/M2-Planet) without
depending on libc features that early bootstrap kernels can't provide.

`hs` is not super fast, not super secure, not super comprehensive. Its
goal is to build with a very limited set of dependencies.

## Building

```
./build.sh
```

stage0-posix will be downloaded if not present at `stage0-posix-1.9.1`.

## Running a script

```
./build/hs script.sh [args...]
```

## Features

### Builtins

`true`, `false`, `:`, `echo`, `printf`, `test`, `[`, `read`, `exit`,
`cd`, `export`, `local`, `typeset`, `unset`, `set`, `shift`, `return`,
`break`, `continue`, `eval`, `.`, `source`, `alias`, `unalias`,
`command`.

### Control flow

`if ... then ... elif ... else ... fi`, `while`, `until`, `for ... in ... do ... done`,
`case ... esac`, functions via `name() { ... }`, `{ ... }` brace groups,
`( ... )` subshells (executed in the current process, no isolation),
`!` negation, `;` lists, `&&` / `||` short-circuiting, `|` pipes.

### Expansion

`$var`, `${var}`, `${#var}`, `${var:-default}`, `${var-default}`,
`${var:+alt}`, `${var+alt}`, `${var#pattern}`, `${var##pattern}`,
`${var%pattern}`, `${var%%pattern}`, positional parameters `$0..$9`,
`$@`, `$*`, `$#`, `$?`, arithmetic `$((...))` (full precedence), command
substitution `$(...)` and backticks.

### Quoting

Single quotes (literal), double quotes (with expansion), backslash
escapes.

### Redirection

`>`, `>>`, `<`, `2>`, `2>>`, explicit fd prefixes `N> file` / `N< file`,
fd duplication `N>&M` (e.g. `2>&1`, `1>&2`), including the trap form
`2>&1 > out`.

### Pattern matching (in `case`)

`*`, `?`, `[abc]`, `[a-z]`.

### Missing by design

Heredocs, process substitution, job control, `$LINENO`, `$FUNCNAME`,
arrays, `$(< file)`, `$10+` positional params, interactive-mode
features, redirection on brace/subshell groups (e.g. `{ ...; } > file`).
The shell is deliberately small.

## How piping/redirection works

All three build variants use the same strategy for redirection: no
`pipe()`, no `dup()`, no `dup2()`, no `/proc/self/fd`, no `mknod`.
The only syscalls involved are `open`, `close`, `read`, `write`,
`fork`, `execve`, and `waitpid`, used to emulate redirection features
using temporary files.

## Testing

hs has a golden-file regression suite under [tests/](tests/), driven
by a pure POSIX shell script [tests/run.sh](tests/run.sh). The same
runner exercises every build (gcc, tcc, M2-Planet) and the host
shell as an oracle. 

```
./build/hs-gcc tests/run.sh --target ./build/hs-gcc      # gcc build
./build/hs-tcc tests/run.sh --target ./build/hs-tcc      # tcc build
./build/hs     tests/run.sh --target ./build/hs          # M2-Planet build
/bin/sh        tests/run.sh --target /bin/sh             # oracle (POSIX drift check)
```

The oracle runs the same suite against the host shell. If hs and the
host shell ever produce different output for the same test, drift
shows up immediately. Pass `--filter SUBSTR` to scope a run to a
single test, `--update` to regenerate goldens, `--verbose` to print
PASS lines too. See [tests/README.md](tests/README.md).

