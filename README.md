<!--
SPDX-FileCopyrightText: 2026 Alexandre Gomes Gaigalas <alganet@gmail.com>

SPDX-License-Identifier: GPL-3.0-or-later
-->

# hs

A small, bootstrappable POSIX-ish shell in a single C file, designed to
build under [M2-Planet](https://github.com/oriansj/M2-Planet) without
depending on libc features that early bootstrap kernels can't provide.

`hs` is not super fast, not super secure, not super comprehensive. Its
goal is to build from a very limited set of dependencies.

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
fd duplication `N>&M` and `N<&M` (e.g. `2>&1`, `1>&2`, `exec 7<&0`),
including the trap form `2>&1 > out`. Redirection also works on compound
groups (`{ ...; } > file`). A redirection whose target can't be opened
fails the command with a nonzero status instead of running it anyway.

Redirection and capture apply to **shell-managed output** — builtins,
subshells, and pipelines. **External commands cannot be redirected**, and hs
**refuses** rather than pretend: running an external whose stdin/stdout/stderr
is bound to a file, pipe, or `$(...)` capture (including inside a redirected
group) fails with a diagnostic and a nonzero status, and the external is not
run. An external with no redirect runs normally. See "How piping/redirection
works" below for why.

### Heredocs

`<<WORD` and `<<-WORD` (the dash form strips leading tabs). A quoted
delimiter (`<<'EOF'`) makes the body literal; an unquoted one expands
`$var`, `$(...)`, and backticks.

### Pattern matching (in `case` and `${v#pat}`/`${v%pat}`)

`*`, `?`, `[abc]`, `[a-z]`, `[!abc]` (negation); a leading `]` in a
class (`[]ab]`) is a literal member, per POSIX. This is **string** matching
only — see below; `*`/`?`/`[` are *not* expanded against filenames.

### Missing by design

Process substitution, job control, `$LINENO`, `$FUNCNAME`, arrays,
`$(< file)`, `$10+` positional params, and interactive-mode features.

**Redirecting or capturing an external command's output** (`cmd > file`,
`$(cmd)`, `cmd | builtin` for an external `cmd`) is unsupported: the target
kernels route writes to fd 0/1/2 straight to the console and provide no
`dup2`, so an exec'd program's stdout/stderr cannot be repointed. hs captures
its own output (builtins, subshells, pipelines) instead; an external with a
redirect or capture is **refused with an error** rather than run with the
redirect silently ineffective.

**Filename globbing** is absent: `*`, `?`, `[` stay literal in word
context, as if `set -f` were always on (`set -f`/`+f` are accepted but
inert). Pattern matching in `case` and `${v#pat}` still works — that is
string matching, not filename expansion. The **`-L`/`-h`** symlink tests
are always false; the other file tests
(`-f`/`-d`/`-s`/`-e`/`-r`/`-w`/`-x`) work.

The shell is deliberately small.

## How piping/redirection works

All builds use the same strategy for redirection: no
`pipe()`, no `dup()`, no `dup2()`, no `/proc/self/fd`, no `mknod`.
The only syscalls involved are `open`, `close`, `read`, `write`,
`fork`, `execve`, and `waitpid`, used to emulate redirection features
using temporary files.

The key consequence is that redirection only reaches output hs produces
itself. Builtins write through a tracked descriptor (whatever `open()`
returned for the redirect target), so `echo hi > file`, `x=$(echo hi)`,
and `builtin | builtin` all work by pointing that descriptor at a temp
file. Subshells, `$(...)`, and pipe stages run **in-process** (not via
`fork`), so they capture the same way. But an **external** program writes
the literal fd 1/2, which the target kernels send to the console with no
way to intercept (no `dup2`, and `fork` shares the fd table). hs therefore
does not try to redirect externals at all. Because running one with an
ineffective redirect would be silently wrong (`$(cmd)` empty, `cmd > file`
empty), hs instead **refuses** such a command — diagnostic on stderr, nonzero
status, the external not run — so the limitation surfaces immediately instead
of as a mystery. An external with no redirect inherits the real descriptors
and runs normally. This is the same on the POSIX host and on the minimal
kernels, so behavior is uniform everywhere (see [KERNEL.md](KERNEL.md)).

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

