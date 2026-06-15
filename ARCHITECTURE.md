<!--
SPDX-FileCopyrightText: 2026 Alexandre Gomes Gaigalas <alganet@gmail.com>

SPDX-License-Identifier: GPL-3.0-or-later
-->

# hs architecture

This is a developer-facing companion to [README.md](../README.md). The
README tells users how to build and run hs and what features it has;
this file tells contributors how the implementation is laid out, where
the load-bearing invariants live, and what the hard limits are.

`hs` is a single C source file ([hs.c](../hs.c)) plus a small vendored
library ([bootstrappable.c](../bootstrappable.c) /
[bootstrappable.h](../bootstrappable.h)) of helpers needed by builds
that don't have a full libc. The single-file design is a goal, not an
accident: the M2-Planet bootstrap path needs a small, self-contained
input, and a single .c file is the easiest thing to compile from a
seed.

## Three-stage pipeline

Each command runs through:

1. **Lexer**: [hs.c:2007 lex_tokenize](../hs.c#L2007) walks the input
   string and produces an array of tokens. The tokenizer is one-shot:
   it consumes the entire input up front before parsing starts.
2. **Parser**: recursive-descent functions in
   [hs.c:2491-3340](../hs.c#L2491-L3340) consume the token stream and
   build an AST in the node arrays (`nd_type`, `nd_a`, `nd_b`, `nd_c`,
   `nd_d`, `nd_str`, `nd_argv`, `nd_assigns`).
3. **Executor**: [hs.c:7684 exec_node](../hs.c#L7684) walks the AST
   and runs each node. Simple commands route through
   [hs.c:6765 exec_simple](../hs.c#L6765).

The same three stages run for the top-level script, for `eval`, for
`source`, and for command substitution `$(...)`. They are reentered via
`expand_and_exec` and `exec_source`.

## File-and-fork redirection

This is the single most non-obvious design choice in hs. The README's
"How piping/redirection works" section explains the user-visible
contract; the implementation reasoning is here.

hs deliberately does not use `pipe()`, `dup()`, `dup2()`, or
`/proc/self/fd`. The only syscalls it uses for I/O plumbing are
`open`, `close`, `read`, `write`, `fork`, `execve`, `waitpid`. This
constraint exists because the M2-Planet bootstrap target's libc doesn't
expose the missing syscalls reliably across stage0 hosts, and adding
them would balloon the bootstrap surface.

To emulate pipes, hs:

1. Forks a child for the left-hand side of the pipe.
2. The child closes fd 1 and `open()`s a fresh `/tmp/hs-cap-<n>` file
   so its writes go to that file.
3. The parent waits for the child, then reads the file back and
   splices it into the next stage.

[hs.c:706 make_cap_tmpfile_path](../hs.c#L706) is the temp-file
allocator. It uses `O_CREAT | O_EXCL | HS_O_NOFOLLOW` with mode 0600
and retries on `EEXIST`, so a local attacker can't win the classic
symlink-attack against a predictable filename. `HS_O_NOFOLLOW` is
defined locally in [hs.c:47](../hs.c#L47) because M2libc's `fcntl.c`
doesn't expose it.

The downsides are real and intentional:

- Pipelines are slower than real OS pipes (the data round-trips through
  the filesystem).
- Background jobs (`&`) and job control are out of scope.
- Concurrent stages of a pipeline run sequentially, not in parallel.

If you find yourself wanting `dup2`, stop and check whether the
behavior can be emulated with the existing primitives. The whole
redirection layer is built around this restriction.

Two application details worth knowing. First, a target that `open()`
can't satisfy (`> /nonexistent/out`, `< missing`) **aborts the command**
with a nonzero status and `hs: cannot open redirect target: <path>`,
rather than silently running it against the inherited fd — this is what
makes `set -e` and autoconf probes like `: > conftest` behave. Second,
input fd duplication `N<&M` (e.g. `exec 7<&0`) mirrors output `N>&M`.
Both forms are applied in **two parallel redirect loops** that must stay
in sync: the one inline in `exec_simple` (simple commands; it also
builds the child-side spec so a forked external inherits the dup) and
the one in `exec_redir` (redirected compound groups, `{ ...; } > file`).
hs tracks only fds 0/1/2, so a dup whose source/target is fd ≥3 is a
graceful no-op. When you touch redirect handling, change both loops.

## Two allocators

hs uses two custom allocators on top of `calloc`/`free`:

- **Persistent pool** ([hs.c:113 perm_init](../hs.c#L113),
  `perm_alloc`, `perm_dup`): an 8 MB bump allocator that survives
  across commands. Function-definition AST nodes and persistent
  function bodies live here.
- **Temporary arena** ([hs.c:185 tmp_init](../hs.c#L185),
  `tmp_alloc`, `tmp_reset`): a 32 MB bump allocator that gets reset
  between commands so per-command scratch (expansion buffers, parsed
  tokens, etc.) doesn't leak across statements.

Neither allocator individually frees. The temporary arena reclaims
everything in one shot via `tmp_reset()`; the persistent pool grows
forever within a script run. Code that needs to survive arena resets
either uses `perm_alloc` or escapes via `calloc`/`free` directly.

`str_dup`, `tmp_dup`, `tmp_cat2`, and `tmp_cat3` are convenience
wrappers. The bounds-checked buffer helpers (`out_put`, `out_puts`,
`lex_put`) exist because hs has many fixed-size buffers; routing writes
through a helper — or a direct `require(i < CAP, ...)` before the store —
turns a silent overflow into a loud `require()` error.

## Quoting via sentinel bytes

The lexer preserves quoting through later stages by injecting five
control bytes into the token stream:

```
Q_SQ_OPEN  = 0x01   single-quote open
Q_SQ_CLOSE = 0x02   single-quote close
Q_DQ_OPEN  = 0x03   double-quote open
Q_DQ_CLOSE = 0x04   double-quote close
Q_SPLIT    = 0x05   word-split marker (from unquoted IFS expansions)
```

(Definitions in [hs.c around line 97](../hs.c#L97).)

These sentinels let `expand_word`, `pat_match`, and `case_match` know
which characters were originally quoted without keeping a parallel
quote-tracking array. `strip_quotes` removes the sentinels for code
paths that shouldn't see them. `pat_match` uses them to make `*`/`?`
inside quotes match literally instead of as glob wildcards.

The choice of bytes 0x01-0x05 is load-bearing: they must be
characters that the user cannot insert literally (none have a normal
shell meaning) and that are below the printable ASCII range so they
don't collide with anything `expand_word` produces.

## Hard limits

These are baked into the source as `#define`s in
[hs.c:21-77](../hs.c#L21-L77) and
[hs.c:657](../hs.c#L657). Most are checked at runtime via
`require()`; hitting such a limit aborts with a specific error message.

| Constant         | Value   | What it bounds                                                                    | Tripped error                                                                                                          |
|------------------|---------|-----------------------------------------------------------------------------------|------------------------------------------------------------------------------------------------------------------------|
| `MAX_STR`        | 8 192   | Lexer word buffer; expansion atom size                                            | `hs: token too long`                                                                                                   |
| `MAX_TOK`        | 196 608 | Tokens per parse                                                                  | `tok_add: too many tokens`                                                                                             |
| `MAX_TOK_NESTED` | 4 096   | Nested-token depth used by lex stack                                              | (internal)                                                                                                             |
| `MAX_ND`         | 262 144 | AST nodes per script                                                              | `nd_new: too many nodes`                                                                                               |
| `MAX_VAR`        | 8 192   | Live shell variables                                                              | `var_set: too many variables`                                                                                          |
| `MAX_FN`         | 4 096   | Live function definitions                                                         | `fn_set: too many functions`                                                                                           |
| `MAX_AL`         | 2 048   | Live aliases                                                                      | `al_set: too many aliases`                                                                                             |
| `MAX_LOCAL`      | 4 096   | Total local-scope slots across the call stack                                     | `lsc_declare: overflow`                                                                                                |
| `MAX_ARGV`       | 1 024   | argv length per command; argv/assign slots in `parse_simple`                      | `hs: too many arguments` / `hs: too many assignments`; field-split expansion clamps in `expand_argv`                   |
| `MAX_DEPTH`      | 256     | Local-scope nesting; parser recursion; expansion recursion; `pat_match` recursion | `lsc_push: too deep`, `hs: nested commands too deep`, `hs: expansion nested too deep`, `hs: pattern too deeply nested` |
| `TMP_SIZE`       | 32 MiB  | Temporary arena size                                                              | (clamps; oldest scratch is reused)                                                                                     |
| `LEX_STACK_MAX`  | 64      | Alias-expansion recursion depth                                                   | (silently caps; recursive aliases stop)                                                                                |

`MAX_STR` bounds individual lexer words and expansion atoms, but **not**
command-substitution output: `$(...)` is captured into a buffer that
grows on demand (double-and-copy), so a `$(find ...)` larger than 8 KB is
spliced whole.

When you raise one of these, raise it for a reason: most of them are
small because the bootstrap target has tight memory and no swap.
`MAX_DEPTH` in particular is checked from four different recursive
sites (parser, `expand_word`, `pat_match`, local scopes), so anything
that approaches it should be re-thought structurally.

## Function definitions: persistent vs eval-defined

There are two storage shapes for function bodies, distinguished by the
first byte of `fn_bodies[i]`:

- **Persistent (AST-form)**: set when a function is defined at the
  top level of a script being run via `exec_source`. The body stores
  `"\x02<ast-node-index>"`. Calling the function jumps to
  [hs.c:7684 exec_node](../hs.c#L7684) at the stored index. The AST
  itself lives in the persistent pool.
- **Eval-defined (string-form)**: set when a function is defined
  inside `eval`, `.`/`source` of a string, command substitution, or
  any other reentrant parse. The body stores the source text of the
  function body (the `{ ... }` slice extracted from the eval'd
  source. See [parse_simple in hs.c around line 3031](../hs.c#L3031)).
  Calling the function re-tokenizes and re-parses that text via
  `expand_and_exec`.

The string-form has a subtlety: when the eval'd function is called,
the parser re-runs the function definition from the body string. That
re-definition must be a no-op rather than a "rotate" (free old, install
new), because `lex_orig_src` aliases the slot's current contents.
[hs.c:1066 fn_replace_body](../hs.c#L1066) detects the
`fn_bodies[i] == body` self-aliased case and returns without touching
the slot. This is the single non-obvious invariant in the function
table; without it, calling an eval-defined function frees the source
the lexer is currently reading from.

A second subtlety: the body string stores **just the brace group**
(`{ ... }`), not the surrounding eval'd context. Storing the whole
context would re-execute everything around the function on every call,
which infinite-recurses if the surrounding context contains a call to
the same function. The slicing happens in
[parse_simple](../hs.c#L3031) using `tok_src_start[]` to find the body
range and `memcpy` to copy it out.

The tests in
[tests/control_flow/function_redefine_*.sh](../tests/control_flow/)
exercise both forms (basic, eval, source, nested, self-redefining,
loop, unset).

## Parser error diagnostics

Parser errors go through [hs.c:2415 parse_err](../hs.c#L2415), which
reports `hs: <path>:<line>:<col>: <msg>` and a snippet of the offending
line with a caret. The line number is global to the script: hs's
`exec_source` chops the script into per-command chunks and parses each
chunk separately, so it tracks `lex_line_offset` to translate
chunk-local line numbers back to file-global ones. `lex_orig_path` is
set to the script path during `exec_source`, or `NULL` for `eval` /
`expand_and_exec` strings (in which case the diagnostic skips the path
and just shows `line:col`).

If you add a new parse error site, capture the relevant token's source
position via `cur_tok_pos()` *before* calling `tok_next_val`, then call
`parse_err(pos, msg)`. Don't `sh_err_puts` directly, the snippet
formatting belongs in one place.

## Build matrix

[build.sh](../build.sh) supports four targets. They differ in what
`bootstrappable.c` they consume, which matters for any fix that lives
in that file:

| Target                | Compiler           | bootstrappable.c source                         |
|-----------------------|--------------------|-------------------------------------------------|
| `m2-planet` (default) | M2-Planet pipeline | upstream copy from `${M2libc}/bootstrappable.c` |
| `--gcc`               | gcc -static -O2    | local file at repo root                         |
| `--tcc`               | tcc                | local file at repo root                         |
| `--chibicc`           | chibicc            | local file at repo root                         |

This means a fix to the **local** `bootstrappable.c` only reaches the
GCC/TCC/chibicc builds. The M2-Planet build keeps using upstream's
copy. If a bug needs to be fixed across all four targets, the fix has
to live in `hs.c` or be sent upstream to M2-Planet first.

CI exercises all four target builds plus an oracle run against
`/bin/sh`, plus a sanitizer build (`gcc -fsanitize=address,undefined`)
in [.github/workflows/ci.yml](../.github/workflows/ci.yml). The
sanitizer job is the most useful one for catching regressions in the
buffer-handling, recursion, and ownership code.

## M2-Planet portability constraints

The M2-Planet target compiles a strict C subset and links against
M2libc, so `hs.c` avoids several things the other compilers accept:

- **No `?:` ternary** — write it as `if`/`else`.
- **No static globals with initializers and no local arrays** — globals
  are bare pointers `calloc`'d in `main`/`*_init`; scratch comes from the
  `tmp` arena (`tmp_alloc`). Even a local `char buf[N]` has corrupted the
  stack under M2-Planet; use a heap/arena buffer instead.
- **Pointer arithmetic on multi-byte element types is NOT scaled.** For a
  `char**` (or `int*`, etc.) both `p + 1` and `&p[1]` advance by *one byte*,
  not one element — only the rvalue index `p[i]` is scaled correctly. So
  `execve(path, argv + 1, envp)` passes a misaligned argv and fails. Shift an
  argv by building a fresh array through indexing instead (`argv_drop`). This
  one is nasty because `p[i]` reads fine, so the type looks healthy until you
  do explicit arithmetic on it.
- **`&&` and `||` are NOT short-circuited** — M2-Planet evaluates *both*
  operands every time, so a single condition behaves differently under M2
  than under gcc/tcc. Two rules follow. (1) If the right operand is unsafe
  when the left short-circuits, split it: `while (b >= 0 && strcmp(results[b],
  x) > 0)` dereferences `results[-1]` once `b` hits `-1` under M2, so write
  `while (b >= 0) { if (strcmp(results[b], x) <= 0) break; ... }`. (2) If the
  right operand has a side effect the program needs (e.g. a parser call that
  advances a cursor), evaluate it into a temp first so it always runs:
  `int r = f(); left = bool_to_int(left && r);` — otherwise gcc/tcc skip it.
- **No inline asm, no raw syscall numbers, no struct offsets.** Every kernel
  interaction goes through a plain M2libc wrapper
  (`open`/`read`/`write`/`close`/`lseek`/`fork`/`execve`/`waitpid`/`access`/
  `chdir`/`getcwd`/`unlink`/`exit`, plus `brk` under `calloc`), so one source
  compiles and runs on any M2-Planet target — `./build.sh --arch riscv64` (or
  `aarch64`) cross-builds an ELF that runs under qemu-user. The only `#ifndef
  __M2__` left is the `<sys/wait.h>` include, a hosted-vs-M2libc header choice,
  not architecture-specific.

  Two shell features need kernel calls M2libc doesn't wrap, so hs does without
  them:
  - **No filename globbing.** Listing a directory needs `getdents64`, which has
    no portable libc form, so `*`, `?`, `[` are literal in word context (as if
    `set -f` were always on; `set -f`/`+f` are accepted but inert). Pattern
    matching in `case` and `${v#pat}`/`${v%pat}` is unaffected — that is pure
    string matching (`pat_match`), no syscalls.
  - **No `-L`/`-h` symlink test** (needs `lstat`); both answer false.

  The file-type tests share one probe (`path_probe`): `open(O_NONBLOCK)` + a
  1-byte `read`. `-f` is read `>= 0` (a readable file), `-d` is read `< 0` (a
  directory fd fails `read` with `EISDIR`), `-s` is read `>= 1` (non-empty) — one
  `open` feeds all three. `-e`/`-r`/`-w`/`-x` use `access`. The probe can't
  classify an unreadable file (open fails → `-f`/`-d` false) or reliably tell a
  FIFO/device from a directory (`EAGAIN` reads like `EISDIR`); those need stat.
  (`lseek` is used only to re-sync a redirected fd to EOF after an external wrote
  to the same file through its own fd, so a later builtin write in
  `{ echo A; echo X | sed …; echo B; } > f` doesn't overwrite it.)

## Test layout

Tests live under [tests/](../tests/) and are listed in
[tests/MANIFEST](../tests/MANIFEST). The runner is
[tests/run.sh](../tests/run.sh), a pure POSIX shell script that takes
a `--target` argument so the same suite runs against any shell. Each
test has golden `.out`, optional `.err`, and `.exit` siblings; the
runner compares actual output byte-for-byte and diffs on mismatch.

The oracle pattern (`/bin/sh tests/run.sh --target /bin/sh`) is the
most important test we have. Any divergence between hs and the host
shell shows up immediately, which keeps hs honest about the POSIX
features it claims to support and prevents accidentally writing tests
to hs-specific behavior.

When adding a new test, run it under the oracle first (`/bin/sh
script.sh > script.out`) to capture the golden, then add the manifest
entry, then run it under each hs build to confirm parity. Tests that
require non-oracle behavior aren't supported by the framework.

## Where things live (quick reference)

- Memory allocators: [hs.c:110-260](../hs.c#L110-L260)
- Buffer helpers (`out_put`/`out_puts`/`lex_put`): [hs.c:200-660](../hs.c#L200-L660)
- Temp-file allocator (`make_cap_tmpfile_path`): [hs.c:706](../hs.c#L706)
- Variable store: [hs.c:760-1040](../hs.c#L760-L1040)
- Function table: [hs.c:1000-1100](../hs.c#L1000-L1100)
- Lexer: [hs.c:1300-2200](../hs.c#L1300-L2200)
- Parser error machinery: [hs.c:2415-2490](../hs.c#L2415-L2490)
- Recursive-descent parser: [hs.c:2491-3340](../hs.c#L2491-L3340)
- Pattern matching: [hs.c:3220-3400](../hs.c#L3220-L3400)
- Arithmetic evaluator: [hs.c:3405-3810](../hs.c#L3405-L3810)
- `expand_word`: [hs.c:4007-4930](../hs.c#L4007-L4930)
- Builtins: [hs.c:4931-6700](../hs.c#L4931-L6700)
- `exec_simple`: [hs.c:6765-7460](../hs.c#L6765-L7460)
- `exec_redir` (compound-group redirects): [hs.c:7495-7680](../hs.c#L7495-L7680)
- `exec_node`: [hs.c:7684-8050](../hs.c#L7684-L8050)
- `main`: [hs.c:8061](../hs.c#L8061)

These ranges drift as the file evolves; treat them as starting points,
not pinned addresses.
