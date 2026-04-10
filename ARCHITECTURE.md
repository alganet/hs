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

1. **Lexer**: [hs.c:1452 lex_tokenize](../hs.c#L1452) walks the input
   string and produces an array of tokens. The tokenizer is one-shot:
   it consumes the entire input up front before parsing starts.
2. **Parser**: recursive-descent functions in
   [hs.c:1889-2456](../hs.c#L1889-L2456) consume the token stream and
   build an AST in the node arrays (`nd_type`, `nd_a`, `nd_b`, `nd_c`,
   `nd_d`, `nd_str`, `nd_argv`, `nd_assigns`).
3. **Executor**: [hs.c:5378 exec_node](../hs.c#L5378) walks the AST
   and runs each node. Simple commands route through
   [hs.c:4819 exec_simple](../hs.c#L4819).

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

[hs.c:599 make_cap_tmpfile_path](../hs.c#L599) is the temp-file
allocator. It uses `O_CREAT | O_EXCL | HS_O_NOFOLLOW` with mode 0600
and retries on `EEXIST`, so a local attacker can't win the classic
symlink-attack against a predictable filename. `HS_O_NOFOLLOW` is
defined locally in [hs.c:36](../hs.c#L36) because M2libc's `fcntl.c`
doesn't expose it.

The downsides are real and intentional:

- Pipelines are slower than real OS pipes (the data round-trips through
  the filesystem).
- Background jobs (`&`) and job control are out of scope.
- Concurrent stages of a pipeline run sequentially, not in parallel.

If you find yourself wanting `dup2`, stop and check whether the
behavior can be emulated with the existing primitives. The whole
redirection layer is built around this restriction.

## Two allocators

hs uses two custom allocators on top of `calloc`/`free`:

- **Persistent pool** ([hs.c:85 perm_init](../hs.c#L85),
  `perm_alloc`, `perm_dup`): an 8 MB bump allocator that survives
  across commands. Function-definition AST nodes and persistent
  function bodies live here.
- **Temporary arena** ([hs.c:123 tmp_init](../hs.c#L123),
  `tmp_alloc`, `tmp_reset`): a 32 MB bump allocator that gets reset
  between commands so per-command scratch (expansion buffers, parsed
  tokens, etc.) doesn't leak across statements.

Neither allocator individually frees. The temporary arena reclaims
everything in one shot via `tmp_reset()`; the persistent pool grows
forever within a script run. Code that needs to survive arena resets
either uses `perm_alloc` or escapes via `calloc`/`free` directly.

`str_dup`, `tmp_dup`, `tmp_cat2`, and `tmp_cat3` are convenience
wrappers. The bounds-checked safe-string helpers (`str_cpy_safe`,
`str_cat_safe`, `buf_putc`, `out_put`, `out_puts`, `lex_put`) live in
[hs.c:155-265](../hs.c#L155-L265) and exist because hs has many
fixed-size buffers that historically called `strcpy`/`strcat` with no
length check. Wrapping every such call site in these helpers turns
silent overflows into loud `require()` errors.

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

(Definitions in [hs.c around line 70](../hs.c#L70).)

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
[hs.c:21-32](../hs.c#L21-L32) and
[hs.c:584](../hs.c#L584). Each one is checked at runtime via
`require()`; hitting any limit aborts with a specific error message.

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
| `MAX_ARGV`       | 1 024   | argv length per command                                                           | (clamps in `expand_argv`)                                                                                              |
| `MAX_DEPTH`      | 256     | Local-scope nesting; parser recursion; expansion recursion; `pat_match` recursion | `lsc_push: too deep`, `hs: nested commands too deep`, `hs: expansion nested too deep`, `hs: pattern too deeply nested` |
| `TMP_SIZE`       | 32 MiB  | Temporary arena size                                                              | (clamps; oldest scratch is reused)                                                                                     |
| `LEX_STACK_MAX`  | 64      | Alias-expansion recursion depth                                                   | (silently caps; recursive aliases stop)                                                                                |

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
  [hs.c:5378 exec_node](../hs.c#L5378) at the stored index. The AST
  itself lives in the persistent pool.
- **Eval-defined (string-form)**: set when a function is defined
  inside `eval`, `.`/`source` of a string, command substitution, or
  any other reentrant parse. The body stores the source text of the
  function body (the `{ ... }` slice extracted from the eval'd
  source. See [parse_simple in hs.c around line 2300](../hs.c#L2300)).
  Calling the function re-tokenizes and re-parses that text via
  `expand_and_exec`.

The string-form has a subtlety: when the eval'd function is called,
the parser re-runs the function definition from the body string. That
re-definition must be a no-op rather than a "rotate" (free old, install
new), because `lex_orig_src` aliases the slot's current contents.
[hs.c:911 fn_replace_body](../hs.c#L911) detects the
`fn_bodies[i] == body` self-aliased case and returns without touching
the slot. This is the single non-obvious invariant in the function
table; without it, calling an eval-defined function frees the source
the lexer is currently reading from.

A second subtlety: the body string stores **just the brace group**
(`{ ... }`), not the surrounding eval'd context. Storing the whole
context would re-execute everything around the function on every call,
which infinite-recurses if the surrounding context contains a call to
the same function. The slicing happens in
[parse_simple](../hs.c#L2300) using `tok_src_start[]` to find the body
range and `memcpy` to copy it out.

The tests in
[tests/control_flow/function_redefine_*.sh](../tests/control_flow/)
exercise both forms (basic, eval, source, nested, self-redefining,
loop, unset).

## Parser error diagnostics

Parser errors go through [hs.c:1800 parse_err](../hs.c#L1800), which
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
buffer-handling, recursion, and ownership code added to hs after the
initial commit.

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

- Memory allocators: [hs.c:85-150](../hs.c#L85-L150)
- Bounds-checked buffer helpers: [hs.c:155-265](../hs.c#L155-L265)
- Variable store: [hs.c:697-880](../hs.c#L697-L880)
- Function table: [hs.c:887-960](../hs.c#L887-L960)
- Lexer: [hs.c:1143-1700](../hs.c#L1143-L1700)
- Parser error machinery: [hs.c:1760-1855](../hs.c#L1760-L1855)
- Recursive-descent parser: [hs.c:1889-2456](../hs.c#L1889-L2456)
- Pattern matching: [hs.c:2488-2620](../hs.c#L2488-L2620)
- Arithmetic evaluator: [hs.c:2630-3050](../hs.c#L2630-L3050)
- `expand_word`: [hs.c:3146-3680](../hs.c#L3146-L3680)
- Builtins: [hs.c:3679-4800](../hs.c#L3679-L4800)
- `exec_simple`: [hs.c:4819-5375](../hs.c#L4819-L5375)
- `exec_node`: [hs.c:5378-5660](../hs.c#L5378-L5660)
- `main`: [hs.c:5664](../hs.c#L5664)

These ranges drift as the file evolves; treat them as starting points,
not pinned addresses.
