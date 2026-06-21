/*
 * SPDX-FileCopyrightText: 2026 Alexandre Gomes Gaigalas <alganet@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#ifndef __M2__
#include <sys/wait.h>
#endif
#include "bootstrappable.h"

/* hs uses only the libc/syscall primitives M2libc provides on every
   architecture -- no inline assembly, no raw syscall numbers, no struct
   byte-offset parsing -- so a single source builds for any M2-Planet target
   (x86, amd64, riscv64, aarch64, ...). The cost is no filesystem globbing and
   no symlink test (both need kernel calls M2libc doesn't wrap); file-type tests
   are derived portably from open()/read() instead. */

/* ============================================================
 * Constants
 * ============================================================ */
#define MAX_STR 8192
#define MAX_TOK 196608
#define MAX_TOK_NESTED 4096
#define MAX_ND 262144
#define MAX_VAR 8192
#define VAR_BUCKETS 64
#define MAX_FN 4096
#define MAX_AL 2048
#define MAX_LOCAL 4096
#define MAX_ARGV 1024
#define MAX_DEPTH 256
#define TMP_SIZE 33554432

/* Linux open() flags not always in M2libc's fcntl.c. Same values across the
   arches hs targets (x86/amd64/arm64/riscv). O_NOFOLLOW refuses symlink
   resolution on temp-file opens (defeats the /tmp symlink-attack); O_NONBLOCK
   keeps the file-type tests from blocking on a FIFO/device. */
#define HS_O_NOFOLLOW 0400000
#define HS_O_NONBLOCK 04000

/* Token types */
#define T_WORD   1
#define T_PIPE   2
#define T_SEMI   3
#define T_DSEMI  4
#define T_AMP    5
#define T_AND    6
#define T_OR     7
#define T_LT     8
#define T_GT     9
#define T_GTGT  10
#define T_NL    11
#define T_EOF   12
#define T_LPAREN 13
#define T_RPAREN 14
#define T_ASSIGN 15
#define T_BANG   16
#define T_DLT    17  /* << / <<- heredoc; val is patched to a redirect pseudo-arg */

/* Heredocs collected on one logical line (cat <<A <<B ...). */
#define MAX_HEREDOC 16
#define MAX_HEREDOC_DELIM 256
/* MAX_HEREDOC * MAX_HEREDOC_DELIM, precomputed: M2-Planet does not evaluate
   `*` in a constant expression (array dimension). */
#define MAX_HEREDOC_BUF 4096

/* Node types */
#define CMD_SIMPLE 1
#define CMD_PIPE   2
#define CMD_IF     3
#define CMD_WHILE  4
#define CMD_FOR    5
#define CMD_LIST   6
#define CMD_AND    7
#define CMD_OR     8
#define CMD_CASE   9
#define CMD_FUNC  10
#define CMD_SUBSH 11
#define CMD_BRACE 12
#define CMD_NOT   13
#define CMD_CASEI 14
#define CMD_REDIR 15  /* compound command with trailing redirects (done < f) */

/* Quoting sentinel bytes - using high values to avoid M2-Planet byte-handling edge cases */
#define Q_SQ_OPEN  '\x01'
#define Q_SQ_CLOSE '\x02'
#define Q_DQ_OPEN  '\x03'
#define Q_DQ_CLOSE '\x04'
#define Q_SPLIT    '\x05'
/* Marks the position of a "$@"/"$*" that expanded to zero positional
 * parameters. Unlike "", such a word must yield no field at all, so
 * expand_argv drops an empty field that carries this marker. strip_quotes
 * removes it everywhere else. (0x06 is the redirect pseudo-arg marker.) */
#define Q_ATNULL   '\x07'

/* Persistent bump allocator. Holds function body strings; never reset. */
char* perm_base;
int   perm_cur;
int   perm_size;

void perm_init(int size)
{
	perm_size = size;
	perm_base = calloc(perm_size, 1);
	require(perm_base != NULL, "perm_init: calloc failed\n");
	perm_cur = 0;
}

char* perm_alloc(int size)
{
	char* p;
	size = (size + 7) & (~7);
	if(perm_cur + size > perm_size)
	{
		/* Overflow: fall back to calloc. */
		return calloc(size, 1);
	}
	p = perm_base + perm_cur;
	perm_cur = perm_cur + size;
	return p;
}

char* perm_dup(char* s)
{
	int len;
	char* r;
	if(s == NULL) return NULL;
	len = strlen(s);
	r = perm_alloc(len + 1);
	strcpy(r, s);
	return r;
}

/* Temporary arena allocator, reset between commands. */
char* tmp_base;
int   tmp_cur;
int   tmp_size;

/* One scratch byte for the file-type tests' 1-byte read (path_is_dir/_reg),
   so they need not take the address of a local under M2-Planet. Allocated once
   in tmp_init. */
char* g_ftbuf;

void tmp_init()
{
	tmp_size = TMP_SIZE;
	tmp_base = calloc(tmp_size, 1);
	require(tmp_base != NULL, "tmp_init: calloc failed\n");
	tmp_cur = 0;
	g_ftbuf = calloc(1, 1);
	require(g_ftbuf != NULL, "tmp_init: calloc failed\n");
}

char* tmp_alloc(int size)
{
	char* p;
	int i;
	size = (size + 7) & (~7);
	if(tmp_cur + size > tmp_size)
	{
		return calloc(size, 1);
	}
	p = tmp_base + tmp_cur;
	tmp_cur = tmp_cur + size;
	/* M2-Planet's byte-level writes can be unreliable; zero explicitly. */
	memset(p, 0, size);
	return p;
}

void tmp_reset()
{
	tmp_cur = 0;
}

/* Bounded string primitives.
 *
 * These exist because hs has many fixed-size buffers (lex word buffer,
 * expand_word output, builtin_read line buffer, etc.) that historically
 * called strcpy/strcat with no length check. Wrapping every such call
 * site in these helpers turns silent overflows into loud truncation
 * errors via require(). Kept in hs.c (not bootstrappable.c) so the
 * M2-Planet build, which uses M2libc's upstream bootstrappable.c,
 * picks them up too. */

/* Grow *out so it can hold at least `need` bytes. The expand_word output
   buffer is a heap allocation that doubles on demand: a short word like
   "$V" can expand to an arbitrarily long value, so sizing the buffer from
   the input length (as the old fixed-size scheme did) overflowed on long
   variable values / command substitutions. */
void out_grow(char** out, int* outsize, int need)
{
	int ns;
	char* nb;
	if(need <= *outsize) return;
	ns = *outsize * 2;
	while(ns < need) ns = ns * 2;
	nb = calloc(ns, 1);
	require(nb != NULL, "hs: out of memory in expansion\n");
	memcpy(nb, *out, *outsize);
	free(*out);
	*out = nb;
	*outsize = ns;
}

/* Append a single byte to the expand_word output buffer, growing it as
   needed. Reserves one byte for the trailing NUL the caller writes. */
int out_put(char** out, int oi, int* outsize, int c)
{
	if(oi + 2 > *outsize) out_grow(out, outsize, oi + 2);
	(*out)[oi] = c;
	return oi + 1;
}

/* Append a NUL-terminated string, growing the buffer as needed. */
int out_puts(char** out, int oi, int* outsize, char* s)
{
	int n;
	n = strlen(s);
	if(oi + n + 1 > *outsize) out_grow(out, outsize, oi + n + 1);
	memcpy(*out + oi, s, n);
	return oi + n;
}

/* String helpers. */
char* str_dup(char* s)
{
	int len;
	char* r;
	if(s == NULL) return NULL;
	len = strlen(s);
	r = calloc(len + 1, 1);
	strcpy(r, s);
	return r;
}

char* tmp_dup(char* s)
{
	int len;
	char* r;
	if(s == NULL) return NULL;
	len = strlen(s);
	r = tmp_alloc(len + 1);
	strcpy(r, s);
	return r;
}

/* Return argv[n..argc-1] as a fresh NUL-terminated arena array. Use this to
   "shift" an argv instead of `argv + n` or `&argv[n]`: M2-Planet does not scale
   pointer arithmetic on a char** (both forms advance by bytes, not elements),
   so only element indexing argv[i] is safe. Copying through indexing sidesteps
   that. The other builds get the same correct result. */
char** argv_drop(char** argv, int argc, int n)
{
	char** out;
	int i;
	out = tmp_alloc((argc - n + 1) * sizeof(char*));
	i = n;
	while(i < argc)
	{
		out[i - n] = argv[i];
		i = i + 1;
	}
	out[argc - n] = NULL;
	return out;
}

char* tmp_cat2(char* a, char* b)
{
	int la;
	int lb;
	char* r;
	la = strlen(a);
	lb = strlen(b);
	r = tmp_alloc(la + lb + 1);
	strcpy(r, a);
	strcat(r, b);
	return r;
}

/* M2-Planet has no ternary operator. */
int bool_to_status(int cond)
{
	if(cond) return 0;
	return 1;
}

int bool_to_int(int cond)
{
	if(cond) return 1;
	return 0;
}

int invert_status(int status)
{
	if(status == 0) return 1;
	return 0;
}

char* tmp_cat3(char* a, char* b, char* c)
{
	return tmp_cat2(tmp_cat2(a, b), c);
}

/* Translate a waitpid() status into a shell exit status. A normally-exited
   child yields its exit code (high byte); a child killed by a signal yields
   128 + signal number (the low 7 bits), matching POSIX/dash/bash. Without the
   signal case a crashed child (e.g. a segfaulting configure probe) would wrongly
   report 0 and slip past `set -e` or a `( ) || handler`. */
int wait_status_to_exit(int status)
{
	int sig;
	sig = status & 0x7F;
	if(sig != 0) return 128 + sig;
	return (status >> 8) & 255;
}

/* External command execution. hs never redirects or captures an external
 * program's stdio: on the target kernels fd 0/1/2 are the console
 * unconditionally and there is no dup2, so an exec'd program's output cannot
 * be repointed (KERNEL.md K1). Redirection/capture is a builtin-only feature;
 * externals always run with the fds they inherit. So this is a plain
 * fork+exec -- the child keeps hs's fd table (no CLOEXEC, harmless for a
 * bootstrap shell, and closing fds in the child would corrupt the parent
 * across the kernels' shared-fd-table fork anyway). */
int exec_external(char* path, char** argv, char** envp)
{
	int child;
	int status;
	child = fork();
	if(child == 0)
	{
		execve(path, argv, envp);
		_exit(127);
	}
	waitpid(child, &status, 0);
	return wait_status_to_exit(status);
}

/* Shell-level stdio state. The three ints are the kernel fds our
 * stdio wrappers write through; redirection for builtins is pure
 * int manipulation on these. Paths track the file each fd was
 * opened from (NULL for inherited fds), and are used to emit
 * path-reopen specs and detect when co-capture is needed. */
int    hs_in_fd;
int    hs_out_fd;
int    hs_err_fd;
char*  hs_in_path;
char*  hs_out_path;
char*  hs_err_path;

/* Stdio wrappers: route stdout/stderr through hs_out_fd / hs_err_fd;
 * other FILE* (e.g. script files) falls through to libc. */
void sh_flush(FILE* f)
{
	/* stdout/stderr are unbuffered via write(), nothing to flush. */
	if(f == stdout) return;
	if(f == stderr) return;
	fflush(f);
}

void sh_puts(char* s, FILE* f)
{
	int fd;
	int n;
	if(f == stdout) fd = hs_out_fd;
	else if(f == stderr) fd = hs_err_fd;
	else
	{
		fputs(s, f);
		return;
	}
	n = strlen(s);
	if(n > 0) write(fd, s, n);
}

void sh_putc(int c, FILE* f)
{
	int fd;
	char ch;
	if(f == stdout) fd = hs_out_fd;
	else if(f == stderr) fd = hs_err_fd;
	else
	{
		fputc(c, f);
		return;
	}
	ch = c;
	write(fd, &ch, 1);
}

/* Stderr wrappers for shell error messages. Routing through
 * hs_err_fd lets `cmd 2>/dev/null` suppress shell errors too. */
void sh_err_puts(char* s)
{
	int n;
	n = strlen(s);
	if(n > 0) write(hs_err_fd, s, n);
}

void sh_err_putc(int c)
{
	char ch;
	ch = c;
	write(hs_err_fd, &ch, 1);
}

/* Read one byte from hs_in_fd. Used by `read` instead of fgetc(stdin). */
int hs_getc_in()
{
	char buf;
	int n;
	n = read(hs_in_fd, &buf, 1);
	if(n <= 0) return EOF;
	return buf & 0xff;
}

int str_to_int(char* s)
{
	int n;
	int neg;
	n = 0;
	neg = FALSE;
	if(s == NULL) return 0;
	if(s[0] == '-')
	{
		neg = TRUE;
		s = s + 1;
	}
	while(s[0] >= '0' && s[0] <= '9')
	{
		n = n * 10 + (s[0] - '0');
		s = s + 1;
	}
	if(neg) return 0 - n;
	return n;
}

char* int_to_str(int n)
{
	return int2str(n, 10, TRUE);
}

/* Global state. */

/* Variable store */
char** var_names;
char** var_vals;
int*   var_exported;
int*   var_hash_next;
int*   var_hash_buckets;
int    var_count;

/* Function store */
char** fn_names;
char** fn_bodies;
int    fn_count;

/* Alias store */
char** al_names;
char** al_vals;
int    al_count;

/* Local scope stack */
char** lsc_names;
char** lsc_vals;
int*   lsc_marks;
int    lsc_count;
int    lsc_mark_count;

/* Token arrays */
int*   tok_type;
char** tok_val;
int*   tok_src_start; /* source position where each token started */
int    tok_count;
int    tok_pos;
int    tok_capacity;  /* actual allocated size of tok arrays */

/* AST node arrays */
int*   nd_type;
int*   nd_a;
int*   nd_b;
int*   nd_c;
int*   nd_d;
char** nd_str;
char*** nd_argv;    /* CMD_SIMPLE: expanded argv pointer */
char*** nd_assigns; /* CMD_SIMPLE: expanded assigns pointer */
int    nd_count;

/* Positional parameters */
char** pp_argv;
int    pp_argc;

/* Shell state */
int    last_status;
int    flag_errexit;
int    suppress_errexit;
/* TRUE when the just-returned exec result is an `&&`/`||` short-circuit (a
   non-final operand failed): set -e must NOT act on such a status, even though
   it is nonzero. Reset at every exec_node entry; set by CMD_AND on short-
   circuit, so it travels with the result through enclosing lists/compounds. */
int    g_andor_shortcircuit;
int    flag_nounset;
/* Whether the current expansion is subject to field splitting. TRUE in
 * list contexts (command words, `for` lists); FALSE in single-word
 * contexts (assignment RHS, redirect targets, case subject), where an
 * unquoted $var / $(cmd) must keep its whitespace verbatim instead of
 * being split. ifs_copy consults this (like the in_dq flag). */
int    g_field_split;
int    fn_return_flag;
int    loop_break_level;
int    loop_continue_flag;
int    in_function;
/* In-process subshell execution. hs runs `( )`, `$( )` and pipe stages without
 * fork-without-exec (the builder-hex0-arch kernels do not restore a parent that
 * forked and then exited without execve -- see KERNEL.md K1), so a subshell body
 * runs in the same process under run_isolated(). subshell_depth > 0 means we are
 * inside such a body; an `exit` then unwinds via g_subshell_exit (like
 * fn_return_flag) instead of terminating the whole shell. */
int    subshell_depth;
int    g_subshell_exit;
int    g_subshell_exit_status;
char*  cached_ifs;
char** global_envp;
/* Action registered for the EXIT pseudo-signal via `trap`, or NULL.
 * heap-owned; fired once when the shell terminates. Other signals are
 * accepted by `trap` but not delivered (hs has no signal handling). */
char*  trap_exit_action;

/* Pipe left-side output: CMD_PIPE writes the left side to a tmp file
 * and stores the path here; exec_simple picks it up on the right side
 * as an implicit `< file` redirect. */
char*  pending_pipe_stdin_file;
int    tmp_counter;

/* Last value handed to `umask`. The minimal kernels have no umask syscall
 * (it is not one of the ~18 they dispatch; an unknown syscall returns -1 on
 * aarch64/riscv64 and 0 on x86), so calling umask(0) to *read* the mask would
 * print arch-dependent garbage. We track the value ourselves instead: the set
 * form records it here (and still calls umask() for the POSIX host, where it
 * works), the print form reports it. Default 022 matches a typical shell. See
 * KERNEL.md. */
int    g_umask;

/* Lexer state for reentrant parsing */
char*  lex_src;
int    lex_pos;
int    lex_len;
int    tok_start_pos; /* lex_pos before current token */
char*  lex_orig_src;  /* original source text (before alias expansion) */
int    lex_line_offset; /* 1-based line where lex_orig_src starts in the
                            outer file. exec_source chops the script into
                            per-command chunks; this offset turns
                            chunk-local line numbers back into script-
                            global ones for parse error diagnostics. */
int    cur_lineno;      /* $LINENO: line of the chunk currently executing. */
int    g_cmdsub_count;  /* incremented per `$(...)`; lets an assignment-only
                           command tell `x=lit` ($?=0) from `x=$(cmd)` ($?=cmd). */
char*  lex_orig_path; /* path of the file containing lex_orig_src, or
                          NULL for eval/expand_and_exec strings. */
int    parsing_persistent; /* TRUE during exec_source - nodes must be heap-backed */

/* Lex source stack for alias expansion (avoids copying remaining input) */
#define LEX_STACK_MAX 64
char** lex_stack_src;
int*   lex_stack_pos;
int*   lex_stack_len;
int*   lex_stack_cmd; /* saved cmd_pos for each stack level */
int    lex_stack_depth;

/* Pending heredocs on the current logical line. The lexer records each
 * `<<`/`<<-` operator as it is seen; when the line's terminating newline
 * arrives, the bodies are read off the following lines and the recorded
 * T_DLT token's value is patched into a redirect pseudo-arg.
 *
 * These (and the fce_* scratch below) are heap pointers allocated in main,
 * not static arrays: hs uses no statically-sized global arrays because
 * M2-Planet does not lay them out correctly. */
int    hd_pending;
int*   hd_tok;          /* token index of the T_DLT to patch [MAX_HEREDOC] */
int*   hd_strip;        /* <<- : strip leading tabs [MAX_HEREDOC] */
int*   hd_expand;       /* unquoted delimiter : expand the body [MAX_HEREDOC] */
char** hd_delim;        /* terminator word, quotes stripped [MAX_HEREDOC] */
char*  hd_delim_buf;    /* backing store for hd_delim [MAX_HEREDOC_BUF] */

/* find_cmd_end's own heredoc scratch (separate so it never overlaps the
 * lexer's, since find_cmd_end runs to completion before lex_tokenize). */
char** fce_hd_dp;       /* [MAX_HEREDOC] */
int*   fce_hd_s;        /* [MAX_HEREDOC] */
char*  fce_hd_d;        /* [MAX_HEREDOC_BUF] */

/* Forward declarations */
int exec_node(int idx);
int exec_source(char* path);
int exec_buffer(char* buf, int len, char* path);
void parse_init(char* input);
int parse_list();
int expand_and_exec(char* code);
int run_isolated(int node_idx, char* expr, int out_fd, char* out_path);

/* Fork-based output capture for $(cmd) and CMD_PIPE left-side.
 * Fork a child, redirect its fd 1 to a fresh /tmp/hs-cap-<n> via
 * close(1)+open(), run the expression or AST node, exit. Caller
 * owns the returned path and must unlink+free it.
 *
 * The file is created here with O_CREAT|O_EXCL|O_NOFOLLOW and mode
 * 0600 so a local attacker on a shared /tmp cannot win the classic
 * symlink race against a predictable filename: O_EXCL means a
 * pre-existing entry (regular file or symlink) makes us bump the
 * counter and retry; O_NOFOLLOW means even if the attacker races
 * us between create and the child's reopen, the open fails safely.
 * The fd is closed immediately, we just want the path to point at
 * a freshly-created, owner-only regular file before fork(). */
char* make_cap_tmpfile_path()
{
	char* num;
	int nlen;
	char* tmpfile;
	int fd;
	int tries;

	tries = 0;
	while(TRUE)
	{
		tmp_counter = tmp_counter + 1;
		num = int_to_str(tmp_counter);
		nlen = strlen("/tmp/hs-cap-") + strlen(num) + 1;
		tmpfile = calloc(nlen, 1);
		strcpy(tmpfile, "/tmp/hs-cap-");
		strcat(tmpfile, num);

		fd = open(tmpfile, O_WRONLY | O_CREAT | O_EXCL | HS_O_NOFOLLOW, 384);
		if(fd >= 0)
		{
			close(fd);
			return tmpfile;
		}
		free(tmpfile);
		tries = tries + 1;
		require(tries < 1024, "hs: cannot create temp file\n");
	}
}

/* Open a path for append (`>>`). Some kernels ignore O_APPEND and, worse, make
   O_CREAT allocate a fresh zero-length entry even when the path exists (KERNEL.md
   K4), so `O_WRONLY|O_CREAT|O_APPEND` would overwrite from offset 0. Emulate
   append portably: reopen the existing file WITHOUT O_CREAT and seek to its end;
   only create when it is genuinely absent. On a POSIX kernel this is equivalent
   to O_APPEND for a single writer. */
int open_append(char* p)
{
	int fd;
	fd = open(p, O_WRONLY, 0);
	if(fd < 0) fd = open(p, O_WRONLY | O_CREAT, 420);
	if(fd >= 0) lseek(fd, 0, SEEK_END);
	return fd;
}

/* Capture a command substitution's stdout into a fresh temp file and return its
   path (the caller reads it back, then unlinks+frees it). The body runs through
   run_isolated() in this same process -- no fork -- with its stdout redirected
   to the temp file via hs_out_fd. This is what lets `$(...)` work on the
   builder-hex0-arch kernels, which cannot fork-without-exec (KERNEL.md K1); it
   also captures builtins/compounds regardless of which fd open() returns, since
   hs's stdio wrapper writes through hs_out_fd rather than literal fd 1. (A body
   that exec's an external still cannot be captured there -- the external writes
   to fd 1, the console -- but the bootstrap has no externals besides hs.)

   make_cap_tmpfile_path() already created the file with O_CREAT|O_EXCL, so we
   reopen WITHOUT O_CREAT (avoids the kernel's "O_CREAT always makes a new entry"
   behaviour, KERNEL.md K4). For `x=$(cmd)` last_status becomes $? (POSIX: an
   assignment-only command exits with the last substitution's status). */
char* capture_expr_to_tmpfile(char* expr)
{
	char* tmpfile;
	int fd;

	tmpfile = make_cap_tmpfile_path();
	g_cmdsub_count = g_cmdsub_count + 1;
	fd = open(tmpfile, O_WRONLY | O_TRUNC | HS_O_NOFOLLOW, 384);
	if(fd < 0) { last_status = 1; return tmpfile; }
	run_isolated(-1, expr, fd, tmpfile);
	close(fd);
	return tmpfile;
}

char* capture_node_to_tmpfile(int node_idx)
{
	char* tmpfile;
	int fd;

	tmpfile = make_cap_tmpfile_path();
	fd = open(tmpfile, O_WRONLY | O_TRUNC | HS_O_NOFOLLOW, 384);
	if(fd < 0) return tmpfile;
	run_isolated(node_idx, NULL, fd, tmpfile);
	close(fd);
	return tmpfile;
}

/* ============================================================
 * Variable store
 * ============================================================ */
int var_hash(char* name)
{
	int h;
	h = 5381;
	while(name[0] != 0)
	{
		/* djb2; mask each step to keep h within positive int and
		   avoid signed overflow on the (h << 5) + h multiply. */
		h = (((h << 5) + h) + name[0]) & 0x00FFFFFF;
		name = name + 1;
	}
	return h & 63;
}

void var_init()
{
	int i;
	var_names = calloc(MAX_VAR, sizeof(char*));
	var_vals  = calloc(MAX_VAR, sizeof(char*));
	var_exported = calloc(MAX_VAR, sizeof(int));
	var_hash_next = calloc(MAX_VAR, sizeof(int));
	var_hash_buckets = calloc(VAR_BUCKETS, sizeof(int));
	i = 0;
	while(i < VAR_BUCKETS)
	{
		var_hash_buckets[i] = -1;
		i = i + 1;
	}
	var_count = 0;
	last_status = 0;
	cached_ifs = " \t\n";
}

int var_find(char* name)
{
	int h;
	int i;
	h = var_hash(name);
	i = var_hash_buckets[h];
	while(i >= 0)
	{
		if(match(var_names[i], name)) return i;
		i = var_hash_next[i];
	}
	return -1;
}

char* var_get_internal(char* name, int check_nounset)
{
	int i;
	int n;

	/* Positional params $0-$9 */
	if(name[0] >= '0' && name[0] <= '9' && name[1] == 0)
	{
		n = name[0] - '0';
		if(n < pp_argc) return pp_argv[n];
		return "";
	}
	if(match(name, "?")) return int_to_str(last_status);
	if(match(name, "#"))
	{
		if(pp_argc > 0) return int_to_str(pp_argc - 1);
		return "0";
	}
	if(match(name, "$")) return "0";
	if(match(name, "-")) return "";
	if(match(name, "!")) return "0";
	/* $LINENO: line of the command currently executing. Tracked per chunk
	   (find_cmd_end splits per command, so consecutive single-command lines
	   get consecutive numbers -- enough for autoconf's LINENO probe, which
	   otherwise sed-rewrites the whole script). */
	if(match(name, "LINENO")) return int_to_str(cur_lineno);

	i = var_find(name);
	if(i < 0)
	{
		if(check_nounset && flag_nounset)
		{
			sh_err_puts("hs: ");
			sh_err_puts(name);
			sh_err_puts(": unbound variable\n");
			if(flag_errexit)
			{
				if(subshell_depth > 0)
				{
					g_subshell_exit = TRUE;
					g_subshell_exit_status = 1;
					return "";
				}
				exit(1);
			}
		}
		return "";
	}
	return var_vals[i];
}

char* var_get(char* name)
{
	return var_get_internal(name, TRUE);
}

char* var_get_safe(char* name)
{
	return var_get_internal(name, FALSE);
}

/* TRUE if the parameter `name` is set (exists), handling positional params
 * ($1.. via pp_argc) and special params, not just the variable table. Used
 * by the ${VAR-w}/${VAR+w}/${VAR=w}/${VAR?w} forms, whose "is set" test is
 * `var_find >= 0` -- which misses positionals (breaking `${1+"$@"}`). */
int param_is_set(char* name)
{
	if(name[0] == 0) return FALSE;
	if(name[0] >= '0' && name[0] <= '9')
	{
		int allnum;
		int i;
		allnum = TRUE;
		i = 0;
		while(name[i] != 0)
		{
			if(name[i] < '0' || name[i] > '9') { allnum = FALSE; break; }
			i = i + 1;
		}
		if(allnum) return str_to_int(name) < pp_argc;
	}
	/* Special single-char params ($?, $#, $@, $*, $!, $$, $-, $0) are set. */
	if(name[1] == 0 &&
	   (name[0] == '?' || name[0] == '#' || name[0] == '@' || name[0] == '*' ||
	    name[0] == '!' || name[0] == '$' || name[0] == '-'))
		return TRUE;
	return var_find(name) >= 0;
}

int in_subshell; /* >0: var_set goes through lsc_declare for isolation. */
void lsc_declare(char* name, char* val);

void var_set(char* name, char* val)
{
	int i;
	int h;

	/* Subshell: record via lsc_declare so lsc_pop restores on exit.
	 * Clear in_subshell while inside lsc_declare to avoid recursion. */
	if(in_subshell > 0)
	{
		int save_in_subshell;
		save_in_subshell = in_subshell;
		in_subshell = 0;
		lsc_declare(name, val);
		in_subshell = save_in_subshell;
		return;
	}

	i = var_find(name);
	if(i >= 0)
	{
		var_vals[i] = str_dup(val);
		if(match(name, "IFS")) cached_ifs = var_vals[i];
		return;
	}
	/* New variable */
	require(var_count < MAX_VAR, "var_set: too many variables\n");
	h = var_hash(name);
	var_hash_next[var_count] = var_hash_buckets[h];
	var_hash_buckets[h] = var_count;
	var_names[var_count] = str_dup(name);
	var_vals[var_count] = str_dup(val);
	var_exported[var_count] = 0;
	var_count = var_count + 1;
	if(match(name, "IFS")) cached_ifs = var_vals[var_count - 1];
}

void var_export(char* name)
{
	int i;
	i = var_find(name);
	if(i >= 0)
	{
		var_exported[i] = 1;
		return;
	}
	var_set(name, "");
	i = var_find(name);
	if(i >= 0) var_exported[i] = 1;
}

void var_unset(char* name)
{
	int i;
	i = var_find(name);
	if(i >= 0)
	{
		var_vals[i] = "";
		var_names[i] = ""; /* hides it from var_find. */
	}
}

/* Build envp array from exported variables. */
char** var_build_envp()
{
	char** envp;
	int i;
	int ei;
	char* buf;

	envp = calloc(var_count + 1, sizeof(char*));
	ei = 0;
	i = 0;
	while(i < var_count)
	{
		if(var_exported[i] && var_names[i][0] != 0)
		{
			{
				int nlen;
				int vlen;
				nlen = strlen(var_names[i]);
				vlen = strlen(var_vals[i]);
				buf = calloc(nlen + vlen + 2, 1);
				memcpy(buf, var_names[i], nlen);
				buf[nlen] = '=';
				memcpy(buf + nlen + 1, var_vals[i], vlen);
			}
			envp[ei] = buf;
			ei = ei + 1;
		}
		i = i + 1;
	}
	envp[ei] = NULL;
	return envp;
}

/* Function store. */
void fn_init()
{
	fn_names = calloc(MAX_FN, sizeof(char*));
	fn_bodies = calloc(MAX_FN, sizeof(char*));
	fn_count = 0;
}

int fn_find(char* name)
{
	int i;
	i = 0;
	while(i < fn_count)
	{
		if(match(fn_names[i], name)) return i;
		i = i + 1;
	}
	return -1;
}

/* Replace fn_bodies[i] with a fresh str_dup of `body`, freeing the
   previously stored body. Centralizes the free so each slot always
   owns its memory.

   Self-aliased case: when an eval-defined function is called, hs
   re-parses its stored body via expand_and_exec; that re-parse
   re-runs the function definition and so calls fn_set with
   body == fn_bodies[i] (lex_orig_src aliases the slot). Freeing
   would leave the active lexer with a dangling pointer. The new
   body would also be byte-identical to the old one anyway, so the
   correct behavior is to no-op. */
void fn_replace_body(int i, char* body)
{
	char* fresh;
	if(fn_bodies[i] == body) return;
	fresh = str_dup(body);
	if(fn_bodies[i] != NULL) free(fn_bodies[i]);
	fn_bodies[i] = fresh;
}

void fn_set(char* name, char* body)
{
	int i;
	i = fn_find(name);
	if(i >= 0)
	{
		fn_replace_body(i, body);
		return;
	}
	require(fn_count < MAX_FN, "fn_set: too many functions\n");
	fn_names[fn_count] = str_dup(name);
	fn_bodies[fn_count] = str_dup(body);
	fn_count = fn_count + 1;
}

void fn_unset(char* name)
{
	int i;
	i = fn_find(name);
	if(i >= 0)
	{
		/* Free the slot's storage and replace with fresh empty
		   strings so future fn_set/fn_unset on the same slot can
		   free unconditionally without risking a double-free. */
		if(fn_names[i] != NULL) free(fn_names[i]);
		if(fn_bodies[i] != NULL) free(fn_bodies[i]);
		fn_names[i] = str_dup("");
		fn_bodies[i] = str_dup("");
	}
}

/* Alias store. */
void al_init()
{
	al_names = calloc(MAX_AL, sizeof(char*));
	al_vals  = calloc(MAX_AL, sizeof(char*));
	al_count = 0;
}

int al_find(char* name)
{
	int i;
	i = 0;
	while(i < al_count)
	{
		if(match(al_names[i], name)) return i;
		i = i + 1;
	}
	return -1;
}

void al_set(char* name, char* val)
{
	int i;
	i = al_find(name);
	if(i >= 0)
	{
		al_vals[i] = str_dup(val);
		return;
	}
	require(al_count < MAX_AL, "al_set: too many aliases\n");
	al_names[al_count] = str_dup(name);
	al_vals[al_count] = str_dup(val);
	al_count = al_count + 1;
}

void al_unset(char* name)
{
	int i;
	i = al_find(name);
	if(i >= 0)
	{
		al_names[i] = "";
		al_vals[i] = "";
	}
}

/* Local scope stack for function-scoped vars. */
void lsc_init()
{
	lsc_names = calloc(MAX_LOCAL, sizeof(char*));
	lsc_vals  = calloc(MAX_LOCAL, sizeof(char*));
	lsc_marks = calloc(MAX_DEPTH, sizeof(int));
	lsc_count = 0;
	lsc_mark_count = 0;
}

void lsc_push()
{
	require(lsc_mark_count < MAX_DEPTH, "lsc_push: too deep\n");
	lsc_marks[lsc_mark_count] = lsc_count;
	lsc_mark_count = lsc_mark_count + 1;
}

void lsc_declare(char* name, char* val)
{
	int i;
	require(lsc_count < MAX_LOCAL, "lsc_declare: overflow\n");
	i = var_find(name);
	lsc_names[lsc_count] = str_dup(name);
	if(i >= 0)
	{
		lsc_vals[lsc_count] = str_dup(var_vals[i]);
	}
	else
	{
		lsc_vals[lsc_count] = NULL; /* was unset, to be restored as unset. */
	}
	lsc_count = lsc_count + 1;
	var_set(name, val);
}

void lsc_pop()
{
	int mark;
	int saved_subshell;
	require(lsc_mark_count > 0, "lsc_pop: underflow\n");
	lsc_mark_count = lsc_mark_count - 1;
	mark = lsc_marks[lsc_mark_count];

	/* Clear in_subshell so the var_set calls below restore directly
	 * instead of appending new entries to the scope being unwound. */
	saved_subshell = in_subshell;
	in_subshell = 0;

	while(lsc_count > mark)
	{
		lsc_count = lsc_count - 1;
		if(lsc_vals[lsc_count] == NULL)
		{
			var_unset(lsc_names[lsc_count]);
		}
		else
		{
			var_set(lsc_names[lsc_count], lsc_vals[lsc_count]);
		}
	}

	in_subshell = saved_subshell;
}

/* Bounds-checked write into the lex word buffer. Bails via require()
   on overflow rather than silently corrupting memory. */
int lex_put(char* buf, int bi, int c)
{
	require(bi + 1 < MAX_STR, "hs: token too long\n");
	buf[bi] = c;
	return bi + 1;
}

/* Rewrite a backtick command substitution into "$( cmd)" appended to buf via
   lex_put, with lex_pos at the opening backtick; consumes through the closing
   one and returns the new buf index. The leading space stops a `(...)` body
   from looking like $(( arithmetic. Inside backticks, \` \\ \$ are escapes.
   Shared by the top-level and inside-double-quote scanners so the two can't
   drift. (heredoc_annotate does the same rewrite on a different buffer; keep
   the escape set -- ` \ $ -- in sync with it.) */
int lex_emit_backtick(char* buf, int bi)
{
	bi = lex_put(buf, bi, '$');
	bi = lex_put(buf, bi, '(');
	bi = lex_put(buf, bi, ' ');
	lex_pos = lex_pos + 1;
	while(lex_pos < lex_len && lex_src[lex_pos] != '`')
	{
		if(lex_src[lex_pos] == '\\' && lex_pos + 1 < lex_len &&
		   (lex_src[lex_pos + 1] == '`' || lex_src[lex_pos + 1] == '\\' || lex_src[lex_pos + 1] == '$'))
		{
			bi = lex_put(buf, bi, lex_src[lex_pos + 1]);
			lex_pos = lex_pos + 2;
		}
		else
		{
			bi = lex_put(buf, bi, lex_src[lex_pos]);
			lex_pos = lex_pos + 1;
		}
	}
	if(lex_pos < lex_len) lex_pos = lex_pos + 1; /* closing ` */
	bi = lex_put(buf, bi, ')');
	return bi;
}

/* Lexer. */
void tok_init()
{
	tok_type = calloc(MAX_TOK, sizeof(int));
	tok_val  = calloc(MAX_TOK, sizeof(char*));
	tok_src_start = calloc(MAX_TOK, sizeof(int));
	tok_count = 0;
	tok_pos = 0;
	tok_capacity = MAX_TOK;
}

int is_op_char(int c)
{
	if(c == '|') return TRUE;
	if(c == '&') return TRUE;
	if(c == ';') return TRUE;
	if(c == '<') return TRUE;
	if(c == '>') return TRUE;
	if(c == '(') return TRUE;
	if(c == ')') return TRUE;
	return FALSE;
}

int is_blank(int c)
{
	if(c == ' ') return TRUE;
	if(c == '\t') return TRUE;
	return FALSE;
}

int is_word_break(int c)
{
	if(c == 0) return TRUE;
	if(c == '\n') return TRUE;
	if(is_blank(c)) return TRUE;
	if(is_op_char(c)) return TRUE;
	return FALSE;
}

int is_keyword(char* w)
{
	if(match(w, "if")) return TRUE;
	if(match(w, "then")) return TRUE;
	if(match(w, "elif")) return TRUE;
	if(match(w, "else")) return TRUE;
	if(match(w, "fi")) return TRUE;
	if(match(w, "while")) return TRUE;
	if(match(w, "until")) return TRUE;
	if(match(w, "for")) return TRUE;
	if(match(w, "do")) return TRUE;
	if(match(w, "done")) return TRUE;
	if(match(w, "case")) return TRUE;
	if(match(w, "in")) return TRUE;
	if(match(w, "esac")) return TRUE;
	if(match(w, "!")) return TRUE;
	if(match(w, "{")) return TRUE;
	if(match(w, "}")) return TRUE;
	return FALSE;
}

/* Lex a word at lex_pos. Quoting is preserved via sentinel bytes. */
char* lex_word()
{
	char* buf;
	int bi;
	int c;
	int depth;

	if(parsing_persistent)
	{
		/* Persistent pool: survives arena reset. */
		if(perm_cur + MAX_STR > perm_size)
		{
			buf = calloc(MAX_STR, 1);
		}
		else
		{
			buf = perm_base + perm_cur;
		}
	}
	else
	{
		/* Arena: reclaimed per-command. */
		buf = tmp_base + tmp_cur;
	}
	bi = 0;

	while(lex_pos < lex_len)
	{
		c = lex_src[lex_pos];

		if(is_word_break(c)) break;

		if(c == '\'')
		{
			bi = lex_put(buf, bi, Q_SQ_OPEN);
			lex_pos = lex_pos + 1;
			while(lex_pos < lex_len && lex_src[lex_pos] != '\'')
			{
				bi = lex_put(buf, bi, lex_src[lex_pos]);
				lex_pos = lex_pos + 1;
			}
			if(lex_pos < lex_len) lex_pos = lex_pos + 1; /* closing ' */
			bi = lex_put(buf, bi, Q_SQ_CLOSE);
		}
		else if(c == '"')
		{
			bi = lex_put(buf, bi, Q_DQ_OPEN);
			lex_pos = lex_pos + 1;
			while(lex_pos < lex_len && lex_src[lex_pos] != '"')
			{
				if(lex_src[lex_pos] == '\\' && lex_pos + 1 < lex_len)
				{
					c = lex_src[lex_pos + 1];
					if(c == '"' || c == '\\' || c == '$' || c == '`' || c == '\n')
					{
						if(c != '\n')
						{
							/* Sentinel-wrap so expand_word treats it as literal. */
							bi = lex_put(buf, bi, Q_SQ_OPEN);
							bi = lex_put(buf, bi, c);
							bi = lex_put(buf, bi, Q_SQ_CLOSE);
						}
						lex_pos = lex_pos + 2;
					}
					else
					{
						bi = lex_put(buf, bi, '\\');
						lex_pos = lex_pos + 1;
					}
				}
				else if(lex_src[lex_pos] == '$')
				{
					/* Pass $ expressions through unparsed. */
					bi = lex_put(buf, bi, '$');
					lex_pos = lex_pos + 1;
					if(lex_pos < lex_len && lex_src[lex_pos] == '(')
					{
						/* $( ... ) or $(( ... )) */
						depth = 1;
						bi = lex_put(buf, bi, '(');
						lex_pos = lex_pos + 1;
						{
							int qsq;
							int qdq;
							qsq = FALSE;
							qdq = FALSE;
							while(lex_pos < lex_len && depth > 0)
							{
								c = lex_src[lex_pos];
								bi = lex_put(buf, bi, c);
								lex_pos = lex_pos + 1;
								/* Quotes shield parens (embedded sed scripts). */
								if(qsq) { if(c == '\'') qsq = FALSE; }
								else if(qdq) { if(c == '"') qdq = FALSE; }
								else if(c == '\'') qsq = TRUE;
								else if(c == '"') qdq = TRUE;
								else if(c == '(') depth = depth + 1;
								else if(c == ')') depth = depth - 1;
							}
						}
					}
					else if(lex_pos < lex_len && lex_src[lex_pos] == '{')
					{
						/* ${...} */
						depth = 1;
						bi = lex_put(buf, bi, '{');
						lex_pos = lex_pos + 1;
						while(lex_pos < lex_len && depth > 0)
						{
							c = lex_src[lex_pos];
							bi = lex_put(buf, bi, c);
							lex_pos = lex_pos + 1;
							if(c == '\\' && lex_pos < lex_len)
							{
								/* Escape: copy next char without affecting depth. */
								bi = lex_put(buf, bi, lex_src[lex_pos]);
								lex_pos = lex_pos + 1;
							}
							else if(c == '\'')
							{
								/* SQ inside ${}: convert to sentinel.
								   Overwrite the just-written ' with the
								   open sentinel; bi was already advanced. */
								buf[bi - 1] = Q_SQ_OPEN;
								while(lex_pos < lex_len && lex_src[lex_pos] != '\'')
								{
									bi = lex_put(buf, bi, lex_src[lex_pos]);
									lex_pos = lex_pos + 1;
								}
								bi = lex_put(buf, bi, Q_SQ_CLOSE);
								if(lex_pos < lex_len) lex_pos = lex_pos + 1;
							}
							else if(c == '"')
							{
								/* DQ inside ${}: convert to sentinel. */
								buf[bi - 1] = Q_DQ_OPEN;
								while(lex_pos < lex_len && lex_src[lex_pos] != '"')
								{
									if(lex_src[lex_pos] == '\\' && lex_pos + 1 < lex_len)
									{
										bi = lex_put(buf, bi, lex_src[lex_pos]); lex_pos = lex_pos + 1;
										bi = lex_put(buf, bi, lex_src[lex_pos]); lex_pos = lex_pos + 1;
									}
									else
									{
										bi = lex_put(buf, bi, lex_src[lex_pos]);
										lex_pos = lex_pos + 1;
									}
								}
								bi = lex_put(buf, bi, Q_DQ_CLOSE);
								if(lex_pos < lex_len) lex_pos = lex_pos + 1;
							}
							else if(c == '{') depth = depth + 1;
							else if(c == '}') depth = depth - 1;
						}
					}
					/* $VAR: copy the name. */
					else
					{
						while(lex_pos < lex_len)
						{
							c = lex_src[lex_pos];
							if((c >= 'a' && c <= 'z') ||
							   (c >= 'A' && c <= 'Z') ||
							   (c >= '0' && c <= '9') ||
							   c == '_')
							{
								bi = lex_put(buf, bi, c);
								lex_pos = lex_pos + 1;
							}
							else if(c == '?' || c == '#' || c == '@' || c == '*' || c == '!' || c == '-')
							{
								/* Special single-char params. */
								bi = lex_put(buf, bi, c);
								lex_pos = lex_pos + 1;
								break;
							}
							else
							{
								break;
							}
						}
					}
				}
				else if(lex_src[lex_pos] == '`')
				{
					/* Backtick inside "...": rewrite to $( cmd). */
					bi = lex_emit_backtick(buf, bi);
				}
				else
				{
					bi = lex_put(buf, bi, lex_src[lex_pos]);
					lex_pos = lex_pos + 1;
				}
			}
			if(lex_pos < lex_len) lex_pos = lex_pos + 1; /* closing " */
			bi = lex_put(buf, bi, Q_DQ_CLOSE);
		}
		else if(c == '\\')
		{
			lex_pos = lex_pos + 1;
			if(lex_pos < lex_len)
			{
				if(lex_src[lex_pos] == '\n')
				{
					/* Line continuation. */
					lex_pos = lex_pos + 1;
				}
				else
				{
					bi = lex_put(buf, bi, Q_SQ_OPEN);
					bi = lex_put(buf, bi, lex_src[lex_pos]);
					bi = lex_put(buf, bi, Q_SQ_CLOSE);
					lex_pos = lex_pos + 1;
				}
			}
		}
		else if(c == '$')
		{
			bi = lex_put(buf, bi, '$');
			lex_pos = lex_pos + 1;
			if(lex_pos < lex_len && lex_src[lex_pos] == '(')
			{
				depth = 1;
				bi = lex_put(buf, bi, '(');
				lex_pos = lex_pos + 1;
				{
					int qsq;
					int qdq;
					qsq = FALSE;
					qdq = FALSE;
					while(lex_pos < lex_len && depth > 0)
					{
						c = lex_src[lex_pos];
						bi = lex_put(buf, bi, c);
						lex_pos = lex_pos + 1;
						/* Quotes shield parens (embedded sed scripts). */
						if(qsq) { if(c == '\'') qsq = FALSE; }
						else if(qdq) { if(c == '"') qdq = FALSE; }
						else if(c == '\'') qsq = TRUE;
						else if(c == '"') qdq = TRUE;
						else if(c == '(') depth = depth + 1;
						else if(c == ')') depth = depth - 1;
					}
				}
			}
			else if(lex_pos < lex_len && lex_src[lex_pos] == '{')
			{
				depth = 1;
				bi = lex_put(buf, bi, '{');
				lex_pos = lex_pos + 1;
				while(lex_pos < lex_len && depth > 0)
				{
					c = lex_src[lex_pos];
					bi = lex_put(buf, bi, c);
					lex_pos = lex_pos + 1;
					if(c == '\\' && lex_pos < lex_len)
					{
						bi = lex_put(buf, bi, lex_src[lex_pos]);
						lex_pos = lex_pos + 1;
					}
					else if(c == '\'')
					{
						/* SQ inside ${}: convert to sentinel so quoted
						   defaults/alternates expand correctly (e.g. the
						   "$@" in ${1+"$@"}). */
						buf[bi - 1] = Q_SQ_OPEN;
						while(lex_pos < lex_len && lex_src[lex_pos] != '\'')
						{
							bi = lex_put(buf, bi, lex_src[lex_pos]);
							lex_pos = lex_pos + 1;
						}
						bi = lex_put(buf, bi, Q_SQ_CLOSE);
						if(lex_pos < lex_len) lex_pos = lex_pos + 1;
					}
					else if(c == '"')
					{
						/* DQ inside ${}: convert to sentinel. */
						buf[bi - 1] = Q_DQ_OPEN;
						while(lex_pos < lex_len && lex_src[lex_pos] != '"')
						{
							if(lex_src[lex_pos] == '\\' && lex_pos + 1 < lex_len)
							{
								bi = lex_put(buf, bi, lex_src[lex_pos]); lex_pos = lex_pos + 1;
								bi = lex_put(buf, bi, lex_src[lex_pos]); lex_pos = lex_pos + 1;
							}
							else
							{
								bi = lex_put(buf, bi, lex_src[lex_pos]);
								lex_pos = lex_pos + 1;
							}
						}
						bi = lex_put(buf, bi, Q_DQ_CLOSE);
						if(lex_pos < lex_len) lex_pos = lex_pos + 1;
					}
					else if(c == '{') depth = depth + 1;
					else if(c == '}') depth = depth - 1;
				}
			}
			else
			{
				while(lex_pos < lex_len)
				{
					c = lex_src[lex_pos];
					if((c >= 'a' && c <= 'z') ||
					   (c >= 'A' && c <= 'Z') ||
					   (c >= '0' && c <= '9') ||
					   c == '_')
					{
						bi = lex_put(buf, bi, c);
						lex_pos = lex_pos + 1;
					}
					else if(c == '?' || c == '#' || c == '@' || c == '*' || c == '!' || c == '-')
					{
						bi = lex_put(buf, bi, c);
						lex_pos = lex_pos + 1;
						break;
					}
					else
					{
						break;
					}
				}
			}
		}
		else if(c == '`')
		{
			/* Backtick command substitution: rewrite `cmd` to $( cmd) so the
			   existing $(...) expansion handles it. */
			bi = lex_emit_backtick(buf, bi);
		}
		else
		{
			bi = lex_put(buf, bi, c);
			lex_pos = lex_pos + 1;
		}
	}

	require(bi < MAX_STR, "hs: token too long\n");
	buf[bi] = 0;
	if(parsing_persistent)
	{
		if(buf >= perm_base && buf < perm_base + perm_size)
		{
			perm_cur = perm_cur + ((bi + 8) & (~7));
		}
		return buf;
	}
	tmp_cur = tmp_cur + ((bi + 8) & (~7));
	if(tmp_cur > tmp_size) tmp_cur = tmp_size;
	return buf;
}

void tok_add(int type, char* val)
{
	require(tok_count < tok_capacity, "tok_add: too many tokens\n");
	tok_type[tok_count] = type;
	tok_val[tok_count] = val;
	tok_src_start[tok_count] = tok_start_pos;
	tok_count = tok_count + 1;
}

/* True if `w` is a valid `name=value` assignment word. */
int is_assignment(char* w)
{
	int i;
	int c;
	c = w[0];
	if(c == 0) return FALSE;
	if(!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_')) return FALSE;
	i = 1;
	while(w[i] != 0 && w[i] != '=')
	{
		c = w[i];
		if(!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) return FALSE;
		i = i + 1;
	}
	if(w[i] == '=') return TRUE;
	return FALSE;
}

/* Parse a heredoc operator's delimiter, starting at src[pos] (just past
 * the `<<`). Sets *strip for the `<<-` tab-stripping form and *expand
 * FALSE when the delimiter is quoted (a quoted delimiter means the body
 * is taken literally, with no expansion). The unquoted delimiter text is
 * written into delim. Returns the index just past the delimiter word. */
int heredoc_parse_op(char* src, int len, int pos, char* delim, int cap, int* strip, int* expand)
{
	int di;
	int c;

	*strip = FALSE;
	*expand = TRUE;
	if(pos < len && src[pos] == '-') { *strip = TRUE; pos = pos + 1; }
	while(pos < len && is_blank(src[pos])) pos = pos + 1;
	di = 0;
	while(pos < len)
	{
		c = src[pos];
		if(is_word_break(c)) break;
		if(c == '\'')
		{
			*expand = FALSE;
			pos = pos + 1;
			while(pos < len && src[pos] != '\'')
			{
				require(di + 1 < cap, "hs: heredoc delimiter too long\n");
				delim[di] = src[pos]; di = di + 1; pos = pos + 1;
			}
			if(pos < len) pos = pos + 1;
		}
		else if(c == '"')
		{
			*expand = FALSE;
			pos = pos + 1;
			while(pos < len && src[pos] != '"')
			{
				require(di + 1 < cap, "hs: heredoc delimiter too long\n");
				delim[di] = src[pos]; di = di + 1; pos = pos + 1;
			}
			if(pos < len) pos = pos + 1;
		}
		else if(c == '\\')
		{
			*expand = FALSE;
			pos = pos + 1;
			if(pos < len)
			{
				require(di + 1 < cap, "hs: heredoc delimiter too long\n");
				delim[di] = src[pos]; di = di + 1; pos = pos + 1;
			}
		}
		else
		{
			require(di + 1 < cap, "hs: heredoc delimiter too long\n");
			delim[di] = c; di = di + 1; pos = pos + 1;
		}
	}
	delim[di] = 0;
	return pos;
}

/* True if the line buf[ls..le) is a heredoc terminator matching `delim`
 * (length `dl`). For <<- (`strip`) leading tabs are skipped first. The
 * chunk-sizer (heredoc_skip_body, used by find_cmd_end) and the body reader
 * (lex_collect_heredocs) MUST agree byte-for-byte on where a body ends, so
 * both decide it through this one predicate -- a change here can't desync them.
 * The length test gates the memcmp (so it never reads past a short line, which
 * also matters under M2-Planet where `&&` does not short-circuit). */
int heredoc_line_is_terminator(char* buf, int ls, int le, char* delim, int dl, int strip)
{
	int cmp;
	cmp = ls;
	if(strip) { while(cmp < le && buf[cmp] == '\t') cmp = cmp + 1; }
	if(le - cmp == dl)
	{
		if(memcmp(buf + cmp, delim, dl) == 0) return TRUE;
	}
	return FALSE;
}

/* Skip a heredoc body in buf[pos..total], returning the index just past
 * the terminator line (or `total` if EOF arrives first). `strip` removes
 * leading tabs before the terminator comparison (for <<-). find_cmd_end
 * uses this to extend a command chunk past its heredoc bodies. */
int heredoc_skip_body(char* buf, int total, int pos, char* delim, int strip)
{
	int dl;
	int ls;
	int le;

	dl = strlen(delim);
	while(pos < total)
	{
		ls = pos;
		le = pos;
		while(le < total && buf[le] != '\n') le = le + 1;
		if(heredoc_line_is_terminator(buf, ls, le, delim, dl, strip))
		{
			if(le < total) return le + 1;
			return le;
		}
		if(le < total) pos = le + 1;
		else return le;
	}
	return pos;
}

/* Turn an expanding-heredoc body into the sentinel-annotated form that
 * expand_word consumes. The body is wrapped in double-quote sentinels so
 * expansion runs in "double-quoted" mode (parameter/command/arith
 * expansion, but no field splitting or globbing). Backslash escapes only
 * $, `, \ and newline -- matching heredoc rules, which differ from a real
 * double-quoted string (where \" is also special). Everything else,
 * including literal " characters and $... expansions, passes through for
 * expand_word to handle. Returned string lives in the perm pool (during
 * exec_buffer) or the tmp arena (eval/command substitution). */
char* heredoc_annotate(char* body)
{
	int bl;
	int cap;
	char* out;
	int oi;
	int i;
	int c;
	int nx;
	char* res;

	bl = strlen(body);
	cap = bl * 3 + 32;
	out = calloc(cap, 1);
	oi = 0;
	out[oi] = Q_DQ_OPEN; oi = oi + 1;
	i = 0;
	while(i < bl)
	{
		c = body[i];
		if(c == '\\')
		{
			/* body is NUL-terminated, so at the last char body[i+1] is the
			   terminator; read it explicitly as 0 rather than indexing past
			   the logical length (matches the backtick scan's i+1 < bl guard). */
			if(i + 1 < bl) nx = body[i + 1];
			else nx = 0;
			if(nx == '$' || nx == '`' || nx == '\\')
			{
				out[oi] = Q_SQ_OPEN; oi = oi + 1;
				out[oi] = nx; oi = oi + 1;
				out[oi] = Q_SQ_CLOSE; oi = oi + 1;
				i = i + 2;
			}
			else if(nx == '\n')
			{
				i = i + 2; /* line continuation */
			}
			else
			{
				out[oi] = '\\'; oi = oi + 1;
				i = i + 1;
			}
		}
		else if(c == '`')
		{
			/* Unescaped backtick command substitution in an expanding
			   heredoc body. The lexer's `->$() rewrite (lex_emit_backtick)
			   never runs here (heredoc bodies bypass lex_word), so do it now
			   on this buffer: autoconf writes `#define `...as_tr_cpp...` 1`
			   into confdefs.h via an unquoted heredoc and relies on the
			   backtick expanding. Keep the escape set -- ` \ $ -- in sync with
			   lex_emit_backtick. */
			out[oi] = '$'; oi = oi + 1;
			out[oi] = '('; oi = oi + 1;
			out[oi] = ' '; oi = oi + 1;
			i = i + 1;
			while(i < bl && body[i] != '`')
			{
				if(body[i] == '\\' && i + 1 < bl && (body[i + 1] == '`' ||
				   body[i + 1] == '\\' || body[i + 1] == '$'))
				{
					out[oi] = body[i + 1]; oi = oi + 1;
					i = i + 2;
				}
				else
				{
					out[oi] = body[i]; oi = oi + 1;
					i = i + 1;
				}
			}
			if(i < bl) i = i + 1; /* closing ` */
			out[oi] = ')'; oi = oi + 1;
		}
		else
		{
			out[oi] = c; oi = oi + 1;
			i = i + 1;
		}
	}
	out[oi] = Q_DQ_CLOSE; oi = oi + 1;
	out[oi] = 0;

	if(parsing_persistent) res = perm_alloc(oi + 1);
	else res = tmp_alloc(oi + 1);
	memcpy(res, out, oi + 1);
	free(out);
	return res;
}

/* Consume the bodies of all heredocs pending on the current line. Called
 * by the lexer once it reaches the newline that ends the operator line:
 * lex_pos is just past that newline, so the bodies sit at lex_pos in the
 * chunk (find_cmd_end already extended the chunk to include them). Each
 * body is read up to its terminator, then the recorded T_DLT token's
 * value is patched to a redirect pseudo-arg -- \x06h<body> (literal) or
 * \x06H<annotated> (expanded). exec_simple materializes the temp file. */
void lex_collect_heredocs()
{
	int k;
	char* delim;
	int strip;
	int expand;
	int t_idx;
	int dl;
	char* body;
	int bcap;
	int bl;
	int ls;
	int le;
	int cmp;
	int p;
	char* arg;
	char* ann;
	int alen;

	k = 0;
	while(k < hd_pending)
	{
		delim = hd_delim[k];
		strip = hd_strip[k];
		expand = hd_expand[k];
		t_idx = hd_tok[k];
		dl = strlen(delim);

		bcap = 4096;
		body = calloc(bcap, 1);
		bl = 0;

		while(lex_pos < lex_len)
		{
			ls = lex_pos;
			le = lex_pos;
			while(le < lex_len && lex_src[le] != '\n') le = le + 1;

			if(heredoc_line_is_terminator(lex_src, ls, le, delim, dl, strip))
			{
				if(le < lex_len) lex_pos = le + 1;
				else lex_pos = le;
				break;
			}

			/* Not a terminator: copy the body line (stripping leading tabs for
			   <<- too), starting past any stripped tabs. */
			cmp = ls;
			if(strip) { while(cmp < le && lex_src[cmp] == '\t') cmp = cmp + 1; }
			p = cmp;
			while(p < le)
			{
				out_grow(&body, &bcap, bl + 3);
				body[bl] = lex_src[p];
				bl = bl + 1;
				p = p + 1;
			}
			if(le < lex_len)
			{
				out_grow(&body, &bcap, bl + 3);
				body[bl] = '\n';
				bl = bl + 1;
				lex_pos = le + 1;
			}
			else
			{
				lex_pos = le; /* EOF before terminator */
				break;
			}
		}
		body[bl] = 0;

		if(expand)
		{
			ann = heredoc_annotate(body);
			alen = strlen(ann);
			if(parsing_persistent) arg = perm_alloc(alen + 3);
			else arg = tmp_alloc(alen + 3);
			arg[0] = '\x06';
			arg[1] = 'H';
			memcpy(arg + 2, ann, alen);
			arg[alen + 2] = 0;
		}
		else
		{
			if(parsing_persistent) arg = perm_alloc(bl + 3);
			else arg = tmp_alloc(bl + 3);
			arg[0] = '\x06';
			arg[1] = 'h';
			memcpy(arg + 2, body, bl);
			arg[bl + 2] = 0;
		}
		tok_val[t_idx] = arg;
		free(body);
		k = k + 1;
	}
	hd_pending = 0;
}

void lex_tokenize(char* input)
{
	int c;
	int cmd_pos; /* next word is in command position (alias expansion). */
	char* word;
	int ai;
	char* aval;
	char* new_src;
	int new_len;

	tok_count = 0;
	tok_pos = 0;
	lex_src = input;
	lex_orig_src = input;
	lex_stack_depth = 0;
	lex_pos = 0;
	lex_len = strlen(input);
	cmd_pos = TRUE;
	hd_pending = 0;

	while(lex_pos <= lex_len)
	{
		tok_start_pos = lex_pos;

		if(lex_pos >= lex_len)
		{
			/* Pop alias expansion stack if any. */
			if(lex_stack_depth > 0)
			{
				lex_stack_depth = lex_stack_depth - 1;
				lex_src = lex_stack_src[lex_stack_depth];
				lex_pos = lex_stack_pos[lex_stack_depth];
				lex_len = lex_stack_len[lex_stack_depth];
				continue;
			}
			/* A heredoc whose operator line had no trailing newline:
			   close it out with an empty body. */
			if(hd_pending > 0) lex_collect_heredocs();
			tok_add(T_EOF, "");
			break;
		}

		c = lex_src[lex_pos];

		if(is_blank(c))
		{
			lex_pos = lex_pos + 1;
		}
		else if(c == '#')
		{
			while(lex_pos < lex_len && lex_src[lex_pos] != '\n')
			{
				lex_pos = lex_pos + 1;
			}
		}
		else if(c == '\n')
		{
			tok_add(T_NL, "\n");
			lex_pos = lex_pos + 1;
			/* The bodies of any heredocs opened on this line follow the
			   newline; consume them now and patch their T_DLT tokens. */
			if(hd_pending > 0) lex_collect_heredocs();
			cmd_pos = TRUE;
		}
		else if(c == '|')
		{
			lex_pos = lex_pos + 1;
			if(lex_pos < lex_len && lex_src[lex_pos] == '|')
			{
				tok_add(T_OR, "||");
				lex_pos = lex_pos + 1;
			}
			else
			{
				tok_add(T_PIPE, "|");
			}
			cmd_pos = TRUE;
		}
		else if(c == '&')
		{
			lex_pos = lex_pos + 1;
			if(lex_pos < lex_len && lex_src[lex_pos] == '&')
			{
				tok_add(T_AND, "&&");
				lex_pos = lex_pos + 1;
			}
			else
			{
				tok_add(T_AMP, "&");
			}
			cmd_pos = TRUE;
		}
		else if(c == ';')
		{
			lex_pos = lex_pos + 1;
			if(lex_pos < lex_len && lex_src[lex_pos] == ';')
			{
				tok_add(T_DSEMI, ";;");
				lex_pos = lex_pos + 1;
			}
			else
			{
				tok_add(T_SEMI, ";");
			}
			cmd_pos = TRUE;
		}
		else if(c == '\\' && lex_pos + 1 < lex_len && lex_src[lex_pos + 1] == '\n')
		{
			/* Line continuation between tokens: a backslash-newline is
			   removed entirely (a `case` alternation `a | b \<nl> | c)`
			   must read as one token stream). Backslash escapes inside a
			   word are handled by lex_word instead. */
			lex_pos = lex_pos + 2;
		}
		else if(c == '<')
		{
			lex_pos = lex_pos + 1;
			if(lex_pos < lex_len && lex_src[lex_pos] == '<')
			{
				/* Heredoc operator: <<DELIM or <<-DELIM. Record it and
				   emit a placeholder T_DLT; the body is read off the
				   following lines when this line's newline is reached
				   (lex_collect_heredocs patches the token value). */
				int hstrip;
				int hexpand;
				lex_pos = lex_pos + 1;
				require(hd_pending < MAX_HEREDOC, "hs: too many heredocs on one line\n");
				hd_delim[hd_pending] = hd_delim_buf + hd_pending * MAX_HEREDOC_DELIM;
				lex_pos = heredoc_parse_op(lex_src, lex_len, lex_pos,
				                           hd_delim[hd_pending], MAX_HEREDOC_DELIM,
				                           &hstrip, &hexpand);
				hd_strip[hd_pending] = hstrip;
				hd_expand[hd_pending] = hexpand;
				hd_tok[hd_pending] = tok_count;
				tok_add(T_DLT, "");
				hd_pending = hd_pending + 1;
				cmd_pos = FALSE;
			}
			else if(lex_pos < lex_len && lex_src[lex_pos] == '&')
			{
				/* <&N input dup: emit < and "&N", mirroring >&N, so the
				   redirect parser handles it (e.g. autoconf's `exec 7<&0`). */
				lex_pos = lex_pos + 1;
				word = lex_word();
				tok_add(T_LT, "<");
				tok_add(T_WORD, tmp_cat2("&", word));
				cmd_pos = FALSE;
			}
			else
			{
				tok_add(T_LT, "<");
				cmd_pos = FALSE;
			}
		}
		else if(c == '>')
		{
			lex_pos = lex_pos + 1;
			if(lex_pos < lex_len && lex_src[lex_pos] == '>')
			{
				tok_add(T_GTGT, ">>");
				lex_pos = lex_pos + 1;
			}
			else if(lex_pos < lex_len && lex_src[lex_pos] == '&')
			{
				/* >&N: emit > and "&N" so the redirect parser handles it. */
				lex_pos = lex_pos + 1;
				word = lex_word();
				tok_add(T_GT, ">");
				tok_add(T_WORD, tmp_cat2("&", word));
				cmd_pos = FALSE;
			}
			else
			{
				tok_add(T_GT, ">");
			}
			cmd_pos = FALSE;
		}
		else if(c == '(')
		{
			tok_add(T_LPAREN, "(");
			lex_pos = lex_pos + 1;
			cmd_pos = TRUE;
		}
		else if(c == ')')
		{
			tok_add(T_RPAREN, ")");
			lex_pos = lex_pos + 1;
			cmd_pos = TRUE; /* a `case` pattern ) leads into a command. */
		}
		else
		{
			word = lex_word();

			if(cmd_pos && !is_keyword(word))
			{
				ai = al_find(word);
				if(ai >= 0)
				{
					aval = al_vals[ai];
					/* Push current source, switch to alias value.
					 * Zero copies; when alias is consumed, pop back. */
					if(lex_stack_depth < LEX_STACK_MAX)
					{
						lex_stack_src[lex_stack_depth] = lex_src;
						lex_stack_pos[lex_stack_depth] = lex_pos;
						lex_stack_len[lex_stack_depth] = lex_len;
						lex_stack_cmd[lex_stack_depth] = cmd_pos;
						lex_stack_depth = lex_stack_depth + 1;
						lex_src = aval;
						lex_pos = 0;
						lex_len = strlen(aval);
						/* cmd_pos unchanged - alias expands in current context */
					}
					continue;
				}
			}

			/* Fd-redirect: digits followed by > or <. */
			if(word[0] >= '0' && word[0] <= '9')
			{
				int all_digits;
				int wi;
				all_digits = TRUE;
				wi = 0;
				while(word[wi] != 0)
				{
					if(word[wi] < '0' || word[wi] > '9')
					{
						all_digits = FALSE;
						break;
					}
					wi = wi + 1;
				}
				/* An all-digit word IMMEDIATELY followed by a redirect (no
				   space) is an fd specifier: `2>file`, `2>>file`, `2>&1`,
				   `0<file`. The fd is carried in the redirect token's VALUE so
				   the parser can tell it from a spaced `2 > file` (where `2` is a
				   plain word and the operator below carries its own "<"/">"/">>"
				   string instead). word_is_digits on the token value is the test. */
				if(all_digits && lex_pos < lex_len)
				{
					c = lex_src[lex_pos];
					if(c == '>')
					{
						lex_pos = lex_pos + 1;
						if(lex_pos < lex_len && lex_src[lex_pos] == '>')
						{
							tok_add(T_GTGT, word);
							lex_pos = lex_pos + 1;
							cmd_pos = FALSE;
							continue;
						}
						else if(lex_pos < lex_len && lex_src[lex_pos] == '&')
						{
							lex_pos = lex_pos + 1;
							tok_add(T_GT, word);
							tok_add(T_WORD, tmp_cat2("&", lex_word()));
							cmd_pos = FALSE;
							continue;
						}
						else
						{
							tok_add(T_GT, word);
							cmd_pos = FALSE;
							continue;
						}
					}
					else if(c == '<')
					{
						lex_pos = lex_pos + 1;
						if(lex_pos < lex_len && lex_src[lex_pos] == '&')
						{
							/* N<&M attached input dup, e.g. `7<&0`. */
							lex_pos = lex_pos + 1;
							tok_add(T_LT, word);
							tok_add(T_WORD, tmp_cat2("&", lex_word()));
							cmd_pos = FALSE;
							continue;
						}
						tok_add(T_LT, word);
						cmd_pos = FALSE;
						continue;
					}
				}
			}

			if(cmd_pos && is_assignment(word))
			{
				tok_add(T_ASSIGN, word);
				/* cmd_pos stays TRUE for chained assignments. */
			}
			else
			{
				tok_add(T_WORD, word);
				if(match(word, "!"))
				{
					cmd_pos = TRUE;
				}
				else
				{
					cmd_pos = is_keyword(word);
				}
			}
		}
	}
}

/* Parser: recursive descent. */
int nd_new(int type)
{
	int idx;
	require(nd_count < MAX_ND, "nd_new: too many nodes\n");
	idx = nd_count;
	nd_type[idx] = type;
	nd_a[idx] = -1;
	nd_b[idx] = -1;
	nd_c[idx] = -1;
	nd_d[idx] = -1;
	nd_str[idx] = NULL;
	nd_argv[idx] = NULL;
	nd_assigns[idx] = NULL;
	nd_count = nd_count + 1;
	return idx;
}

void nd_init()
{
	nd_type = calloc(MAX_ND, sizeof(int));
	nd_a    = calloc(MAX_ND, sizeof(int));
	nd_b    = calloc(MAX_ND, sizeof(int));
	nd_c    = calloc(MAX_ND, sizeof(int));
	nd_d    = calloc(MAX_ND, sizeof(int));
	nd_str     = calloc(MAX_ND, sizeof(char*));
	nd_argv    = calloc(MAX_ND, sizeof(char*));
	nd_assigns = calloc(MAX_ND, sizeof(char*));
	nd_count = 0;
}

int tok_peek()
{
	if(tok_pos >= tok_count) return T_EOF;
	return tok_type[tok_pos];
}

char* tok_peek_val()
{
	if(tok_pos >= tok_count) return "";
	return tok_val[tok_pos];
}

int tok_next()
{
	int t;
	t = tok_peek();
	if(tok_pos < tok_count) tok_pos = tok_pos + 1;
	return t;
}

char* tok_next_val()
{
	char* v;
	v = tok_peek_val();
	if(tok_pos < tok_count) tok_pos = tok_pos + 1;
	return v;
}

/* Walk lex_orig_src to convert a byte offset into 1-based line and
   column for the character AT that offset. O(n) but only called on
   errors. */
void compute_line_col(int pos, int* out_line, int* out_col)
{
	int line;
	int col;
	int i;
	line = 1;
	col = 1;
	if(lex_orig_src == NULL)
	{
		out_line[0] = 0;
		out_col[0] = 0;
		return;
	}
	i = 0;
	while(i < pos && lex_orig_src[i] != 0)
	{
		if(lex_orig_src[i] == '\n')
		{
			line = line + 1;
			col = 1;
		}
		else
		{
			col = col + 1;
		}
		i = i + 1;
	}
	/* Above advances col after consuming each char, so when the loop
	   exits col is one past the column of lex_orig_src[pos-1]. That
	   is exactly the column of lex_orig_src[pos], except after a 
	   newline, where the loop has already reset col to 1. 
	   So no further adjustment is needed. */
	out_line[0] = line;
	out_col[0] = col;
}

/* Print "hs: <path>:line:col: <msg>" and a short snippet of the line
   containing the error. The snippet stops at the next newline so a
   long line doesn't dump the rest of the script. */
void parse_err(int src_pos, char* msg)
{
	int line;
	int col;
	int line_start;
	int line_end;
	int i;
	compute_line_col(src_pos, &line, &col);
	if(line > 0 && lex_line_offset > 0) line = line + lex_line_offset - 1;
	sh_err_puts("hs: ");
	if(lex_orig_path != NULL)
	{
		sh_err_puts(lex_orig_path);
		sh_err_puts(":");
	}
	if(line > 0)
	{
		sh_err_puts(int_to_str(line));
		sh_err_puts(":");
		sh_err_puts(int_to_str(col));
		sh_err_puts(": ");
	}
	sh_err_puts(msg);
	sh_err_puts("\n");
	if(lex_orig_src == NULL || line <= 0) { exit(2); }
	/* Find the start of the line. */
	line_start = src_pos;
	while(line_start > 0 && lex_orig_src[line_start - 1] != '\n') line_start = line_start - 1;
	line_end = line_start;
	while(lex_orig_src[line_end] != 0 && lex_orig_src[line_end] != '\n') line_end = line_end + 1;
	sh_err_puts("    ");
	i = line_start;
	while(i < line_end)
	{
		sh_err_putc(lex_orig_src[i]);
		i = i + 1;
	}
	sh_err_puts("\n    ");
	i = line_start;
	while(i < src_pos)
	{
		if(lex_orig_src[i] == '\t') sh_err_putc('\t');
		else sh_err_putc(' ');
		i = i + 1;
	}
	sh_err_puts("^\n");
	exit(2);
}

/* Position of the current token (or end-of-source if none left). */
int cur_tok_pos()
{
	if(tok_pos >= tok_count) return lex_len;
	return tok_src_start[tok_pos];
}

void tok_expect(char* w)
{
	char* v;
	int err_pos;
	char* msg;
	err_pos = cur_tok_pos();
	v = tok_next_val();
	if(!match(v, w))
	{
		msg = tmp_cat3("expected '", w, tmp_cat3("' got '", v, "'"));
		parse_err(err_pos, msg);
	}
}

void skip_nl()
{
	while(tok_peek() == T_NL) tok_next();
}

/* Recursive-descent forward decls. */
int parse_list();
int parse_andor();
int parse_pipeline();
int parse_command();
int parse_simple();
int parse_if();
int parse_while();
int parse_for();
int parse_case();
int parse_brace();
int parse_subsh();

int parse_list()
{
	int left;
	int idx;
	int t;

	skip_nl();
	if(tok_peek() == T_EOF || match(tok_peek_val(), ")") ||
	   match(tok_peek_val(), "}") || match(tok_peek_val(), "esac") ||
	   match(tok_peek_val(), "fi") || match(tok_peek_val(), "done") ||
	   match(tok_peek_val(), "elif") || match(tok_peek_val(), "else") ||
	   match(tok_peek_val(), "do") || match(tok_peek_val(), "then"))
	{
		return -1;
	}

	left = parse_andor();
	if(left < 0) return -1;

	while(TRUE)
	{
		t = tok_peek();
		/* `;`, newline and `&` all separate list elements. hs has no job
		   control, so `&` (background) is accepted and the command runs
		   synchronously -- enough for configure's `( sleep 1 ) &` timestamp
		   trick, which only needs it to parse and eventually complete. */
		if(t == T_SEMI || t == T_NL || t == T_AMP)
		{
			tok_next();
			skip_nl();
			if(tok_peek() == T_EOF || match(tok_peek_val(), ")") ||
			   match(tok_peek_val(), "}") || match(tok_peek_val(), "esac") ||
			   match(tok_peek_val(), "fi") || match(tok_peek_val(), "done") ||
			   match(tok_peek_val(), "elif") || match(tok_peek_val(), "else") ||
			   match(tok_peek_val(), "do") || match(tok_peek_val(), "then") ||
			   tok_peek() == T_DSEMI)
			{
				break;
			}
			idx = nd_new(CMD_LIST);
			nd_a[idx] = left;
			nd_b[idx] = parse_andor();
			left = idx;
		}
		else
		{
			break;
		}
	}
	return left;
}

int parse_andor()
{
	int left;
	int idx;
	int t;

	left = parse_pipeline();
	while(TRUE)
	{
		t = tok_peek();
		if(t == T_AND)
		{
			tok_next();
			skip_nl();
			idx = nd_new(CMD_AND);
			nd_a[idx] = left;
			nd_b[idx] = parse_pipeline();
			left = idx;
		}
		else if(t == T_OR)
		{
			tok_next();
			skip_nl();
			idx = nd_new(CMD_OR);
			nd_a[idx] = left;
			nd_b[idx] = parse_pipeline();
			left = idx;
		}
		else
		{
			break;
		}
	}
	return left;
}

int parse_pipeline()
{
	int left;
	int idx;
	int negated;

	negated = FALSE;
	if(match(tok_peek_val(), "!"))
	{
		negated = TRUE;
		tok_next();
	}

	left = parse_command();

	while(tok_peek() == T_PIPE)
	{
		tok_next();
		skip_nl();
		idx = nd_new(CMD_PIPE);
		nd_a[idx] = left;
		nd_b[idx] = parse_command();
		left = idx;
	}

	if(negated)
	{
		idx = nd_new(CMD_NOT);
		nd_a[idx] = left;
		left = idx;
	}

	return left;
}

int parse_depth;

/* Collect redirects trailing a compound command (e.g. `done < file`,
 * `done > out`, a heredoc on a loop). Returns `child` unchanged when none
 * follow, else a CMD_REDIR node wrapping `child` with the redirect
 * pseudo-args. Simple commands carry their own redirects, so this only
 * applies to compound commands. */
/* TRUE if w is a non-empty run of digits (an fd number like the 2 in 2>&1). */
int word_is_digits(char* w)
{
	int i;
	/* Split, not `w == NULL || w[0] == 0`: M2-Planet doesn't short-circuit
	   ||, so the combined form reads w[0] off a NULL pointer. */
	if(w == NULL) return FALSE;
	if(w[0] == 0) return FALSE;
	i = 0;
	while(w[i] != 0)
	{
		if(w[i] < '0' || w[i] > '9') return FALSE;
		i = i + 1;
	}
	return TRUE;
}

/* Build the \x06-prefixed redirect pseudo-arg for the redirect operator the
   parser is sitting on. `op` is the operator string ("<", ">", ">>"). The
   operator token's value is the attached fd digits when the lexer saw `N<file`
   (no space), or the operator string itself for a bare/space-separated
   redirect; word_is_digits tells them apart. Consumes the operator token and
   the following filename token. Shared by parse_simple and the compound path. */
char* parse_redir_arg(char* op)
{
	char* fdw;
	char* rfile;
	fdw = tok_peek_val();
	tok_next();
	rfile = tok_next_val();
	if(word_is_digits(fdw)) return tmp_cat3("\x06", fdw, tmp_cat2(op, rfile));
	return tmp_cat3("\x06", op, rfile);
}

/* TRUE if the parser is positioned at the start of a trailing redirect. The
   leading fd (if any) now rides in the redirect token's value, so this is just
   a redirect-operator check. */
int at_compound_redir()
{
	int t;
	t = tok_peek();
	if(t == T_LT) return TRUE;
	if(t == T_GT) return TRUE;
	if(t == T_GTGT) return TRUE;
	if(t == T_DLT) return TRUE;
	return FALSE;
}

int parse_compound_redirs(int child)
{
	int t;
	char* rfile;
	char** redirs;
	int rc;
	int idx;

	if(!at_compound_redir()) return child;

	redirs = tmp_alloc(MAX_ARGV * sizeof(char*));
	rc = 0;
	while(TRUE)
	{
		t = tok_peek();
		if(t == T_LT)
		{
			redirs[rc] = parse_redir_arg("<"); rc = rc + 1;
		}
		else if(t == T_GT)
		{
			redirs[rc] = parse_redir_arg(">"); rc = rc + 1;
		}
		else if(t == T_GTGT)
		{
			redirs[rc] = parse_redir_arg(">>"); rc = rc + 1;
		}
		else if(t == T_DLT)
		{
			redirs[rc] = tok_next_val(); rc = rc + 1;
		}
		else break;
	}

	idx = nd_new(CMD_REDIR);
	nd_a[idx] = child;
	nd_b[idx] = rc;
	if(parsing_persistent)
	{
		char** hr;
		int hi;
		hr = perm_alloc((rc + 1) * sizeof(char*));
		hi = 0;
		while(hi < rc) { hr[hi] = perm_dup(redirs[hi]); hi = hi + 1; }
		hr[rc] = NULL;
		nd_argv[idx] = hr;
	}
	else
	{
		redirs[rc] = NULL;
		nd_argv[idx] = redirs;
	}
	return idx;
}

int parse_command()
{
	char* w;
	int t;
	int r;
	int compound;

	parse_depth = parse_depth + 1;
	require(parse_depth < MAX_DEPTH, "hs: nested commands too deep\n");

	t = tok_peek();
	w = tok_peek_val();

	compound = TRUE;
	if(match(w, "if")) r = parse_if();
	else if(match(w, "while") || match(w, "until")) r = parse_while();
	else if(match(w, "for")) r = parse_for();
	else if(match(w, "case")) r = parse_case();
	else if(match(w, "{")) r = parse_brace();
	else if(t == T_LPAREN) r = parse_subsh();
	else { r = parse_simple(); compound = FALSE; }

	/* A compound command may be followed by redirects (done < file). */
	if(compound && r >= 0) r = parse_compound_redirs(r);

	parse_depth = parse_depth - 1;
	return r;
}

int parse_if()
{
	int idx;
	int cond;
	int then_body;
	int else_body;

	/* elif recurses into parse_if. */
	if(match(tok_peek_val(), "elif"))
	{
		tok_next();
	}
	else
	{
		tok_expect("if");
	}
	skip_nl();
	cond = parse_list();
	skip_nl();
	tok_expect("then");
	skip_nl();
	then_body = parse_list();
	skip_nl();

	else_body = -1;
	if(match(tok_peek_val(), "elif"))
	{
		else_body = parse_if();
	}
	else if(match(tok_peek_val(), "else"))
	{
		tok_next();
		skip_nl();
		else_body = parse_list();
		skip_nl();
		tok_expect("fi");
	}
	else
	{
		tok_expect("fi");
	}

	idx = nd_new(CMD_IF);
	nd_a[idx] = cond;
	nd_b[idx] = then_body;
	nd_c[idx] = else_body;
	return idx;
}

int parse_while()
{
	int idx;
	int is_until;
	int cond;
	int body;

	is_until = match(tok_peek_val(), "until");
	tok_next();
	skip_nl();
	cond = parse_list();
	skip_nl();
	tok_expect("do");
	skip_nl();
	body = parse_list();
	skip_nl();
	tok_expect("done");

	idx = nd_new(CMD_WHILE);
	nd_a[idx] = cond;
	nd_b[idx] = body;
	nd_c[idx] = is_until; /* 0=while, 1=until */
	return idx;
}

int parse_for()
{
	int idx;
	char* varname;
	int body;
	char* words_buf;
	int wbi;
	char* wv;

	tok_expect("for");
	varname = tok_next_val();
	skip_nl();

	words_buf = tmp_alloc(4096);
	wbi = 0;

	if(match(tok_peek_val(), "in"))
	{
		tok_next();
		while(tok_peek() != T_SEMI && tok_peek() != T_NL && tok_peek() != T_EOF)
		{
			int wvlen;
			if(wbi > 0)
			{
				require(wbi + 1 < 4096, "hs: for: word list too long\n");
				words_buf[wbi] = Q_SPLIT;
				wbi = wbi + 1;
			}
			wv = tok_next_val();
			wvlen = strlen(wv);
			require(wbi + wvlen + 1 < 4096, "hs: for: word list too long\n");
			memcpy(words_buf + wbi, wv, wvlen);
			wbi = wbi + wvlen;
		}
		if(tok_peek() == T_SEMI || tok_peek() == T_NL) tok_next();
	}
	else
	{
		/* `for var; do` and `for var do` iterate "$@". */
		if(tok_peek() == T_SEMI) tok_next();
		words_buf[0] = '$';
		words_buf[1] = '@';
		wbi = 2;
	}
	words_buf[wbi] = 0;

	skip_nl();
	tok_expect("do");
	skip_nl();
	body = parse_list();
	skip_nl();
	tok_expect("done");

	idx = nd_new(CMD_FOR);
	nd_str[idx] = str_dup(varname);
	nd_a[idx] = body;
	/* Word list lives in a synthesized child node. */
	{
		int wn;
		wn = nd_new(CMD_SIMPLE);
		nd_str[wn] = str_dup(words_buf);
		nd_b[idx] = wn;
	}
	return idx;
}

int parse_case()
{
	int idx;
	int first_item;
	int last_item;
	int item;
	char* word;
	char* pat_buf;
	int pat_cap;
	int pbi;
	int body;
	char* pv;

	tok_expect("case");
	word = tok_next_val();
	skip_nl();
	tok_expect("in");
	skip_nl();

	idx = nd_new(CMD_CASE);
	nd_str[idx] = str_dup(word);

	first_item = -1;
	last_item = -1;

	/* One pattern buffer reused across items; grows on demand because
	   config.sub's case has very long `a | b | c | ...` pattern lists. */
	pat_cap = 8192;
	pat_buf = calloc(pat_cap, 1);

	while(!match(tok_peek_val(), "esac") && tok_peek() != T_EOF)
	{
		skip_nl();
		if(match(tok_peek_val(), "esac")) break;

		if(tok_peek() == T_LPAREN) tok_next();

		pbi = 0;
		while(TRUE)
		{
			int pvlen;
			pv = tok_next_val();
			pvlen = strlen(pv);
			out_grow(&pat_buf, &pat_cap, pbi + pvlen + 3);
			memcpy(pat_buf + pbi, pv, pvlen);
			pbi = pbi + pvlen;
			if(tok_peek() == T_PIPE)
			{
				tok_next();
				pat_buf[pbi] = '|';
				pbi = pbi + 1;
			}
			else
			{
				break;
			}
		}
		pat_buf[pbi] = 0;
		tok_expect(")");
		skip_nl();

		body = parse_list();
		skip_nl();

		item = nd_new(CMD_CASEI);
		nd_str[item] = str_dup(pat_buf);
		/* (pat_buf is reused for the next item; freed after the loop.) */
		nd_a[item] = body;
		nd_b[item] = -1;

		if(first_item < 0) first_item = item;
		if(last_item >= 0) nd_b[last_item] = item;
		last_item = item;

		if(tok_peek() == T_DSEMI)
		{
			tok_next();
			skip_nl();
		}
	}
	tok_expect("esac");

	free(pat_buf);
	nd_a[idx] = first_item;
	return idx;
}

int parse_brace()
{
	int body;
	tok_expect("{");
	skip_nl();
	body = parse_list();
	skip_nl();
	tok_expect("}");
	return body;
}

int parse_subsh()
{
	int idx;
	int body;
	tok_next();
	skip_nl();
	body = parse_list();
	skip_nl();
	tok_expect(")");
	idx = nd_new(CMD_SUBSH);
	nd_a[idx] = body;
	return idx;
}

/* True if the last parsed word is a bare fd number, so a following `N<file`
   attaches to it. Written as nested ifs, NOT `argc > 0 && argv[argc-1][0]...`:
   M2-Planet does not short-circuit &&, so the flat form dereferences argv[-1]
   when argc==0 -- which a redirect-only command like `>file` (autoconf's
   `>$cache_file`) hits, crashing the parser. */
/* True if an expanded argv entry is a \x06-prefixed redirection pseudo-arg.
   A function (not `a != NULL && a[0] == ...`) so M2-Planet's non-short-circuit
   && can't deref a NULL entry. */
int is_redir_word(char* a)
{
	if(a == NULL) return FALSE;
	return a[0] == '\x06';
}

int parse_simple()
{
	int idx;
	int argc;
	char** argv;
	int t;
	char* w;
	char* rfile;
	int rfd;
	char* assign_buf;
	int assign_count;
	char** assigns;
	int saw_word;
	int fn_idx;
	int body;

	idx = nd_new(CMD_SIMPLE);
	argv = tmp_alloc(MAX_ARGV * sizeof(char*));
	assigns = tmp_alloc(MAX_ARGV * sizeof(char*));
	argc = 0;
	assign_count = 0;
	saw_word = FALSE;

	/* Redirects are stored as \x06-prefixed pseudo-args inside argv. */
	while(TRUE)
	{
		/* One slot is consumed per iteration at most; bound both arrays
		   (argv keeps a NULL terminator slot, hence -1). */
		require(argc < MAX_ARGV - 1, "hs: too many arguments\n");
		require(assign_count < MAX_ARGV - 1, "hs: too many assignments\n");
		t = tok_peek();

		if(t == T_ASSIGN && !saw_word)
		{
			assigns[assign_count] = tok_next_val();
			assign_count = assign_count + 1;
		}
		else if(t == T_WORD)
		{
			w = tok_next_val();

			/* Function definition: name() { ... } */
			if(!saw_word && argc == 0 && tok_peek() == T_LPAREN)
			{
				tok_next();
				if(tok_peek() == T_RPAREN)
				{
					int body_first_tok;
					int body_is_brace;
					tok_next();
					skip_nl();
					body_first_tok = tok_pos;
					body_is_brace = match(tok_peek_val(), "{");
					body = parse_command();
					fn_idx = nd_new(CMD_FUNC);
					nd_str[fn_idx] = str_dup(w);
					nd_a[fn_idx] = body;
					nd_c[fn_idx] = parsing_persistent;
					if(!parsing_persistent)
					{
						/* eval-defined: the parse-tree AST is in the
						   tmp arena and gets reset between commands,
						   so we have to keep the body as a re-parsable
						   string. Slice it out of the original source
						   from the body's first token to just past the
						   closing `}` (the last token consumed by
						   parse_command). Storing only the body prevents
						   re-running the definition site on every call,
						   which would otherwise infinite-recurse via
						   the call site that follows.

						   The non-brace fallback (e.g. `f() simple`) is
						   not used by hs's tests and would need a token
						   length table to slice precisely; for those
						   cases we keep the old behavior of storing
						   the whole source. */
						if(body_is_brace && tok_pos > body_first_tok)
						{
							int body_src_start;
							int body_src_end;
							int body_src_len;
							char* body_src;
							body_src_start = tok_src_start[body_first_tok];
							/* tok_pos - 1 is the `}`; +1 to include it. */
							body_src_end = tok_src_start[tok_pos - 1] + 1;
							body_src_len = body_src_end - body_src_start;
							body_src = tmp_alloc(body_src_len + 1);
							memcpy(body_src, lex_orig_src + body_src_start, body_src_len);
							body_src[body_src_len] = 0;
							fn_set(w, body_src);
						}
						else
						{
							fn_set(w, lex_orig_src);
						}
					}
					return fn_idx;
				}
				else
				{
					/* Not a function def. We already consumed (; treat
					 * the word as a normal argv entry and continue. */
					argv[argc] = w;
					argc = argc + 1;
					saw_word = TRUE;
				}
			}
			else
			{
				argv[argc] = w;
				argc = argc + 1;
				saw_word = TRUE;
			}
		}
		else if(t == T_LT)
		{
			/* The leading fd (if `N<` was attached, no space) rides in the
			   redirect token's value; parse_redir_arg reads it from there. */
			argv[argc] = parse_redir_arg("<");
			argc = argc + 1;
		}
		else if(t == T_GT)
		{
			argv[argc] = parse_redir_arg(">");
			argc = argc + 1;
		}
		else if(t == T_GTGT)
		{
			argv[argc] = parse_redir_arg(">>");
			argc = argc + 1;
		}
		else if(t == T_DLT)
		{
			/* Heredoc: the lexer already built the \x06h/\x06H redirect
			   pseudo-arg (body or annotated body) as the token value. */
			argv[argc] = tok_next_val();
			argc = argc + 1;
		}
		else
		{
			break;
		}
	}

	argv[argc] = NULL;

	nd_str[idx] = NULL;
	nd_a[idx] = argc;

	if(parsing_persistent)
	{
		/* Copy argv/assigns into the perm pool. */
		{
			char** hargv;
			char** hassigns;
			int hi;
			hargv = perm_alloc((argc + 1) * sizeof(char*));
			hi = 0;
			while(hi < argc) { hargv[hi] = perm_dup(argv[hi]); hi = hi + 1; }
			hargv[argc] = NULL;
			nd_argv[idx] = hargv;
			hassigns = perm_alloc((assign_count + 1) * sizeof(char*));
			hi = 0;
			while(hi < assign_count) { hassigns[hi] = perm_dup(assigns[hi]); hi = hi + 1; }
			nd_c[idx] = assign_count;
			nd_assigns[idx] = hassigns;
		}
	}
	else
	{
		nd_argv[idx] = argv;
		nd_c[idx] = assign_count;
		nd_assigns[idx] = assigns;
	}

	return idx;
}

void parse_init(char* input)
{
	/* Don't reset nd_count: outer-context nodes must persist. */
	lex_tokenize(input);
}

/* Pattern matching (glob for case/esac and ${var#pat} forms). */
/* Recursion-bounded pattern matcher. The depth parameter caps the
   stack growth from nested `*` wildcards (each new `*` recurses on
   every suffix). pat_match() below is the public entry point that
   resets depth. */
int pat_match_d(char* pat, char* str, int depth)
{
	int pc;
	int sc;
	int neg;
	int matched;
	int lo;
	int hi;
	int in_quote;
	int first;

	require(depth < 256, "hs: pattern too deeply nested\n");
	in_quote = FALSE;

	while(TRUE)
	{
		pc = pat[0];
		sc = str[0];

		if(pc == 0) return sc == 0;

		/* Track quote sentinels - chars inside quotes are literal */
		if(pc == Q_SQ_OPEN || pc == Q_DQ_OPEN)
		{
			in_quote = TRUE;
			pat = pat + 1;
			continue;
		}
		if(pc == Q_SQ_CLOSE || pc == Q_DQ_CLOSE)
		{
			in_quote = FALSE;
			pat = pat + 1;
			continue;
		}

		/* When inside quotes, treat everything as literal */
		if(in_quote)
		{
			if(pc != sc) return FALSE;
			pat = pat + 1;
			str = str + 1;
			continue;
		}

		/* Backslash quotes the next pattern char: it matches that char
		   literally, defusing any glob metacharacter. POSIX uses this in
		   `${v#\$[\(\{]}`-style patterns (e.g. GNU make's build.sh
		   get_mk_var). Inside quotes the branch above already treats the
		   backslash literally, matching shell quoting rules. */
		if(pc == '\\' && pat[1] != 0)
		{
			if(pat[1] != sc) return FALSE;
			pat = pat + 2;
			str = str + 1;
			continue;
		}

		if(pc == '*')
		{
			pat = pat + 1;
			while(pat[0] == Q_SQ_CLOSE || pat[0] == Q_DQ_CLOSE) pat = pat + 1;
			if(pat[0] == 0) return TRUE;
			while(str[0] != 0)
			{
				if(pat_match_d(pat, str, depth + 1)) return TRUE;
				str = str + 1;
			}
			return pat_match_d(pat, str, depth + 1);
		}

		if(pc == '?')
		{
			if(sc == 0) return FALSE;
			pat = pat + 1;
			str = str + 1;
		}
		else if(pc == '[')
		{
			if(sc == 0) return FALSE;
			pat = pat + 1;
			neg = FALSE;
			if(pat[0] == '!' || pat[0] == '^')
			{
				neg = TRUE;
				pat = pat + 1;
			}
			matched = FALSE;
			/* A `]` immediately after `[` or `[!` is a literal member, not
			   the class terminator (POSIX): scan it on the first iteration. */
			first = TRUE;
			while(pat[0] != 0)
			{
				if(pat[0] == ']' && !first) break;
				first = FALSE;
				lo = pat[0];
				pat = pat + 1;
				if(pat[0] == '-' && pat[1] != 0 && pat[1] != ']')
				{
					pat = pat + 1;
					hi = pat[0];
					pat = pat + 1;
					if(sc >= lo && sc <= hi) matched = TRUE;
				}
				else
				{
					if(sc == lo) matched = TRUE;
				}
			}
			if(pat[0] == ']') pat = pat + 1;
			if(neg) matched = !matched;
			if(!matched) return FALSE;
			str = str + 1;
		}
		else
		{
			if(pc != sc) return FALSE;
			pat = pat + 1;
			str = str + 1;
		}
	}
}

/* Public wrapper: callers don't need to track depth. */
int pat_match(char* pat, char* str)
{
	return pat_match_d(pat, str, 0);
}

/* Match a case pattern, splitting on | for alternation. */
int case_match(char* pat, char* str)
{
	char* seg;
	int si;
	int c;
	int i;

	seg = tmp_alloc(MAX_STR);
	si = 0;
	i = 0;

	{
		int in_sq;
		int in_dq;
		in_sq = FALSE;
		in_dq = FALSE;
		while(TRUE)
		{
			c = pat[i];
			/* Track quote sentinels so | inside quotes is literal. */
			if(c == Q_SQ_OPEN && !in_dq) in_sq = TRUE;
			else if(c == Q_SQ_CLOSE && in_sq) in_sq = FALSE;
			else if(c == Q_DQ_OPEN && !in_sq) in_dq = TRUE;
			else if(c == Q_DQ_CLOSE && in_dq) in_dq = FALSE;

			if((c == '|' && !in_sq && !in_dq) || c == 0)
			{
				seg[si] = 0;
				if(pat_match(seg, str)) return TRUE;
				if(c == 0) break;
				si = 0;
				i = i + 1;
			}
			else
			{
				seg[si] = c;
				si = si + 1;
				i = i + 1;
			}
		}
	}
	return FALSE;
}

/* Arithmetic evaluator: recursive descent with C precedence. */
char* arith_src;
int   arith_pos;
int   arith_len;
/* >0 while parsing the untaken branch of a ?: ternary: the branch is still
   scanned (single-pass parser must advance arith_pos) but its side effects --
   currently only the divide/modulo-by-zero diagnostic -- are suppressed. */
int   arith_noeval;

void arith_skip_space()
{
	while(arith_pos < arith_len && (arith_src[arith_pos] == ' ' || arith_src[arith_pos] == '\t'))
	{
		arith_pos = arith_pos + 1;
	}
}

int arith_expr();
int arith_ternary();
int arith_logor();
int arith_logand();
int arith_bitor();
int arith_bitxor();
int arith_bitand();
int arith_equality();
int arith_relational();
int arith_shift();
int arith_additive();
int arith_multiplicative();
int arith_unary();
int arith_primary();

int arith_primary()
{
	int val;
	int c;
	int neg;
	char* name;
	int ni;

	arith_skip_space();
	c = arith_src[arith_pos];

	if(c == '(')
	{
		arith_pos = arith_pos + 1;
		val = arith_expr();
		arith_skip_space();
		if(arith_pos < arith_len && arith_src[arith_pos] == ')')
		{
			arith_pos = arith_pos + 1;
		}
		return val;
	}

	if(c >= '0' && c <= '9')
	{
		val = 0;
		while(arith_pos < arith_len && arith_src[arith_pos] >= '0' && arith_src[arith_pos] <= '9')
		{
			val = val * 10 + (arith_src[arith_pos] - '0');
			arith_pos = arith_pos + 1;
		}
		return val;
	}

	/* Variable reference (no $ prefix in $((...)) context). */
	if((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_')
	{
		name = tmp_alloc(256);
		ni = 0;
		while(arith_pos < arith_len)
		{
			c = arith_src[arith_pos];
			if((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')
			{
				require(ni < 255, "hs: arithmetic identifier too long\n");
				name[ni] = c;
				ni = ni + 1;
				arith_pos = arith_pos + 1;
			}
			else
			{
				break;
			}
		}
		name[ni] = 0;
		val = str_to_int(var_get(name));
		/* name from tmp arena */
		return val;
	}

	/* $ prefix - skip it and read var */
	if(c == '$')
	{
		arith_pos = arith_pos + 1;
		return arith_primary();
	}

	return 0;
}

int arith_unary()
{
	int c;
	arith_skip_space();
	c = arith_src[arith_pos];

	if(c == '-')
	{
		arith_pos = arith_pos + 1;
		return 0 - arith_unary();
	}
	if(c == '+')
	{
		arith_pos = arith_pos + 1;
		return arith_unary();
	}
	if(c == '~')
	{
		arith_pos = arith_pos + 1;
		return ~arith_unary();
	}
	if(c == '!')
	{
		arith_pos = arith_pos + 1;
		return !arith_unary();
	}
	return arith_primary();
}

int arith_multiplicative()
{
	int left;
	int c;
	left = arith_unary();
	while(TRUE)
	{
		arith_skip_space();
		c = arith_src[arith_pos];
		if(c == '*')
		{
			arith_pos = arith_pos + 1;
			left = left * arith_unary();
		}
		else if(c == '/' && arith_src[arith_pos + 1] != '/')
		{
			int d;
			arith_pos = arith_pos + 1;
			/* Guard against SIGFPE: a zero divisor (or zero from an
			   untaken ternary branch, which is still parsed) must not crash
			   the shell. Report once and treat the result as 0. */
			d = arith_unary();
			if(d == 0) { if(arith_noeval == 0) sh_err_puts("hs: arithmetic: division by zero\n"); left = 0; }
			else left = left / d;
		}
		else if(c == '%')
		{
			int d;
			arith_pos = arith_pos + 1;
			d = arith_unary();
			if(d == 0) { if(arith_noeval == 0) sh_err_puts("hs: arithmetic: modulo by zero\n"); left = 0; }
			else left = left % d;
		}
		else
		{
			break;
		}
	}
	return left;
}

int arith_additive()
{
	int left;
	int c;
	left = arith_multiplicative();
	while(TRUE)
	{
		arith_skip_space();
		c = arith_src[arith_pos];
		if(c == '+')
		{
			arith_pos = arith_pos + 1;
			left = left + arith_multiplicative();
		}
		else if(c == '-')
		{
			arith_pos = arith_pos + 1;
			left = left - arith_multiplicative();
		}
		else
		{
			break;
		}
	}
	return left;
}

int arith_shift()
{
	int left;
	left = arith_additive();
	while(TRUE)
	{
		arith_skip_space();
		if(arith_src[arith_pos] == '<' && arith_src[arith_pos + 1] == '<')
		{
			arith_pos = arith_pos + 2;
			left = left << arith_additive();
		}
		else if(arith_src[arith_pos] == '>' && arith_src[arith_pos + 1] == '>')
		{
			arith_pos = arith_pos + 2;
			left = left >> arith_additive();
		}
		else
		{
			break;
		}
	}
	return left;
}

int arith_relational()
{
	int left;
	left = arith_shift();
	while(TRUE)
	{
		arith_skip_space();
		if(arith_src[arith_pos] == '<' && arith_src[arith_pos + 1] == '=')
		{
			arith_pos = arith_pos + 2;
			left = bool_to_int(left <= arith_shift());
		}
		else if(arith_src[arith_pos] == '>' && arith_src[arith_pos + 1] == '=')
		{
			arith_pos = arith_pos + 2;
			left = bool_to_int(left >= arith_shift());
		}
		else if(arith_src[arith_pos] == '<' && arith_src[arith_pos + 1] != '<')
		{
			arith_pos = arith_pos + 1;
			left = bool_to_int(left < arith_shift());
		}
		else if(arith_src[arith_pos] == '>' && arith_src[arith_pos + 1] != '>')
		{
			arith_pos = arith_pos + 1;
			left = bool_to_int(left > arith_shift());
		}
		else
		{
			break;
		}
	}
	return left;
}

int arith_equality()
{
	int left;
	left = arith_relational();
	while(TRUE)
	{
		arith_skip_space();
		if(arith_src[arith_pos] == '=' && arith_src[arith_pos + 1] == '=')
		{
			arith_pos = arith_pos + 2;
			left = bool_to_int(left == arith_relational());
		}
		else if(arith_src[arith_pos] == '!' && arith_src[arith_pos + 1] == '=')
		{
			arith_pos = arith_pos + 2;
			left = bool_to_int(left != arith_relational());
		}
		else
		{
			break;
		}
	}
	return left;
}

int arith_bitand()
{
	int left;
	left = arith_equality();
	while(TRUE)
	{
		arith_skip_space();
		if(arith_src[arith_pos] == '&' && arith_src[arith_pos + 1] != '&')
		{
			arith_pos = arith_pos + 1;
			left = left & arith_equality();
		}
		else break;
	}
	return left;
}

int arith_bitxor()
{
	int left;
	left = arith_bitand();
	while(TRUE)
	{
		arith_skip_space();
		if(arith_src[arith_pos] == '^')
		{
			arith_pos = arith_pos + 1;
			left = left ^ arith_bitand();
		}
		else break;
	}
	return left;
}

int arith_bitor()
{
	int left;
	left = arith_bitxor();
	while(TRUE)
	{
		arith_skip_space();
		if(arith_src[arith_pos] == '|' && arith_src[arith_pos + 1] != '|')
		{
			arith_pos = arith_pos + 1;
			left = left | arith_bitxor();
		}
		else break;
	}
	return left;
}

int arith_logand()
{
	int left;
	left = arith_bitor();
	while(TRUE)
	{
		arith_skip_space();
		if(arith_src[arith_pos] == '&' && arith_src[arith_pos + 1] == '&')
		{
			int r;
			arith_pos = arith_pos + 2;
			/* Force the right operand to be parsed (it advances arith_pos):
			   on gcc/tcc a bare `left && arith_bitor()` would short-circuit
			   and never consume the right side, desyncing the parser. M2 never
			   short-circuits, so evaluating into a temp is correct everywhere. */
			r = arith_bitor();
			left = bool_to_int(left && r);
		}
		else break;
	}
	return left;
}

int arith_logor()
{
	int left;
	left = arith_logand();
	while(TRUE)
	{
		arith_skip_space();
		if(arith_src[arith_pos] == '|' && arith_src[arith_pos + 1] == '|')
		{
			int r;
			arith_pos = arith_pos + 2;
			/* Same as &&: force the right operand to parse so arith_pos
			   advances on gcc/tcc (which short-circuit ||); M2 does not. */
			r = arith_logand();
			left = bool_to_int(left || r);
		}
		else break;
	}
	return left;
}

int arith_ternary()
{
	int cond;
	int a;
	int b;
	cond = arith_logor();
	arith_skip_space();
	if(arith_pos < arith_len && arith_src[arith_pos] == '?')
	{
		arith_pos = arith_pos + 1;
		/* Only the taken branch is evaluated; the other is parsed with
		   side effects suppressed so e.g. `cond ? x : 1/0` cannot raise a
		   spurious divide-by-zero (both branches are still scanned because
		   the parser is single-pass). */
		if(!cond) arith_noeval = arith_noeval + 1;
		a = arith_expr();
		if(!cond) arith_noeval = arith_noeval - 1;
		arith_skip_space();
		if(arith_pos < arith_len && arith_src[arith_pos] == ':')
		{
			arith_pos = arith_pos + 1;
		}
		if(cond) arith_noeval = arith_noeval + 1;
		b = arith_expr();
		if(cond) arith_noeval = arith_noeval - 1;
		if(cond) return a;
		return b;
	}
	return cond;
}

int arith_expr()
{
	return arith_ternary();
}

int sh_arith(char* expr)
{
	arith_src = expr;
	arith_pos = 0;
	arith_len = strlen(expr);
	arith_noeval = 0;
	return arith_expr();
}

/* Expansion engine. */

/* Copy `val` into `out` at `oi`, inserting Q_SPLIT for IFS chars
 * when not inside quotes. Returns new oi. */
int ifs_copy(char** out, int oi, int* outsize, char* val, int in_dq)
{
	int vi;
	int c;
	char* ifs;
	int is_ifs;
	int ji;

	if(in_dq || !g_field_split)
	{
		/* No field splitting inside double quotes or in single-word
		   contexts (assignment RHS, redirect target, case subject). */
		return out_puts(out, oi, outsize, val);
	}

	ifs = cached_ifs;
	vi = 0;
	while(val[vi] != 0)
	{
		c = val[vi];
		is_ifs = FALSE;
		ji = 0;
		while(ifs[ji] != 0)
		{
			if(c == ifs[ji]) { is_ifs = TRUE; break; }
			ji = ji + 1;
		}
		if(is_ifs)
		{
			oi = out_put(out, oi, outsize, Q_SPLIT);
			/* Coalesce consecutive IFS chars into one split. */
			while(val[vi] != 0)
			{
				c = val[vi];
				is_ifs = FALSE;
				ji = 0;
				while(ifs[ji] != 0)
				{
					if(c == ifs[ji]) { is_ifs = TRUE; break; }
					ji = ji + 1;
				}
				if(!is_ifs) break;
				vi = vi + 1;
			}
		}
		else
		{
			oi = out_put(out, oi, outsize, c);
			vi = vi + 1;
		}
	}
	return oi;
}

/* Read from `w` at `*pos` until the matching closing `}` at depth 0,
 * copying into `pat`. Returns length written and advances `*pos` past `}`. */
int extract_brace_pattern(char* w, int* pos, char* pat, int patsize)
{
	int pi;
	int pd;
	int in_br;       /* inside a [...] bracket expression */
	pi = 0;
	pd = 1;
	in_br = FALSE;
	while(w[*pos] != 0)
	{
		/* A `\`-escaped char (e.g. `\}`, `\{`) is literal and must not
		   change brace depth -- copy both bytes verbatim. Likewise a `}`
		   or `{` inside a [...] bracket expression is an ordinary pattern
		   char. Without this, `${v%[\)\}]}` closes at the escaped `}` and
		   `${v#\$[\(\{]}` over-counts on the escaped `{`. */
		if(w[*pos] == '\\' && w[*pos + 1] != 0)
		{
			require(pi + 2 < patsize, "hs: brace pattern too long\n");
			pat[pi] = w[*pos];
			pat[pi + 1] = w[*pos + 1];
			pi = pi + 2;
			*pos = *pos + 2;
			continue;
		}
		if(in_br)
		{
			if(w[*pos] == ']') in_br = FALSE;
		}
		else if(w[*pos] == '[') in_br = TRUE;
		else if(w[*pos] == '{') pd = pd + 1;
		else if(w[*pos] == '}') { pd = pd - 1; if(pd == 0) break; }
		require(pi + 1 < patsize, "hs: brace pattern too long\n");
		pat[pi] = w[*pos];
		pi = pi + 1;
		*pos = *pos + 1;
	}
	pat[pi] = 0;
	if(w[*pos] == '}') *pos = *pos + 1;
	return pi;
}

/* Strip Q_*_OPEN/CLOSE sentinels from a string. */
char* strip_quotes(char* s)
{
	char* out;
	int oi;
	int i;
	int c;

	if(s == NULL) return "";
	out = tmp_alloc(strlen(s) + 1);
	oi = 0;
	i = 0;
	while(s[i] != 0)
	{
		c = s[i];
		if(c == Q_SQ_OPEN || c == Q_SQ_CLOSE || c == Q_DQ_OPEN || c == Q_DQ_CLOSE || c == Q_ATNULL)
		{
			i = i + 1;
		}
		else
		{
			out[oi] = c;
			oi = oi + 1;
			i = i + 1;
		}
	}
	out[oi] = 0;
	return out;
}

/* Apply a ${VAR#}/${VAR##}/${VAR%}/${VAR%%} affix removal: strip the matching
   prefix (is_suffix=FALSE) or suffix (TRUE), shortest (longest=FALSE) or longest
   (TRUE) match of glob `pat` from `val`. The split point k is the number of
   leading chars removed (prefix) or kept (suffix); the iteration direction
   selects shortest vs longest. Returns a suffix pointer into `val` for a prefix
   removal, a fresh arena copy for a suffix removal, or `val` unchanged if
   nothing matches. */
char* strip_affix(char* val, char* pat, int is_suffix, int longest)
{
	int vlen;
	int k;
	int start;
	int end;
	int dir;
	char* sub;

	vlen = strlen(val);
	if(is_suffix)
	{
		/* suffix: match val[k..]; shortest = largest k first, longest = smallest. */
		if(longest) { start = 0; end = vlen; dir = 1; }
		else { start = vlen; end = 0; dir = -1; }
	}
	else
	{
		/* prefix: match val[0..k); shortest = smallest k first, longest = largest. */
		if(longest) { start = vlen; end = 0; dir = -1; }
		else { start = 0; end = vlen; dir = 1; }
	}

	sub = tmp_alloc(vlen + 1);
	k = start;
	while(TRUE)
	{
		if(is_suffix)
		{
			if(pat_match(pat, val + k))
			{
				memcpy(sub, val, k);
				sub[k] = 0;
				return sub;
			}
		}
		else
		{
			memcpy(sub, val, k);
			sub[k] = 0;
			if(pat_match(pat, sub)) return val + k;
		}
		if(k == end) break;
		k = k + dir;
	}
	return val;
}

/* Fully expand a word (variable, command, arithmetic). The returned
 * string may contain Q_SPLIT markers driving word splitting. */
int expand_depth;

char* expand_word(char* w)
{
	char* out;
	int outsize;
	int oi;
	int i;
	int c;
	int in_sq;
	int in_dq;
	int depth;
	char* name;
	int ni;
	char* val;
	char* sub_expr;
	int si;
	char* pat;
	int j;
	int k;
	int vlen;
	int wlen;
	char* result;

	if(w == NULL) return "";

	expand_depth = expand_depth + 1;
	require(expand_depth < MAX_DEPTH, "hs: expansion nested too deep\n");

	wlen = strlen(w);
	/* Heap scratch that grows on demand (out_put/out_puts realloc it);
	   copied into the arena at the end. Start from an input-based guess. */
	outsize = wlen * 4 + 64;
	if(outsize < 512) outsize = 512;
	out = calloc(outsize, 1);
	require(out != NULL, "hs: out of memory in expansion\n");
	oi = 0;
	i = 0;
	in_sq = FALSE;
	in_dq = FALSE;

	while(w[i] != 0)
	{
		c = w[i];

		/* Forward sentinels through so downstream consumers (pat_match
		 * for case patterns) can distinguish quoted from unquoted
		 * glob chars. strip_quotes removes them when not needed. */
		if(c == Q_SQ_OPEN && !in_dq)
		{
			in_sq = TRUE;
			oi = out_put(&out, oi, &outsize, c);
			i = i + 1;
		}
		else if(c == Q_SQ_CLOSE && in_sq)
		{
			in_sq = FALSE;
			oi = out_put(&out, oi, &outsize, c);
			i = i + 1;
		}
		else if(c == Q_DQ_OPEN && !in_sq)
		{
			in_dq = TRUE;
			oi = out_put(&out, oi, &outsize, c);
			i = i + 1;
		}
		else if(c == Q_DQ_CLOSE && in_dq)
		{
			in_dq = FALSE;
			oi = out_put(&out, oi, &outsize, c);
			i = i + 1;
		}
		else if(in_sq)
		{
			/* No expansion inside single quotes. */
			oi = out_put(&out, oi, &outsize, c);
			i = i + 1;
		}
		else if(c == '$')
		{
			i = i + 1;
			if(w[i] == 0)
			{
				oi = out_put(&out, oi, &outsize, '$');
			}
			else if(w[i] == '(' && w[i+1] == '(')
			{
				i = i + 2;
				sub_expr = tmp_alloc(MAX_STR);
				si = 0;
				depth = 2;
				while(w[i] != 0 && depth > 0)
				{
					if(w[i] == '(' ) depth = depth + 1;
					else if(w[i] == ')') depth = depth - 1;
					if(depth > 0)
					{
						require(si + 1 < MAX_STR, "hs: arith expression too long\n");
						sub_expr[si] = w[i];
						si = si + 1;
					}
					i = i + 1;
				}
				sub_expr[si] = 0;
				/* Expand variables before evaluating. */
				val = expand_word(sub_expr);
				val = strip_quotes(val);
				val = int_to_str(sh_arith(val));
				oi = out_puts(&out, oi, &outsize, val);
			}
			else if(w[i] == '(')
			{
				i = i + 1;
				sub_expr = tmp_alloc(MAX_STR);
				si = 0;
				depth = 1;
				{
					int cs_sq;
					int cs_dq;
					cs_sq = FALSE;
					cs_dq = FALSE;
					while(w[i] != 0 && depth > 0)
					{
						/* Don't count parens inside quotes: a `)` in an
						   embedded sed script like 's/\(x\))/.../' must not
						   close the substitution early. */
						if(cs_sq) { if(w[i] == '\'') cs_sq = FALSE; }
						else if(cs_dq) { if(w[i] == '"') cs_dq = FALSE; }
						else if(w[i] == '\'') cs_sq = TRUE;
						else if(w[i] == '"') cs_dq = TRUE;
						else if(w[i] == '(') depth = depth + 1;
						else if(w[i] == ')') depth = depth - 1;
						if(depth > 0)
						{
							require(si + 1 < MAX_STR, "hs: command substitution too long\n");
							sub_expr[si] = w[i];
							si = si + 1;
						}
						i = i + 1;
					}
				}
				sub_expr[si] = 0;

				/* Capture the substitution into a tmp file via fork+close+open,
				 * then splice its contents into the output. */
				{
					char* cs_tmpfile;
					int cs_fd;
					int cs_rd;
					int cs_len;
					int cs_cap;
					char* cs_buf;

					cs_tmpfile = capture_expr_to_tmpfile(sub_expr);
					cs_cap = MAX_STR;
					cs_buf = calloc(cs_cap, 1);
					cs_len = 0;
					cs_fd = open(cs_tmpfile, O_RDONLY, 0);
					if(cs_fd >= 0)
					{
						while(TRUE)
						{
							/* Grow on demand so large $(...) output (e.g. a
							   file list from `find`) is not silently truncated
							   at MAX_STR. Keep one byte for the NUL terminator. */
							if(cs_len >= cs_cap - 1)
							{
								char* cs_nb;
								cs_cap = cs_cap * 2;
								cs_nb = calloc(cs_cap, 1);
								memcpy(cs_nb, cs_buf, cs_len);
								free(cs_buf);
								cs_buf = cs_nb;
							}
							cs_rd = read(cs_fd, cs_buf + cs_len, cs_cap - 1 - cs_len);
							if(cs_rd <= 0) break;
							cs_len = cs_len + cs_rd;
						}
						close(cs_fd);
					}
					unlink(cs_tmpfile);
					free(cs_tmpfile);

					while(cs_len > 0 && cs_buf[cs_len - 1] == '\n')
					{
						cs_len = cs_len - 1;
					}
					cs_buf[cs_len] = 0;

					/* Unquoted command substitution is subject to field
					   splitting on IFS (ifs_copy inserts Q_SPLIT markers);
					   inside double quotes it is spliced literally. This is
					   what makes `for dep in $(cat deps)` iterate per line. */
					oi = ifs_copy(&out, oi, &outsize, cs_buf, in_dq);
					free(cs_buf);
				}
			}
			else if(w[i] == '{')
			{
				/* ${...} braced expansion. */
				i = i + 1;
				if(w[i] == '#')
				{
					i = i + 1;
					name = tmp_alloc(256);
					ni = 0;
					while(w[i] != 0 && w[i] != '}')
					{
						require(ni + 1 < 256, "hs: variable name too long\n");
						name[ni] = w[i];
						ni = ni + 1;
						i = i + 1;
					}
					name[ni] = 0;
					if(w[i] == '}') i = i + 1;
					val = var_get_safe(name);
					val = int_to_str(strlen(val));
					oi = out_puts(&out, oi, &outsize, val);
				}
				else
				{
					name = tmp_alloc(256);
					ni = 0;
					while(w[i] != 0 && w[i] != '}' && w[i] != '#' && w[i] != '%' && w[i] != ':' && w[i] != '+')
					{
						/* `-`, `=`, `?` only stop the name after the first
						 * char, so ${VAR-d}/${VAR=d}/${VAR?d} parse while the
						 * special params ${-}/${?} are still read as names. */
						if(w[i] == '-' && ni > 0) break;
						if(w[i] == '=' && ni > 0) break;
						if(w[i] == '?' && ni > 0) break;
						require(ni + 1 < 256, "hs: variable name too long\n");
						name[ni] = w[i];
						ni = ni + 1;
						i = i + 1;
					}
					name[ni] = 0;

					if(w[i] == '}')
					{
						/* ${VAR} */
						i = i + 1;
						val = var_get(name);
						oi = ifs_copy(&out, oi, &outsize, val, in_dq);
					}
					else if(w[i] == ':' && w[i+1] == '-')
					{
						/* ${VAR:-default} */
						i = i + 2;
						sub_expr = tmp_alloc(MAX_STR);
						extract_brace_pattern(w, &i, sub_expr, MAX_STR);
						val = var_get_safe(name);
						if(val[0] == 0)
						{
							val = expand_word(sub_expr);
							val = strip_quotes(val);
						}
						oi = out_puts(&out, oi, &outsize, val);
					}
					else if(w[i] == '-')
					{
						/* ${VAR-default}: only used if unset. */
						i = i + 1;
						sub_expr = tmp_alloc(MAX_STR);
						extract_brace_pattern(w, &i, sub_expr, MAX_STR);
						if(!param_is_set(name))
						{
							val = expand_word(sub_expr);
							val = strip_quotes(val);
						}
						else
						{
							val = var_get(name);
						}
						oi = out_puts(&out, oi, &outsize, val);
						/* sub_expr from tmp arena */
					}
					else if(w[i] == ':' && w[i+1] == '+')
					{
						/* ${VAR:+alternate} */
						i = i + 2;
						sub_expr = tmp_alloc(MAX_STR);
						extract_brace_pattern(w, &i, sub_expr, MAX_STR);
						val = var_get_safe(name);
						if(val[0] != 0)
						{
							val = expand_word(sub_expr);
							val = strip_quotes(val);
						}
						else
						{
							val = "";
						}
						oi = out_puts(&out, oi, &outsize, val);
						/* sub_expr from tmp arena */
					}
					else if(w[i] == '+')
					{
						/* ${VAR+alt}: alt if VAR is set, empty otherwise. */
						i = i + 1;
						sub_expr = tmp_alloc(MAX_STR);
						extract_brace_pattern(w, &i, sub_expr, MAX_STR);
						if(param_is_set(name))
						{
							val = expand_word(sub_expr);
							val = strip_quotes(val);
						}
						else
						{
							val = "";
						}
						oi = out_puts(&out, oi, &outsize, val);
						/* sub_expr from tmp arena */
					}
					else if(w[i] == ':' && w[i+1] == '=')
					{
						/* ${VAR:=word}: if VAR unset or empty, assign word
						 * to VAR and substitute it. */
						i = i + 2;
						sub_expr = tmp_alloc(MAX_STR);
						extract_brace_pattern(w, &i, sub_expr, MAX_STR);
						val = var_get_safe(name);
						if(val[0] == 0)
						{
							val = strip_quotes(expand_word(sub_expr));
							var_set(name, val);
						}
						oi = out_puts(&out, oi, &outsize, val);
					}
					else if(w[i] == '=')
					{
						/* ${VAR=word}: if VAR unset, assign word and use it;
						 * otherwise use VAR's value. */
						i = i + 1;
						sub_expr = tmp_alloc(MAX_STR);
						extract_brace_pattern(w, &i, sub_expr, MAX_STR);
						if(!param_is_set(name))
						{
							val = strip_quotes(expand_word(sub_expr));
							var_set(name, val);
						}
						else
						{
							val = var_get(name);
						}
						oi = out_puts(&out, oi, &outsize, val);
					}
					else if(w[i] == ':' && w[i+1] == '?')
					{
						/* ${VAR:?word}: error+exit if VAR unset or empty. */
						i = i + 2;
						sub_expr = tmp_alloc(MAX_STR);
						extract_brace_pattern(w, &i, sub_expr, MAX_STR);
						val = var_get_safe(name);
						if(val[0] == 0)
						{
							sh_err_puts(name);
							sh_err_puts(": ");
							val = strip_quotes(expand_word(sub_expr));
							if(val[0] != 0) sh_err_puts(val);
							else sh_err_puts("parameter not set or null");
							sh_err_puts("\n");
							sh_flush(stdout);
							exit(1);
						}
						oi = out_puts(&out, oi, &outsize, val);
					}
					else if(w[i] == '?')
					{
						/* ${VAR?word}: error+exit if VAR is unset. */
						i = i + 1;
						sub_expr = tmp_alloc(MAX_STR);
						extract_brace_pattern(w, &i, sub_expr, MAX_STR);
						if(!param_is_set(name))
						{
							sh_err_puts(name);
							sh_err_puts(": ");
							val = strip_quotes(expand_word(sub_expr));
							if(val[0] != 0) sh_err_puts(val);
							else sh_err_puts("parameter not set");
							sh_err_puts("\n");
							sh_flush(stdout);
							exit(1);
						}
						val = var_get(name);
						oi = out_puts(&out, oi, &outsize, val);
					}
					else if(w[i] == '#' && w[i+1] == '#')
					{
						/* ${VAR##pat}: longest matching prefix removal. */
						i = i + 2;
						pat = tmp_alloc(MAX_STR);
						extract_brace_pattern(w, &i, pat, MAX_STR);
						val = var_get_safe(name);
						pat = strip_quotes(expand_word(pat));
						val = strip_affix(val, pat, FALSE, TRUE);
						oi = out_puts(&out, oi, &outsize, val);
					}
					else if(w[i] == '#')
					{
						/* ${VAR#pat}: shortest matching prefix removal. */
						i = i + 1;
						pat = tmp_alloc(MAX_STR);
						extract_brace_pattern(w, &i, pat, MAX_STR);
						val = var_get_safe(name);
						pat = strip_quotes(expand_word(pat));
						val = strip_affix(val, pat, FALSE, FALSE);
						oi = out_puts(&out, oi, &outsize, val);
					}
					else if(w[i] == '%' && w[i+1] == '%')
					{
						/* ${VAR%%pat}: longest matching suffix removal. */
						i = i + 2;
						pat = tmp_alloc(MAX_STR);
						extract_brace_pattern(w, &i, pat, MAX_STR);
						val = var_get_safe(name);
						pat = strip_quotes(expand_word(pat));
						val = strip_affix(val, pat, TRUE, TRUE);
						oi = out_puts(&out, oi, &outsize, val);
					}
					else if(w[i] == '%')
					{
						/* ${VAR%pat}: shortest matching suffix removal. */
						i = i + 1;
						pat = tmp_alloc(MAX_STR);
						extract_brace_pattern(w, &i, pat, MAX_STR);
						val = var_get_safe(name);
						pat = strip_quotes(expand_word(pat));
						val = strip_affix(val, pat, TRUE, FALSE);
						oi = out_puts(&out, oi, &outsize, val);
					}
					else
					{
						/* Unknown operator: output ${...} verbatim. */
						oi = out_put(&out, oi, &outsize, '$');
						oi = out_put(&out, oi, &outsize, '{');
						oi = out_puts(&out, oi, &outsize, name);
						{
							int ud;
							ud = 1;
							while(w[i] != 0 && ud > 0)
							{
								if(w[i] == '{') ud = ud + 1;
								else if(w[i] == '}') { ud = ud - 1; if(ud == 0) break; }
								oi = out_put(&out, oi, &outsize, w[i]);
								i = i + 1;
							}
						}
						if(w[i] == '}')
						{
							oi = out_put(&out, oi, &outsize, '}');
							i = i + 1;
						}
					}
				}
			}
			else if(w[i] == '@' || w[i] == '*')
			{
				j = 1;
				while(j < pp_argc)
				{
					if(j > 1)
					{
						if(in_dq && w[i] == '*')
						{
							/* "$*" joins with first char of IFS. */
							if(cached_ifs[0] != 0)
							{
								oi = out_put(&out, oi, &outsize, cached_ifs[0]);
							}
						}
						else
						{
							oi = out_put(&out, oi, &outsize, Q_SPLIT);
						}
					}
					val = pp_argv[j];
					oi = out_puts(&out, oi, &outsize, val);
					j = j + 1;
				}
				/* No positional params: mark a null so an otherwise-empty
				   word ("$@" alone) drops instead of becoming one "". */
				if(pp_argc <= 1) oi = out_put(&out, oi, &outsize, Q_ATNULL);
				i = i + 1;
			}
			else
			{
				/* $VAR (also $0-$9, $?, $#, etc.). */
				name = tmp_alloc(256);
				ni = 0;
				while(w[i] != 0)
				{
					c = w[i];
					if((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')
					{
						require(ni + 1 < 256, "hs: variable name too long\n");
						name[ni] = c;
						ni = ni + 1;
						i = i + 1;
					}
					else if(ni == 0 && (c == '?' || c == '#' || c == '@' || c == '*' || c == '!' || c == '-' || c == '$'))
					{
						name[ni] = c;
						ni = ni + 1;
						i = i + 1;
						break;
					}
					else
					{
						break;
					}
				}
				name[ni] = 0;
				if(ni == 0)
				{
					oi = out_put(&out, oi, &outsize, '$');
				}
				else
				{
					val = var_get(name);
					oi = ifs_copy(&out, oi, &outsize, val, in_dq);
				}
			}
		}
		else
		{
			oi = out_put(&out, oi, &outsize, c);
			i = i + 1;
		}
	}
	/* Copy the grown heap scratch into the arena (callers expect an
	   arena-owned string that survives until the next tmp_reset). */
	result = tmp_alloc(oi + 1);
	memcpy(result, out, oi);
	result[oi] = 0;
	free(out);
	expand_depth = expand_depth - 1;
	return result;
}

/* Strip-quote one split field and append it to out[oi], applying the
   "$@"/"$*"-with-no-params empty-field drop and the MAX_ARGV bound; returns the
   next index. Shared by the two field-splitting sites (command words in
   expand_argv and the `for`-list below). There is no filesystem globbing -- hs
   has no getdents -- so an unquoted *, ?, [ stays literal, as if `set -f`. */
int emit_field(char** out, int oi, char* stripped)
{
	char* fs;
	int had_atnull;
	int m;
	had_atnull = FALSE;
	m = 0;
	while(stripped[m] != 0)
	{
		if(stripped[m] == Q_ATNULL) had_atnull = TRUE;
		m = m + 1;
	}
	fs = strip_quotes(stripped);
	/* Drop an empty "$@"/"$*"-with-no-params field (yields no word, not "")
	   so `for x in "$@"` over no args iterates zero times. */
	if(fs[0] != 0 || !had_atnull)
	{
		if(oi < MAX_ARGV - 1)
		{
			out[oi] = fs;
			oi = oi + 1;
		}
	}
	return oi;
}

/* Expand each word and split on Q_SPLIT markers from unquoted expansions. */
char** expand_argv(char** words, int wc, int* out_argc)
{
	char** result;
	int ri;
	int i;
	char* expanded;
	char* stripped;
	int j;
	int start;
	int c;

	result = tmp_alloc(MAX_ARGV * sizeof(char*));
	ri = 0;

	i = 0;
	while(i < wc)
	{
		expanded = expand_word(words[i]);

		j = 0;
		start = 0;
		while(TRUE)
		{
			c = expanded[j];
			if(c == Q_SPLIT || c == 0)
			{
				if(j > start)
				{
					stripped = tmp_alloc(j - start + 1);
					memcpy(stripped, expanded + start, j - start);
					stripped[j - start] = 0;
					ri = emit_field(result, ri, stripped);
				}
				if(c == 0) break;
				start = j + 1;
				j = j + 1;
			}
			else
			{
				j = j + 1;
			}
		}
		i = i + 1;
	}
	result[ri] = NULL;
	*out_argc = ri;
	return result;
}

/* Builtins. */
int builtin_echo(char** argv, int argc)
{
	int i;
	int no_nl;
	int start;

	no_nl = FALSE;
	start = 1;

	if(argc > 1 && match(argv[1], "-n"))
	{
		no_nl = TRUE;
		start = 2;
	}

	i = start;
	while(i < argc)
	{
		if(i > start) sh_puts(" ", stdout);
		sh_puts(argv[i], stdout);
		i = i + 1;
	}
	if(!no_nl) sh_puts("\n", stdout);
	sh_flush(stdout);
	return 0;
}

int builtin_printf(char** argv, int argc)
{
	char* fmt;
	int fi;
	int ai;
	int c;
	char* s;
	int n;
	int oct_val;
	int oct_cnt;

	if(argc < 2) return 0;
	fmt = argv[1];
	ai = 2;

	/* Reapply the format until all operands are consumed (POSIX: `printf
	   '%s\n' a b c` prints three lines). The inner pass runs once even
	   with no operands; the outer loop repeats only while a pass consumed
	   at least one, to avoid spinning on a format with no conversions. */
	while(TRUE)
	{
	int pf_start_ai;
	pf_start_ai = ai;
	fi = 0;

	while(fmt[fi] != 0)
	{
		c = fmt[fi];
		if(c == '\\')
		{
			fi = fi + 1;
			c = fmt[fi];
			if(c == 'n') { sh_putc('\n', stdout); fi = fi + 1; }
			else if(c == 't') { sh_putc('\t', stdout); fi = fi + 1; }
			else if(c == 'r') { sh_putc('\r', stdout); fi = fi + 1; }
			else if(c == '\\') { sh_putc('\\', stdout); fi = fi + 1; }
			else if(c == '0' || (c >= '1' && c <= '3'))
			{
				/* Octal escape: \NNN or \0NNN. */
				oct_val = 0;
				oct_cnt = 0;
				if(c == '0')
				{
					fi = fi + 1;
					c = fmt[fi];
				}
				while(oct_cnt < 3 && c >= '0' && c <= '7')
				{
					oct_val = oct_val * 8 + (c - '0');
					fi = fi + 1;
					c = fmt[fi];
					oct_cnt = oct_cnt + 1;
				}
				sh_putc(oct_val, stdout);
			}
			else
			{
				sh_putc('\\', stdout);
				sh_putc(c, stdout);
				fi = fi + 1;
			}
		}
		else if(c == '%')
		{
			int pf_left;
			int pf_zero;
			int pf_width;
			int pf_prec;
			int pf_start;
			char* pf_val;
			int pf_len;
			int pf_pad;

			pf_start = fi; /* so an unrecognized spec can echo verbatim */
			fi = fi + 1;

			/* Flags. */
			pf_left = FALSE;
			pf_zero = FALSE;
			while(fmt[fi] == '-' || fmt[fi] == '0' || fmt[fi] == '+' || fmt[fi] == ' ' || fmt[fi] == '#')
			{
				if(fmt[fi] == '-') pf_left = TRUE;
				else if(fmt[fi] == '0') pf_zero = TRUE;
				fi = fi + 1;
			}
			/* Width. */
			pf_width = 0;
			while(fmt[fi] >= '0' && fmt[fi] <= '9')
			{
				pf_width = pf_width * 10 + (fmt[fi] - '0');
				fi = fi + 1;
			}
			/* Precision. */
			pf_prec = -1;
			if(fmt[fi] == '.')
			{
				fi = fi + 1;
				pf_prec = 0;
				while(fmt[fi] >= '0' && fmt[fi] <= '9')
				{
					pf_prec = pf_prec * 10 + (fmt[fi] - '0');
					fi = fi + 1;
				}
			}

			c = fmt[fi];
			pf_val = NULL;
			if(c == 's')
			{
				if(ai < argc) pf_val = argv[ai];
				else pf_val = "";
				if(ai < argc) ai = ai + 1;
			}
			else if(c == 'd' || c == 'i')
			{
				if(ai < argc) pf_val = int_to_str(str_to_int(argv[ai]));
				else pf_val = "0";
				if(ai < argc) ai = ai + 1;
			}
			else if(c == 'x' || c == 'X')
			{
				char* hex;
				int hi;
				int hv;
				if(ai < argc) hv = str_to_int(argv[ai]);
				else hv = 0;
				hex = int2str(hv, 16, FALSE);
				if(c == 'x')
				{
					hi = 0;
					while(hex[hi] != 0)
					{
						if(hex[hi] >= 'A' && hex[hi] <= 'F') hex[hi] = hex[hi] + 32;
						hi = hi + 1;
					}
				}
				pf_val = hex;
				if(ai < argc) ai = ai + 1;
			}
			else if(c == 'c')
			{
				pf_val = tmp_alloc(2);
				pf_val[0] = 0;
				/* Nested ifs, not `ai < argc && argv[ai][0]`: M2-Planet does
				   not short-circuit, so the && form would deref argv[argc]
				   (NULL) when fewer args than %c specs are given. */
				if(ai < argc)
				{
					if(argv[ai][0] != 0) pf_val[0] = argv[ai][0];
				}
				pf_val[1] = 0;
				if(ai < argc) ai = ai + 1;
			}
			else if(c == '%')
			{
				pf_val = "%";
			}

			if(pf_val == NULL)
			{
				/* Unrecognized conversion: echo the spec verbatim. */
				while(pf_start <= fi && fmt[pf_start] != 0)
				{
					sh_putc(fmt[pf_start], stdout);
					pf_start = pf_start + 1;
				}
				if(fmt[fi] != 0) fi = fi + 1;
			}
			else
			{
				/* Precision truncates strings; width pads either side. */
				pf_len = strlen(pf_val);
				if((c == 's') && pf_prec >= 0 && pf_len > pf_prec) pf_len = pf_prec;
				pf_pad = pf_width - pf_len;
				if(!pf_left)
				{
					int pf_padc;
					if(pf_zero && c != 's') pf_padc = '0';
					else pf_padc = ' ';
					while(pf_pad > 0)
					{
						sh_putc(pf_padc, stdout);
						pf_pad = pf_pad - 1;
					}
				}
				{
					int pi;
					pi = 0;
					while(pi < pf_len) { sh_putc(pf_val[pi], stdout); pi = pi + 1; }
				}
				if(pf_left)
				{
					while(pf_pad > 0) { sh_putc(' ', stdout); pf_pad = pf_pad - 1; }
				}
				fi = fi + 1;
			}
		}
		else
		{
			sh_putc(c, stdout);
			fi = fi + 1;
		}
	}

	if(ai >= argc) break;       /* all operands consumed */
	if(ai == pf_start_ai) break; /* format had no arg-consuming conversion */
	}
	sh_flush(stdout);
	return 0;
}

/* `test -f`/`-d`/`-s` need the file *type*, which access() can't give. hs has
   no stat syscall -- and deliberately no inline asm -- so all three share one
   portable probe: open the path O_RDONLY|O_NONBLOCK and try to read one byte.
   The single open feeds every predicate (no double-open for `-s`), and the
   1-byte read lands in g_ftbuf (a shared scratch byte) to avoid taking the
   address of a local under M2-Planet.

   path_probe returns the read() result: >= 1 = a readable byte is present,
   0 = opened but no byte (EOF), < 0 = opened but read failed. PROBE_NOOPEN
   means open() itself failed (a value no read() return can collide with).

   TYPE RULE (size-as-type): >= 1 byte readable -> regular file; opened but no
   readable byte -> directory. The "no byte" case unifies two kernels: a real
   POSIX kernel returns EISDIR (< 0) when you read a directory, while the
   builder-hex0-arch bootstrap kernels (see KERNEL.md, finding K2) have no stat,
   represent a directory as a size-0 entry, and return EOF (== 0) for it -- the
   same signature as an empty regular file there. Treating BOTH "read < 0" and
   "read == 0" as a directory makes hs classify directories identically on both,
   at the documented cost below.

   Limits vs a real stat (accepted -- they need stat/lstat we don't have):
   - an EMPTY regular file (0 bytes) is reported as a directory (`-d` true, `-f`
     false) on every kernel. This is deliberate: on a size-0-directory kernel an
     empty file and a directory are byte-for-byte indistinguishable without
     stat, so hs picks one rule and applies it everywhere for consistency. The
     bootstrap convention is "no zero-byte regular files" (directories are made
     with `src 0 /name`), which makes this exact on those kernels;
   - a file with no read permission can't be opened, so `-f`/`-d` report false
     even though it exists (POSIX uses the inode type regardless of read perm);
   - a FIFO/socket with no data ready reads -1 (EAGAIN), like a directory;
   - `-L`/`-h` (symlink) need lstat, so they are always false. */
#define PROBE_NOOPEN (-1000)
int path_probe(char* p)
{
	int fd;
	int r;
	fd = open(p, HS_O_NONBLOCK, 0);   /* O_RDONLY (0) | O_NONBLOCK */
	if(fd < 0) return PROBE_NOOPEN;
	r = read(fd, g_ftbuf, 1);
	close(fd);
	return r;
}

int path_is_reg(char* p)
{
	if(path_probe(p) >= 1) return TRUE;   /* opened with >= 1 readable byte -> regular file */
	return FALSE;
}

int path_is_dir(char* p)
{
	int r;
	r = path_probe(p);
	if(r == PROBE_NOOPEN) return FALSE;   /* couldn't open -> not a directory */
	if(r >= 1) return FALSE;              /* has a readable byte -> regular file */
	return TRUE;                          /* opened, no byte (EISDIR < 0, or EOF == 0 on a
	                                         size-0-directory kernel) -> directory */
}

/* `-s`: a readable file with at least one byte (read() returns 1 when a byte is
   present, 0 when empty). */
int path_nonempty(char* p)
{
	if(path_probe(p) >= 1) return TRUE;
	return FALSE;
}

/* Evaluate a binary test expression `a OP b` (the string and numeric
   comparison operators). Returns the 0/1 exit status, or -1 if OP is not one of
   them. Shared by the `a OP b` (argc==4) and `! a OP b` (argc==5) dispatch. */
int test_binop(char* a, char* op, char* b)
{
	if(match(op, "=")) return bool_to_status(match(a, b));
	if(match(op, "!=")) return bool_to_status(!match(a, b));
	if(match(op, "-eq")) return bool_to_status(str_to_int(a) == str_to_int(b));
	if(match(op, "-ne")) return bool_to_status(str_to_int(a) != str_to_int(b));
	if(match(op, "-lt")) return bool_to_status(str_to_int(a) < str_to_int(b));
	if(match(op, "-gt")) return bool_to_status(str_to_int(a) > str_to_int(b));
	if(match(op, "-le")) return bool_to_status(str_to_int(a) <= str_to_int(b));
	if(match(op, "-ge")) return bool_to_status(str_to_int(a) >= str_to_int(b));
	return -1;
}

int builtin_test(char** argv, int argc)
{
	int i;
	char* op;

	if(match(argv[0], "["))
	{
		if(argc > 0 && match(argv[argc - 1], "]"))
		{
			argc = argc - 1;
		}
	}

	if(argc <= 1) return 1;

	if(argc == 3)
	{
		op = argv[1];
		if(match(op, "-z")) return bool_to_status(strlen(argv[2]) == 0);
		if(match(op, "-n")) return bool_to_status(strlen(argv[2]) != 0);
		if(match(op, "-f")) return bool_to_status(path_is_reg(argv[2]));
		if(match(op, "-d")) return bool_to_status(path_is_dir(argv[2]));
		if(match(op, "-e")) return bool_to_status(access(argv[2], 0) == 0);
		if(match(op, "-s")) return bool_to_status(path_nonempty(argv[2]));
		if(match(op, "-r")) return bool_to_status(access(argv[2], 4) == 0);
		if(match(op, "-w")) return bool_to_status(access(argv[2], 2) == 0);
		if(match(op, "-x")) return bool_to_status(access(argv[2], 1) == 0);
		/* No lstat (and no asm): symlinks can't be detected portably, so
		   `-L`/`-h` are accepted but always false. */
		if(match(op, "-L") || match(op, "-h")) return bool_to_status(FALSE);
		if(match(op, "!"))
		{
			return bool_to_status(strlen(argv[2]) == 0);
		}
	}

	if(argc == 2)
	{
		/* Single arg: true if non-empty string. */
		if(match(argv[1], "!")) return 0;
		return bool_to_status(strlen(argv[1]) != 0);
	}

	if(argc == 4)
	{
		int tb;
		op = argv[2];
		tb = test_binop(argv[1], op, argv[3]);
		if(tb >= 0) return tb;

		/* `test ! EXPR` form. */
		if(match(argv[1], "!"))
		{
			if(match(argv[2], "-z")) return invert_status(bool_to_status(strlen(argv[3]) == 0));
			if(match(argv[2], "-n")) return invert_status(bool_to_status(strlen(argv[3]) != 0));
			if(match(argv[2], "-f")) return invert_status(bool_to_status(path_is_reg(argv[3])));
			if(match(argv[2], "-d")) return invert_status(bool_to_status(path_is_dir(argv[3])));
			if(match(argv[2], "-e")) return invert_status(bool_to_status(access(argv[3], 0) == 0));
			return bool_to_status(strlen(argv[3]) == 0);
		}
	}

	if(argc >= 5)
	{
		if(match(argv[1], "!") && argc == 5)
		{
			int tb;
			op = argv[3];
			tb = test_binop(argv[2], op, argv[4]);
			if(tb >= 0) return invert_status(tb);
		}

		/* `-a` (AND) and `-o` (OR): split around the operator and recurse. */
		{
			int ai;
			ai = 1;
			while(ai < argc)
			{
				if(match(argv[ai], "-a") || match(argv[ai], "-o"))
				{
					int left_result;
					int right_result;
					char** left_argv;
					char** right_argv;
					int li;

					left_argv = tmp_alloc((ai + 1) * sizeof(char*));
					li = 0;
					while(li < ai) { left_argv[li] = argv[li]; li = li + 1; }
					left_argv[ai] = NULL;
					left_result = builtin_test(left_argv, ai);

					right_argv = tmp_alloc((argc - ai + 1) * sizeof(char*));
					right_argv[0] = argv[0];
					li = 1;
					while(ai + li < argc) { right_argv[li] = argv[ai + li]; li = li + 1; }
					right_argv[li] = NULL;
					right_result = builtin_test(right_argv, li);

					if(match(argv[ai], "-a"))
					{
						return bool_to_status(left_result == 0 && right_result == 0);
					}
					else
					{
						return bool_to_status(left_result == 0 || right_result == 0);
					}
				}
				ai = ai + 1;
			}
		}
	}

	return 1;
}

/* TRUE if c is one of the IFS characters in `ifs`. */
int read_is_ifs(int c, char* ifs)
{
	int i;
	i = 0;
	while(ifs[i] != 0)
	{
		if(ifs[i] == c) return TRUE;
		i = i + 1;
	}
	return FALSE;
}

/* `trap ACTION SIG...`: hs only honors the EXIT (a.k.a. signal 0)
 * pseudo-signal, which covers the cleanup idiom (trap 'rm -rf "$T"' EXIT).
 * `trap - EXIT` / `trap '' EXIT` clears it. Real signal numbers/names are
 * accepted but ignored, since hs has no signal handling. */
int builtin_trap(char** argv, int argc)
{
	char* action;
	int i;
	int is_exit;

	if(argc < 2)
	{
		if(trap_exit_action != NULL)
		{
			sh_puts("trap -- '", stdout);
			sh_puts(trap_exit_action, stdout);
			sh_puts("' EXIT\n", stdout);
		}
		return 0;
	}

	action = argv[1];
	is_exit = FALSE;
	i = 2;
	while(i < argc)
	{
		if(match(argv[i], "EXIT") || match(argv[i], "0")) is_exit = TRUE;
		i = i + 1;
	}

	if(is_exit)
	{
		if(trap_exit_action != NULL) { free(trap_exit_action); trap_exit_action = NULL; }
		/* `trap - SIG` (reset) and `trap '' SIG` (ignore) both leave no
		   action to fire. */
		if(!match(action, "-") && action[0] != 0)
		{
			trap_exit_action = calloc(strlen(action) + 1, 1);
			strcpy(trap_exit_action, action);
		}
	}
	return 0;
}

/* Run the EXIT trap, if any, exactly once. Called from the `exit` builtin
 * and when the top-level script/`-c` string finishes. */
void run_exit_trap()
{
	char* act;
	if(trap_exit_action == NULL) return;
	act = trap_exit_action;
	trap_exit_action = NULL;
	expand_and_exec(act);
	free(act);
}

int builtin_read(char** argv, int argc)
{
	char* line;
	int li;
	int c;
	int rflag;
	int i;
	char** varnames;
	int nv;
	char* ifs;
	int pos;
	int k;
	int start;
	int end;
	char* field;

	rflag = FALSE;
	varnames = tmp_alloc(MAX_ARGV * sizeof(char*));
	nv = 0;
	i = 1;
	while(i < argc)
	{
		if(match(argv[i], "-r"))
		{
			rflag = TRUE;
		}
		else
		{
			varnames[nv] = argv[i];
			nv = nv + 1;
		}
		i = i + 1;
	}
	if(nv == 0)
	{
		varnames[0] = "REPLY";
		nv = 1;
	}

	line = tmp_alloc(4096);
	li = 0;

	while(TRUE)
	{
		c = hs_getc_in();
		if(c == EOF)
		{
			if(li == 0) return 1;
			break;
		}
		if(c == '\n') break;
		if(!rflag && c == '\\')
		{
			c = hs_getc_in();
			if(c == '\n') continue;
			if(c == EOF) break;
		}
		/* Reserve one byte for the terminating NUL. Lines longer
		   than 4095 bytes are silently truncated; the rest of the
		   line up to the next \n is discarded by the loop above. */
		if(li + 1 >= 4096) continue;
		line[li] = c;
		li = li + 1;
	}
	line[li] = 0;

	/* Split the line across the named variables on IFS. Each variable
	   but the last gets one field; the last gets the unsplit remainder
	   (leading IFS skipped, trailing IFS trimmed) -- POSIX `read` with
	   several names, the `while read -r a b c` idiom shpack relies on. */
	ifs = cached_ifs;
	pos = 0;
	k = 0;
	while(k < nv)
	{
		while(line[pos] != 0 && read_is_ifs(line[pos], ifs)) pos = pos + 1;

		if(k == nv - 1)
		{
			/* Last variable: remainder with trailing IFS trimmed. */
			end = li;
			while(end > pos && read_is_ifs(line[end - 1], ifs)) end = end - 1;
			field = tmp_alloc(end - pos + 1);
			memcpy(field, line + pos, end - pos);
			field[end - pos] = 0;
			var_set(varnames[k], field);
			pos = end;
		}
		else
		{
			start = pos;
			while(line[pos] != 0 && !read_is_ifs(line[pos], ifs)) pos = pos + 1;
			field = tmp_alloc(pos - start + 1);
			memcpy(field, line + start, pos - start);
			field[pos - start] = 0;
			var_set(varnames[k], field);
		}
		k = k + 1;
	}

	return 0;
}

/* Execution engine. */

int kw_len_match(char* buf, int len, char* kw)
{
	int klen;
	klen = strlen(kw);
	if(len != klen) return FALSE;
	if(memcmp(buf, kw, len) == 0) return TRUE;
	return FALSE;
}

/* Find the end of the next complete command in `buf`. Tracks quoting
 * and nesting depth. Returns the index past the final newline. */
int find_cmd_end(char* buf, int start, int total)
{
	int i;
	int c;
	int in_sq;
	int in_dq;
	int kw_depth;
	int brace_depth;
	int paren_depth;
	char* kw;
	int kwi;
	int np;                                  /* heredocs pending on this line */

	i = start;
	in_sq = FALSE;
	in_dq = FALSE;
	kw_depth = 0;
	brace_depth = 0;
	paren_depth = 0;
	np = 0;

	while(i < total)
	{
		c = buf[i];

		if(in_sq)
		{
			if(c == '\'') in_sq = FALSE;
			i = i + 1;
		}
		else if(in_dq)
		{
			if(c == '\\' && i + 1 < total) { i = i + 2; }
			else if(c == '"') { in_dq = FALSE; i = i + 1; }
			else { i = i + 1; }
		}
		else if(c == '\'') { in_sq = TRUE; i = i + 1; }
		else if(c == '"') { in_dq = TRUE; i = i + 1; }
		else if(c == '\\' && i + 1 < total) { i = i + 2; }
		else if(c == '#')
		{
			/* Autoconf balances each case-pattern `)` inside a multi-line
			   subshell with a `#(` marker comment (e.g. `case $x in #(` and
			   `;; #(` between arms), so paren-matching keeps an accurate depth.
			   Honor exactly that form -- a run of `(` immediately after the
			   `#` -- but do NOT count parens anywhere else in comment text:
			   an ordinary `# note (see below)` must not desync paren_depth and
			   swallow the following commands into this chunk. */
			i = i + 1;
			while(i < total && buf[i] == '(')
			{
				paren_depth = paren_depth + 1;
				i = i + 1;
			}
			while(i < total && buf[i] != '\n') i = i + 1;
		}
		else if(c == '\n')
		{
			int nlp;
			int cont;
			/* A line ending in &&, ||, or | continues onto the next line,
			   so the chunk must not end here -- else `a &&<nl> b ||<nl> c`
			   splits and the `c` runs unconditionally. (A single trailing
			   `&` is background and does end the command.) */
			cont = FALSE;
			nlp = i - 1;
			while(nlp >= start && (buf[nlp] == ' ' || buf[nlp] == '\t')) nlp = nlp - 1;
			if(nlp >= start)
			{
				if(buf[nlp] == '|') cont = TRUE;
				else if(buf[nlp] == '&' && nlp - 1 >= start && buf[nlp - 1] == '&') cont = TRUE;
			}

			i = i + 1;
			if(np > 0)
			{
				/* The heredoc bodies opened on this line follow the
				   newline; skip past them so the chunk includes them. */
				int hk;
				hk = 0;
				while(hk < np)
				{
					i = heredoc_skip_body(buf, total, i, fce_hd_dp[hk], fce_hd_s[hk]);
					hk = hk + 1;
				}
				np = 0;
				if(!cont && kw_depth <= 0 && brace_depth <= 0 && paren_depth <= 0) return i;
			}
			else if(!cont && kw_depth <= 0 && brace_depth <= 0 && paren_depth <= 0)
			{
				return i;
			}
		}
		else if(c == '<' && i + 1 < total && buf[i + 1] == '<')
		{
			/* Heredoc operator: record the delimiter so the matching
			   body (after the next newline) is pulled into this chunk. */
			int hstrip;
			int hexpand;
			i = i + 2;
			if(np < MAX_HEREDOC)
			{
				fce_hd_dp[np] = fce_hd_d + np * MAX_HEREDOC_DELIM;
				i = heredoc_parse_op(buf, total, i, fce_hd_dp[np], MAX_HEREDOC_DELIM, &hstrip, &hexpand);
				fce_hd_s[np] = hstrip;
				np = np + 1;
			}
		}
		else if(c == '{')
		{
			brace_depth = brace_depth + 1;
			i = i + 1;
		}
		else if(c == '}')
		{
			brace_depth = brace_depth - 1;
			i = i + 1;
		}
		else if(c == '(')
		{
			/* An empty paren pair `()` is a function-definition header
			   (name ()). Its body may sit on the next line(s), so skip the
			   intervening newlines instead of ending the chunk between the
			   header and the body -- otherwise the body runs as its own
			   command (executing the would-be function immediately). An
			   ordinary `(` (subshell) is stepped over normally. */
			int pk;
			pk = i + 1;
			while(pk < total && (buf[pk] == ' ' || buf[pk] == '\t')) pk = pk + 1;
			if(pk < total && buf[pk] == ')')
			{
				i = pk + 1;
				while(i < total && (buf[i] == ' ' || buf[i] == '\t' || buf[i] == '\n')) i = i + 1;
			}
			else
			{
				/* Subshell open: track depth so a multi-line `( ... \n ... )`
				   is not split at the newline after `(`. */
				paren_depth = paren_depth + 1;
				i = i + 1;
			}
		}
		else if(c == ')')
		{
			/* Subshell close. A `case` pattern terminator `)` has no matching
			   `(`; autoconf balances those via `#(` comments (counted above),
			   so when paren_depth is already 0 this is such a terminator and
			   we just step over it without underflowing. */
			if(paren_depth > 0) paren_depth = paren_depth - 1;
			i = i + 1;
		}
		else if(c >= 'a' && c <= 'z')
		{
			kw = buf + i;
			kwi = 0;
			while(i + kwi < total)
			{
				c = buf[i + kwi];
				if(!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_')) break;
				kwi = kwi + 1;
			}
			if(kwi > 0)
			{
				/* A keyword must be a whole word: a metacharacter or start
				   before it and a metacharacter or end after it. Otherwise
				   the `fi` in an option like `--cache-fi` would be taken as
				   the `fi` keyword and wrongly close a compound. */
				int kw_bnd;
				int bc;
				kw_bnd = TRUE;
				if(i > 0)
				{
					bc = buf[i - 1];
					if(!(bc == ' ' || bc == '\t' || bc == '\n' || bc == ';' ||
					     bc == '&' || bc == '|' || bc == '(' || bc == ')' ||
					     bc == '`')) kw_bnd = FALSE;
				}
				if(i + kwi < total)
				{
					bc = buf[i + kwi];
					if(!(bc == ' ' || bc == '\t' || bc == '\n' || bc == ';' ||
					     bc == '&' || bc == '|' || bc == '(' || bc == ')' ||
					     bc == '`')) kw_bnd = FALSE;
				}
				if(kw_bnd)
				{
					if(kw_len_match(kw, kwi, "if") ||
					   kw_len_match(kw, kwi, "while") ||
					   kw_len_match(kw, kwi, "until") ||
					   kw_len_match(kw, kwi, "for") ||
					   kw_len_match(kw, kwi, "case"))
					{
						kw_depth = kw_depth + 1;
					}
					else if(kw_len_match(kw, kwi, "fi") ||
					        kw_len_match(kw, kwi, "done") ||
					        kw_len_match(kw, kwi, "esac"))
					{
						kw_depth = kw_depth - 1;
					}
				}
				i = i + kwi;
			}
			else
			{
				i = i + 1;
			}
		}
		else
		{
			i = i + 1;
		}
	}
	return total;
}

/* Parse and execute a whole script held in `buf` (length `len`), one
 * complete command at a time. `path` is the source path for parse-error
 * diagnostics, or NULL for `-c`/string sources. The caller owns `buf`.
 * Split out of exec_source so both file execution and `hs -c <string>`
 * share the chunking loop (function/alias definitions stay visible to
 * later commands, which expand_and_exec's whole-string parse does not
 * give). */
int exec_buffer(char* buf, int len, char* path)
{
	int save_tok_count;
	int save_tok_pos;
	int* save_tok_type;
	char** save_tok_val;
	int* save_tok_src;
	char* save_lex_src;
	int save_lex_pos;
	int save_lex_len;
	int save_tmp_cur;
	int save_nd_count;
	int root;
	int result;
	int pos;
	int cmd_end;
	char* chunk;

	/* Save parser/arena state so nested source calls don't clobber it. */
	save_tok_count = tok_count;
	save_tok_pos = tok_pos;
	save_tok_type = tok_type;
	save_tok_val = tok_val;
	save_tok_src = tok_src_start;
	save_lex_src = lex_src;
	save_lex_pos = lex_pos;
	save_lex_len = lex_len;
	save_tmp_cur = tmp_cur;
	save_nd_count = nd_count;

	/* Fresh token arrays reused across all chunks of this file. */
	tok_type = calloc(MAX_TOK, sizeof(int));
	tok_val = calloc(MAX_TOK, sizeof(char*));
	tok_src_start = calloc(MAX_TOK, sizeof(int));
	tok_capacity = MAX_TOK;
	parsing_persistent = TRUE;

	result = 0;
	pos = 0;
	{
		int save_line_offset;
		char* save_orig_path;
		save_line_offset = lex_line_offset;
		save_orig_path = lex_orig_path;
		lex_orig_path = path;

	/* Execute one complete command at a time so aliases and function
	 * definitions become visible to subsequent commands. */
	while(pos < len)
	{
		while(pos < len && (buf[pos] == '\n' || buf[pos] == ' ' || buf[pos] == '\t'))
		{
			pos = pos + 1;
		}
		if(pos >= len) break;

		/* Reclaim per-chunk arena scratch. Don't touch nd_count: function
		 * bodies parsed earlier in the file must persist. */
		tmp_cur = save_tmp_cur;

		cmd_end = find_cmd_end(buf, pos, len);
		if(cmd_end <= pos) cmd_end = len;

		chunk = calloc(cmd_end - pos + 1, 1);
		memcpy(chunk, buf + pos, cmd_end - pos);
		chunk[cmd_end - pos] = 0;

		/* Count newlines in buf[0..pos] so chunk-local line numbers
		   land on the right line in the original file. */
		{
			int nl;
			int j;
			nl = 0;
			j = 0;
			while(j < pos)
			{
				if(buf[j] == '\n') nl = nl + 1;
				j = j + 1;
			}
			lex_line_offset = nl + 1;
			cur_lineno = nl + 1;
		}

		parse_init(chunk);
		root = parse_list();
		/* Tokens lived in the arena and have been perm_dup'd into nodes
		 * already; reclaim the arena before executing. */
		tmp_cur = save_tmp_cur;
		if(root >= 0)
		{
			result = exec_node(root);
			last_status = result;
			if(flag_errexit && !suppress_errexit && result != 0 && !g_andor_shortcircuit)
			{
				free(chunk);
				break;
			}
		}

		if(fn_return_flag || g_subshell_exit) { free(chunk); break; }

		free(chunk);
		pos = cmd_end;
	}

		lex_line_offset = save_line_offset;
		lex_orig_path = save_orig_path;
	}

	/* Restore parser state */
	free(tok_type);
	free(tok_val);
	free(tok_src_start);
	tok_count = save_tok_count;
	tok_pos = save_tok_pos;
	tok_type = save_tok_type;
	tok_val = save_tok_val;
	tok_src_start = save_tok_src;
	lex_src = save_lex_src;
	lex_pos = save_lex_pos;
	lex_len = save_lex_len;
	tmp_cur = save_tmp_cur;

	return result;
}

int exec_source(char* path)
{
	FILE* f;
	char* buf;
	int len;
	int cap;
	int c;
	int result;

	f = fopen(path, "r");
	if(f == NULL)
	{
		sh_err_puts("hs: cannot open: ");
		sh_err_puts(path);
		sh_err_puts("\n");
		return 1;
	}

	/* Read the whole file. */
	cap = 65536;
	buf = calloc(cap, 1);
	len = 0;
	while(TRUE)
	{
		c = fgetc(f);
		if(c == EOF) break;
		out_grow(&buf, &cap, len + 2);
		buf[len] = c;
		len = len + 1;
	}
	buf[len] = 0;
	fclose(f);

	result = exec_buffer(buf, len, path);
	free(buf);
	return result;
}

/* Parse and execute a code string in a fresh nested parser context. */
int expand_and_exec(char* code)
{
	int save_tok_count;
	int save_tok_pos;
	int save_tok_cap;
	int* save_tok_type;
	char** save_tok_val;
	int* save_tok_src;
	char* save_lex_src;
	int save_lex_pos;
	int save_lex_len;
	int save_tmp_cur;
	int save_nd_count;
	int save_loop_continue;
	int root;
	int result;

	save_tok_count = tok_count;
	save_tok_pos = tok_pos;
	save_tok_cap = tok_capacity;
	save_tok_type = tok_type;
	save_tok_val = tok_val;
	save_tok_src = tok_src_start;
	save_lex_src = lex_src;
	save_lex_pos = lex_pos;
	save_lex_len = lex_len;
	save_tmp_cur = tmp_cur;
	save_nd_count = nd_count;
	save_loop_continue = loop_continue_flag;
	loop_continue_flag = FALSE;

	tok_type = tmp_alloc(MAX_TOK_NESTED * sizeof(int));
	tok_val = tmp_alloc(MAX_TOK_NESTED * sizeof(char*));
	tok_src_start = tmp_alloc(MAX_TOK_NESTED * sizeof(int));
	tok_capacity = MAX_TOK_NESTED;
	parsing_persistent = FALSE;
	parse_init(code);
	root = parse_list();
	result = 0;
	if(root >= 0) result = exec_node(root);

	tok_count = save_tok_count;
	tok_pos = save_tok_pos;
	tok_capacity = save_tok_cap;
	/* Restore parsing_persistent based on the outer capacity. */
	parsing_persistent = save_tok_cap > MAX_TOK_NESTED;
	tok_type = save_tok_type;
	tok_val = save_tok_val;
	tok_src_start = save_tok_src;
	lex_src = save_lex_src;
	lex_pos = save_lex_pos;
	lex_len = save_lex_len;
	tmp_cur = save_tmp_cur;
	nd_count = save_nd_count;
	loop_continue_flag = save_loop_continue;

	return result;
}

/* Dispatch to a builtin if cmd is one. Returns the exit status, or
 * -1 if cmd is not a builtin. */
/* TRUE if NAME is one of hs's builtins. Used by `type` and `command -v`. */
int is_shell_builtin(char* name)
{
	if(match(name, "echo") || match(name, "printf") || match(name, "test") ||
	   match(name, "read") || match(name, "eval") || match(name, "local") ||
	   match(name, "export") || match(name, "set") || match(name, "shift") ||
	   match(name, "unset") || match(name, "return") || match(name, "break") ||
	   match(name, "continue") || match(name, "alias") || match(name, "unalias") ||
	   match(name, "cd") || match(name, "pwd") || match(name, "true") || match(name, "false") ||
	   match(name, ":") || match(name, ".") || match(name, "source") ||
	   match(name, "exit") || match(name, "command") || match(name, "typeset") ||
	   match(name, "trap") || match(name, "type") || match(name, "exec") || match(name, "wait") ||
	   match(name, "umask") ||
	   match(name, "["))
		return TRUE;
	return FALSE;
}

/* Resolve a command name to an executable path via PATH (absolute/relative
   names are checked directly). Returns a heap path the caller frees, or
   NULL if not found. Mirrors exec_simple's resolver, shared by `type` and
   `command -v`. */
char* path_lookup(char* cmd)
{
	char* path;
	char* mpath;
	char* p;
	char* colon;
	char* trial;
	char* result;

	if(cmd[0] == '/' || cmd[0] == '.')
	{
		if(access(cmd, 0) == 0) return str_dup(cmd);
		return NULL;
	}
	path = var_get("PATH");
	if(path[0] == 0) return NULL;
	mpath = str_dup(path);
	p = mpath;
	result = NULL;
	while(p != NULL)
	{
		if(p[0] == 0) break;
		colon = strchr(p, ':');
		if(colon != NULL) colon[0] = 0;
		trial = calloc(strlen(p) + strlen(cmd) + 2, 1);
		strcpy(trial, p);
		strcat(trial, "/");
		strcat(trial, cmd);
		if(access(trial, 0) == 0) { result = trial; break; }
		free(trial);
		if(colon != NULL) p = colon + 1;
		else p = NULL;
	}
	free(mpath);
	return result;
}

/* `type NAME...`: report how each name would be resolved (alias, function,
   builtin, or PATH executable). shpack's is_function greps this output for
   "function". */
int builtin_type(char** argv, int argc)
{
	int i;
	int rc;
	char* name;
	char* p;

	rc = 0;
	i = 1;
	while(i < argc)
	{
		name = argv[i];
		if(al_find(name) >= 0)
		{
			sh_puts(name, stdout);
			sh_puts(" is aliased to `", stdout);
			sh_puts(al_vals[al_find(name)], stdout);
			sh_puts("'\n", stdout);
		}
		else if(fn_find(name) >= 0)
		{
			sh_puts(name, stdout);
			sh_puts(" is a shell function\n", stdout);
		}
		else if(is_shell_builtin(name))
		{
			sh_puts(name, stdout);
			sh_puts(" is a shell builtin\n", stdout);
		}
		else
		{
			p = path_lookup(name);
			if(p != NULL)
			{
				sh_puts(name, stdout);
				sh_puts(" is ", stdout);
				sh_puts(p, stdout);
				sh_puts("\n", stdout);
				free(p);
			}
			else
			{
				sh_err_puts("hs: type: ");
				sh_err_puts(name);
				sh_err_puts(": not found\n");
				rc = 1;
			}
		}
		i = i + 1;
	}
	sh_flush(stdout);
	return rc;
}

int exec_builtin(char** argv, int argc)
{
	char* cmd;
	char* val;
	int i;
	int j;
	char* name;
	char* body;

	if(argc == 0) return -1;
	cmd = argv[0];

	if(match(cmd, "true") || match(cmd, ":"))
	{
		last_status = 0;
		return 0;
	}
	if(match(cmd, "false"))
	{
		last_status = 1;
		return 1;
	}

	if(match(cmd, "echo"))
	{
		last_status = builtin_echo(argv, argc);
		return last_status;
	}

	if(match(cmd, "printf"))
	{
		last_status = builtin_printf(argv, argc);
		return last_status;
	}

	if(match(cmd, "test") || match(cmd, "["))
	{
		last_status = builtin_test(argv, argc);
		return last_status;
	}

	if(match(cmd, "read"))
	{
		last_status = builtin_read(argv, argc);
		return last_status;
	}

	if(match(cmd, "exit"))
	{
		i = 0;
		if(argc > 1) i = str_to_int(argv[1]);
		/* Inside an in-process subshell, `exit` ends the subshell, not the
		   whole shell: set the unwind flag (checked alongside fn_return_flag in
		   the exec loops) and let run_isolated() catch it. */
		if(subshell_depth > 0)
		{
			g_subshell_exit = TRUE;
			g_subshell_exit_status = i;
			last_status = i;
			return i;
		}
		run_exit_trap();
		sh_flush(stdout);
		exit(i);
	}

	if(match(cmd, "trap"))
	{
		last_status = builtin_trap(argv, argc);
		return last_status;
	}

	if(match(cmd, "wait"))
	{
		/* No job control: backgrounded commands already ran synchronously,
		   so there is nothing to wait for. */
		last_status = 0;
		return 0;
	}

	if(match(cmd, "type"))
	{
		last_status = builtin_type(argv, argc);
		return last_status;
	}

	if(match(cmd, "umask"))
	{
		/* `umask [mode]`: with an octal mode, set the file-creation mask;
		   with none, print it as 4-digit octal. config.guess does
		   `umask 077` to make a private temp dir, so the set form is what
		   the bootstrap actually needs. The minimal kernels have no umask
		   syscall, so we track the value in g_umask rather than reading it
		   back via umask(0) (which would return arch-dependent garbage --
		   see KERNEL.md); umask() is still called so the POSIX host honours
		   the mask. */
		if(argc >= 2)
		{
			int m;
			int di;
			char* a;
			m = 0;
			di = 0;
			a = argv[1];
			while(a[di] != 0)
			{
				if(a[di] < '0' || a[di] > '7') break;
				m = m * 8 + (a[di] - '0');
				di = di + 1;
			}
			g_umask = m;
			umask(m);
		}
		else
		{
			int m;
			char* ob;
			m = g_umask;
			/* Heap (not a local array): M2-Planet mishandles local arrays. */
			ob = tmp_alloc(6);
			ob[0] = '0';
			ob[1] = '0' + ((m >> 6) & 7);
			ob[2] = '0' + ((m >> 3) & 7);
			ob[3] = '0' + (m & 7);
			ob[4] = '\n';
			ob[5] = 0;
			sh_puts(ob, stdout);
		}
		last_status = 0;
		return 0;
	}

	if(match(cmd, "exec"))
	{
		/* `exec CMD args`: replace this shell process with CMD. shpack
		 * ends its install driver with `exec make -f dag.mk ...`. Any
		 * redirections were already applied for this command. */
		if(argc >= 2)
		{
			char* xp;
			char** xenvp;
			xp = path_lookup(argv[1]);
			if(xp == NULL)
			{
				sh_err_puts("hs: exec: ");
				sh_err_puts(argv[1]);
				sh_err_puts(": not found\n");
				exit(127);
			}
			sh_flush(stdout);
			xenvp = var_build_envp();
			execve(xp, argv_drop(argv, argc, 1), xenvp);
			sh_err_puts("hs: exec: ");
			sh_err_puts(argv[1]);
			sh_err_puts(": cannot execute\n");
			exit(126);
		}
		/* `exec` with only redirections: hs applies redirects per-command,
		 * so there is nothing to make permanent here. */
		last_status = 0;
		return 0;
	}

	if(match(cmd, "cd"))
	{
		if(argc > 1)
		{
			if(chdir(argv[1]) != 0)
			{
				sh_err_puts("hs: cd: ");
				sh_err_puts(argv[1]);
				sh_err_puts(": no such directory\n");
				last_status = 1;
				return 1;
			}
		}
		last_status = 0;
		return 0;
	}

	if(match(cmd, "pwd"))
	{
		char* cwd;
		/* No external `pwd` exists on a minimal bootstrap kernel, so hs reports
		   the working directory itself via getcwd. */
		cwd = getcwd(calloc(4096, 1), 4096);
		if(cwd == NULL) { last_status = 1; return 1; }
		sh_puts(cwd, stdout);
		sh_puts("\n", stdout);
		sh_flush(stdout);
		last_status = 0;
		return 0;
	}

	if(match(cmd, "export"))
	{
		i = 1;
		while(i < argc)
		{
			name = argv[i];
			val = strchr(name, '=');
			if(val != NULL)
			{
				val[0] = 0;
				val = val + 1;
				var_set(name, val);
			}
			var_export(name);
			i = i + 1;
		}
		last_status = 0;
		return 0;
	}

	if(match(cmd, "local") || match(cmd, "typeset"))
	{
		i = 1;
		while(i < argc)
		{
			if(match(cmd, "typeset") && match(argv[i], "-f"))
			{
				/* `typeset -f name`: print one function body. */
				if(i + 1 < argc)
				{
					i = i + 1;
					j = fn_find(argv[i]);
					if(j >= 0)
					{
						sh_puts(fn_names[j], stdout);
						sh_puts(" () {\n", stdout);
						sh_puts(fn_bodies[j], stdout);
						sh_puts("\n}\n", stdout);
						sh_flush(stdout);
					}
				}
				else
				{
					/* `typeset -f` with no name: print all bodies. */
					j = 0;
					while(j < fn_count)
					{
						if(fn_names[j][0] != 0)
						{
							sh_puts(fn_names[j], stdout);
							sh_puts(" () {\n", stdout);
							sh_puts(fn_bodies[j], stdout);
							sh_puts("\n}\n", stdout);
						}
						j = j + 1;
					}
					sh_flush(stdout);
				}
				i = i + 1;
				continue;
			}
			if(match(cmd, "typeset") && match(argv[i], "+f"))
			{
				/* `typeset +f`: list function names. */
				j = 0;
				while(j < fn_count)
				{
					if(fn_names[j][0] != 0)
					{
						sh_puts(fn_names[j], stdout);
						sh_puts("\n", stdout);
					}
					j = j + 1;
				}
				sh_flush(stdout);
				i = i + 1;
				continue;
			}

			name = argv[i];
			val = strchr(name, '=');
			if(val != NULL)
			{
				val[0] = 0;
				val = val + 1;
				lsc_declare(name, val);
			}
			else
			{
				lsc_declare(name, "");
			}
			i = i + 1;
		}
		last_status = 0;
		return 0;
	}

	if(match(cmd, "unset"))
	{
		i = 1;
		if(argc > 1 && match(argv[1], "-f"))
		{
			/* `unset -f name`: drop a function. */
			i = 2;
			while(i < argc)
			{
				fn_unset(argv[i]);
				i = i + 1;
			}
		}
		else
		{
			while(i < argc)
			{
				var_unset(argv[i]);
				i = i + 1;
			}
		}
		last_status = 0;
		return 0;
	}

	if(match(cmd, "set"))
	{
		if(argc == 1) { last_status = 0; return 0; }

		i = 1;
		while(i < argc)
		{
			val = argv[i];
			if(match(val, "--"))
			{
				/* `set -- args...`: replace positional params, keep $0. */
				i = i + 1;
				pp_argc = 1;
				while(i < argc)
				{
					if(pp_argc < MAX_ARGV)
					{
						pp_argv[pp_argc] = str_dup(argv[i]);
						pp_argc = pp_argc + 1;
					}
					i = i + 1;
				}
				break;
			}
			else if(val[0] == '-')
			{
				j = 1;
				while(val[j] != 0)
				{
					if(val[j] == 'e') flag_errexit = TRUE;
					else if(val[j] == 'u') flag_nounset = TRUE;
					/* `-f` (noglob) accepted but inert: hs never globs. */
					j = j + 1;
				}
			}
			else if(val[0] == '+')
			{
				j = 1;
				while(val[j] != 0)
				{
					if(val[j] == 'e') flag_errexit = FALSE;
					else if(val[j] == 'u') flag_nounset = FALSE;
					/* `+f` accepted but inert: hs never globs. */
					j = j + 1;
				}
			}
			else
			{
				/* `set arg...` with a non-option operand: replace the
				   positional parameters (the `--` is optional when no
				   operand looks like an option). */
				pp_argc = 1;
				while(i < argc)
				{
					if(pp_argc < MAX_ARGV)
					{
						pp_argv[pp_argc] = str_dup(argv[i]);
						pp_argc = pp_argc + 1;
					}
					i = i + 1;
				}
				break;
			}
			i = i + 1;
		}
		last_status = 0;
		return 0;
	}

	if(match(cmd, "shift"))
	{
		i = 1;
		if(argc > 1) i = str_to_int(argv[1]);
		if(i > 0 && pp_argc > 1)
		{
			j = 1;
			while(j + i < pp_argc)
			{
				pp_argv[j] = pp_argv[j + i];
				j = j + 1;
			}
			pp_argc = pp_argc - i;
			if(pp_argc < 1) pp_argc = 1;
		}
		last_status = 0;
		return 0;
	}

	if(match(cmd, "return"))
	{
		if(argc > 1) last_status = str_to_int(argv[1]);
		fn_return_flag = TRUE;
		return last_status;
	}

	if(match(cmd, "break"))
	{
		loop_break_level = 1;
		if(argc > 1) loop_break_level = str_to_int(argv[1]);
		return 0;
	}

	if(match(cmd, "continue"))
	{
		loop_continue_flag = TRUE;
		return 0;
	}

	if(match(cmd, "eval"))
	{
		if(argc <= 1) { last_status = 0; return 0; }
		/* Concatenate args with spaces, then re-parse and execute. */
		val = tmp_alloc(4096);
		j = 0;
		i = 1;
		while(i < argc)
		{
			if(i > 1)
			{
				require(j + 1 < 4096, "hs: eval: arguments too long\n");
				val[j] = ' ';
				j = j + 1;
			}
			{
				int alen;
				alen = strlen(argv[i]);
				require(j + alen + 1 < 4096, "hs: eval: arguments too long\n");
				memcpy(val + j, argv[i], alen);
				j = j + alen;
			}
			i = i + 1;
		}
		val[j] = 0;
		last_status = expand_and_exec(val);
		return last_status;
	}

	if(match(cmd, ".") || match(cmd, "source"))
	{
		if(argc < 2)
		{
			sh_err_puts("hs: source: filename argument required\n");
			last_status = 2;
			return 2;
		}
		last_status = exec_source(argv[1]);
		return last_status;
	}

	if(match(cmd, "alias"))
	{
		if(argc == 1)
		{
			i = 0;
			while(i < al_count)
			{
				if(al_names[i][0] != 0)
				{
					sh_puts("alias ", stdout);
					sh_puts(al_names[i], stdout);
					sh_puts("='", stdout);
					sh_puts(al_vals[i], stdout);
					sh_puts("'\n", stdout);
				}
				i = i + 1;
			}
			sh_flush(stdout);
			last_status = 0;
			return 0;
		}
		i = 1;
		while(i < argc)
		{
			val = strchr(argv[i], '=');
			if(val != NULL)
			{
				val[0] = 0;
				val = val + 1;
				al_set(argv[i], val);
			}
			i = i + 1;
		}
		last_status = 0;
		return 0;
	}

	if(match(cmd, "unalias"))
	{
		i = 1;
		while(i < argc)
		{
			al_unset(argv[i]);
			i = i + 1;
		}
		last_status = 0;
		return 0;
	}

	if(match(cmd, "command"))
	{
		if(argc >= 3 && match(argv[1], "-v"))
		{
			/* `command -v NAME`: print resolved name if it exists. */
			name = argv[2];
			if(match(name, "echo") || match(name, "printf") || match(name, "test") ||
			   match(name, "read") || match(name, "eval") || match(name, "local") ||
			   match(name, "export") || match(name, "set") || match(name, "shift") ||
			   match(name, "unset") || match(name, "return") || match(name, "break") ||
			   match(name, "continue") || match(name, "alias") || match(name, "unalias") ||
			   match(name, "cd") || match(name, "pwd") || match(name, "true") || match(name, "false") ||
			   match(name, ":") || match(name, ".") || match(name, "source") ||
			   match(name, "exit") || match(name, "command") || match(name, "typeset") ||
			   match(name, "["))
			{
				sh_puts(name, stdout);
				sh_puts("\n", stdout);
				sh_flush(stdout);
				last_status = 0;
				return 0;
			}
			if(fn_find(name) >= 0)
			{
				sh_puts(name, stdout);
				sh_puts("\n", stdout);
				sh_flush(stdout);
				last_status = 0;
				return 0;
			}
			if(al_find(name) >= 0)
			{
				sh_puts("alias ", stdout);
				sh_puts(name, stdout);
				sh_puts("='", stdout);
				sh_puts(al_vals[al_find(name)], stdout);
				sh_puts("'\n", stdout);
				sh_flush(stdout);
				last_status = 0;
				return 0;
			}
			{
				char* rp;
				rp = path_lookup(name);
				if(rp != NULL)
				{
					sh_puts(rp, stdout);
					sh_puts("\n", stdout);
					sh_flush(stdout);
					free(rp);
					last_status = 0;
					return 0;
				}
			}
			last_status = 1;
			return 1;
		}
		if(argc >= 3 && match(argv[1], "-p"))
		{
			/* `command -p`: run with the default PATH (we just fall through). */
			argv = argv_drop(argv, argc, 2);
			argc = argc - 2;
			return -1;
		}
		if(argc >= 2)
		{
			/* `command NAME args...`: bypass function lookup. */
			argv = argv_drop(argv, argc, 1);
			argc = argc - 1;
			return -1;
		}
		last_status = 0;
		return 0;
	}

	return -1;
}

/* Expand and apply NAME=VALUE assignment tokens. With is_local set,
 * declares them in the local scope; otherwise sets globals. */
void apply_assigns(char** raw_assigns, int assign_count, int is_local)
{
	int i;
	char* name;
	char* val;
	i = 0;
	while(i < assign_count)
	{
		name = str_dup(raw_assigns[i]);
		val = strchr(name, '=');
		if(val != NULL)
		{
			val[0] = 0;
			val = val + 1;
			/* Assignment RHS is not field-split. */
			g_field_split = FALSE;
			val = strip_quotes(expand_word(val));
			g_field_split = TRUE;
			if(is_local) { lsc_declare(name, val); }
			else { var_set(name, val); }
		}
		i = i + 1;
	}
}

/* Materialize a heredoc body to a fresh temp file and return a read-only fd
   open on it (-1 on failure). `rfile` points at the redirect marker: 'H' means
   expand the body, 'h' means literal. The temp path is recorded in tmps/*ntmp
   for later unlink and returned through *out_path. Both redirect appliers
   (exec_simple, exec_redir) share this; they differ only in how they install
   the resulting fd. A new file per execution keeps loop bodies re-expanded. */
int heredoc_body_to_fd(char* rfile, char** tmps, int* ntmp, char** out_path)
{
	char* hbody;
	char* htmp;
	int hfd;
	int hlen;
	int hwn;
	int hwr;

	if(rfile[0] == 'H') hbody = strip_quotes(expand_word(rfile + 1));
	else hbody = rfile + 1;
	hlen = strlen(hbody);

	htmp = make_cap_tmpfile_path();
	hfd = open(htmp, O_WRONLY | O_TRUNC | HS_O_NOFOLLOW, 384);
	/* Fail loudly (like make_cap_tmpfile_path itself) rather than deliver a
	   silently-empty heredoc: an unwritten/short-written body would otherwise
	   reopen RDONLY on the empty file and feed the command no data, silently
	   corrupting e.g. autoconf's `cat <<EOF >>confdefs.h`. */
	require(hfd >= 0, "hs: heredoc: cannot open temp file\n");
	hwn = 0;
	while(hwn < hlen)
	{
		hwr = write(hfd, hbody + hwn, hlen - hwn);
		if(hwr <= 0) break;
		hwn = hwn + hwr;
	}
	close(hfd);
	require(hwn == hlen, "hs: heredoc: short write to temp file\n");
	if(ntmp[0] < MAX_HEREDOC)
	{
		tmps[ntmp[0]] = htmp;
		ntmp[0] = ntmp[0] + 1;
	}
	out_path[0] = htmp;
	return open(htmp, O_RDONLY, 0);
}

/* Snapshot the live fd/path of a stdio slot (hs_in/out/err) on first touch, so
   the redirect machinery can restore it after the redirected command. cur_*
   point at the live hs_*_fd/hs_*_path; saved_* are the caller's bookkeeping. */
void redir_save(int* cur_fd, char** cur_path, int* saved_fd, char** saved_path)
{
	if(*saved_fd < 0)
	{
		*saved_fd = *cur_fd;
		*saved_path = *cur_path;
	}
}

/* Redirect a stdio slot to `fd`/`path`: redir_save it, close any fd this slot
   previously opened, then install the new one. The shared core of every
   open-file redirect in exec_simple and exec_redir (the N>&M dup cases use only
   redir_save, since they alias an existing fd rather than opening one). The
   saved-fd and opened-fd bookkeeping is passed by address so no globals are
   needed -- exec_redir recurses, so its per-call locals must stay per-call. */
void redir_install(int* cur_fd, char** cur_path, int* saved_fd, char** saved_path, int* opened, int fd, char* path)
{
	redir_save(cur_fd, cur_path, saved_fd, saved_path);
	if(*opened >= 0) close(*opened);
	*opened = fd;
	*cur_fd = fd;
	*cur_path = path;
}

/* Execute a simple command node */
int exec_simple(int idx)
{
	int argc;
	int assign_count;
	char** raw_argv;
	char** raw_assigns;
	char** argv;
	int exp_argc;
	int i;
	int j;
	char* cmd;
	int fi;
	int result;
	char** envp;
	char* path;
	char* pathbuf;
	char* name;
	char* val;
	int save_argc;
	char** save_argv;
	int fd;
	char* rfile;
	int skip_fn;
	char* body;
	int exec_save_tmp;
	int saved_in_fd;
	int saved_out_fd;
	int saved_err_fd;
	char* saved_in_path;
	char* saved_out_path;
	char* saved_err_path;
	int opened_in;   /* fd we opened for <;  closed on restore */
	int opened_out;  /* fd we opened for >;  closed on restore */
	int opened_err;  /* fd we opened for 2>; closed on restore */
	int rfd;
	int src_fd;
	char* src_path;
	/* A path redirect (<, >, >>) whose open() failed: the command must not
	   run (POSIX), so we record the failing path and abort after the loop. */
	int redir_failed;
	char* redir_failed_path;
	/* Heredoc temp files created for this command, unlinked after it runs.
	   Heap-backed (tmp arena), not a local array: M2-Planet mishandles
	   local pointer arrays, corrupting the stack frame. */
	char** heredoc_tmps;
	int   heredoc_ntmp;
	/* Command-prefix assignments (`VAR=val cmd`): applied to the variable
	   table for the command's duration (so a builtin like `read` sees them
	   and an external inherits them via var_build_envp), then restored. */
	char** pre_names;
	char** pre_old;
	int*   pre_had;
	int*   pre_exp;
	int    pre_n;

	heredoc_tmps = tmp_alloc(MAX_HEREDOC * sizeof(char*));
	heredoc_ntmp = 0;
	redir_failed = FALSE;
	redir_failed_path = NULL;
	pre_n = 0;
	argc = nd_a[idx];
	raw_argv = nd_argv[idx];
	assign_count = nd_c[idx];
	raw_assigns = nd_assigns[idx];
	exec_save_tmp = tmp_cur;

	/* Split argv into plain words and redirect pseudo-args. */
	{
		char** words;
		int wc;
		char** redirs;
		int rc;

		words = tmp_alloc(MAX_ARGV * sizeof(char*));
		redirs = tmp_alloc(MAX_ARGV * sizeof(char*));
		wc = 0;
		rc = 0;
		i = 0;
		while(i < argc)
		{
			if(is_redir_word(raw_argv[i]))
			{
				redirs[rc] = raw_argv[i];
				rc = rc + 1;
			}
			else
			{
				words[wc] = raw_argv[i];
				wc = wc + 1;
			}
			i = i + 1;
		}

		argv = expand_argv(words, wc, &exp_argc);

		if(exp_argc == 0 && assign_count > 0)
		{
			/* Assignments-only command. $? is 0 unless a value contained a
			   command substitution, in which case it is that substitution's
			   status. Don't pre-clear last_status: a `$?` in the RHS must
			   still read the previous command's status. */
			{
				int sub_before;
				sub_before = g_cmdsub_count;
				apply_assigns(raw_assigns, assign_count, FALSE);
				if(g_cmdsub_count == sub_before) last_status = 0;
			}
			tmp_cur = exec_save_tmp;
			return last_status;
		}

		if(exp_argc == 0)
		{
			last_status = 0;
			tmp_cur = exec_save_tmp;
			return 0;
		}

		cmd = argv[0];
		skip_fn = FALSE;

		/* Apply command-prefix assignments (`VAR=val cmd`) to the variable
		   table, saving prior values to restore afterward. */
		if(assign_count > 0)
		{
			int pai;
			pre_names = tmp_alloc(assign_count * sizeof(char*));
			pre_old = tmp_alloc(assign_count * sizeof(char*));
			pre_had = tmp_alloc(assign_count * sizeof(int));
			pre_exp = tmp_alloc(assign_count * sizeof(int));
			pai = 0;
			while(pai < assign_count)
			{
				char* pnm;
				char* peq;
				pnm = str_dup(raw_assigns[pai]);
				peq = strchr(pnm, '=');
				if(peq != NULL)
				{
					int pfi;
					char* pval;
					peq[0] = 0;
					pval = peq + 1;
					pfi = var_find(pnm);
					pre_names[pre_n] = pnm;
					if(pfi >= 0) { pre_had[pre_n] = TRUE; pre_old[pre_n] = str_dup(var_vals[pfi]); pre_exp[pre_n] = var_exported[pfi]; }
					else { pre_had[pre_n] = FALSE; pre_old[pre_n] = NULL; pre_exp[pre_n] = 0; }
					g_field_split = FALSE;
					var_set(pnm, strip_quotes(expand_word(pval)));
					g_field_split = TRUE;
					/* In effect for this command only, but visible to an
					   external child, so it must be exported. */
					var_export(pnm);
					pre_n = pre_n + 1;
				}
				else
				{
					free(pnm);
				}
				pai = pai + 1;
			}
		}

		/* Apply redirects to the shell's own stdio fds (hs_in_fd/out/err) so
		 * builtins, subshells and pipelines honor them. Externals are never
		 * redirected -- they inherit the real fds (KERNEL.md). State is purely
		 * (int fd, char* path) per std stream; no FILE* manipulation. */
		saved_in_fd = -1;
		saved_out_fd = -1;
		saved_err_fd = -1;
		saved_in_path = NULL;
		saved_out_path = NULL;
		saved_err_path = NULL;
		opened_in = -1;
		opened_out = -1;
		opened_err = -1;
		if(rc > 0)
		{
			/* Redirect targets are a single-word context: an unquoted $var /
			   $(cmd) keeps its whitespace verbatim and is never field-split
			   (else a marker byte would be baked into the filename). */
			g_field_split = FALSE;
			i = 0;
			while(i < rc)
			{
				rfile = redirs[i] + 1; /* skip \x06 marker. */
				rfd = -1;

				if(rfile[0] >= '0' && rfile[0] <= '9')
				{
					rfd = 0;
					while(rfile[0] >= '0' && rfile[0] <= '9')
					{
						rfd = rfd * 10 + (rfile[0] - '0');
						rfile = rfile + 1;
					}
				}

				if(rfile[0] == 'h' || rfile[0] == 'H')
				{
					/* Heredoc body: \x06h<literal-body> or
					   \x06H<annotated-body>. Materialize the body to a
					   fresh temp file and make it stdin, exactly like
					   `< tmpfile`. A new file per execution keeps loop
					   bodies re-expanded; it is unlinked after the
					   command runs. */
					char* htmp;

					fd = heredoc_body_to_fd(rfile, heredoc_tmps, &heredoc_ntmp, &htmp);
					if(fd >= 0) redir_install(&hs_in_fd, &hs_in_path, &saved_in_fd, &saved_in_path, &opened_in, fd, htmp);
				}
				else if(rfile[0] == '<' && rfile[1] == '&')
				{
					/* N<&M input dup (e.g. `<&0`, `exec 7<&0`). Parent-side int
					   update so builtins read the duped stream. (Externals are
					   never redirected -- they inherit the real fds.) */
					int src_m;
					if(rfd < 0) rfd = 0;
					src_m = str_to_int(rfile + 2);
					if(src_m == 0) { src_fd = hs_in_fd; src_path = hs_in_path; }
					else if(src_m == 1) { src_fd = hs_out_fd; src_path = hs_out_path; }
					else { src_fd = hs_err_fd; src_path = hs_err_path; }

					if(rfd == 0)
					{
						redir_save(&hs_in_fd, &hs_in_path, &saved_in_fd, &saved_in_path);
						hs_in_fd = src_fd; hs_in_path = src_path;
					}
					else if(rfd == 1)
					{
						redir_save(&hs_out_fd, &hs_out_path, &saved_out_fd, &saved_out_path);
						hs_out_fd = src_fd; hs_out_path = src_path;
					}
					else if(rfd == 2)
					{
						redir_save(&hs_err_fd, &hs_err_path, &saved_err_fd, &saved_err_path);
						hs_err_fd = src_fd; hs_err_path = src_path;
					}
				}
				else if(rfile[0] == '<')
				{
					rfile = strip_quotes(expand_word(rfile + 1));
					fd = open(rfile, O_RDONLY, 0);
					if(fd >= 0) redir_install(&hs_in_fd, &hs_in_path, &saved_in_fd, &saved_in_path, &opened_in, fd, rfile);
					else { redir_failed = TRUE; redir_failed_path = rfile; }
				}
				else if(rfile[0] == '>' && rfile[1] == '>')
				{
					if(rfd < 0) rfd = 1;
					rfile = strip_quotes(expand_word(rfile + 2));
					fd = open_append(rfile);
					if(fd >= 0)
					{
						if(rfd == 1) redir_install(&hs_out_fd, &hs_out_path, &saved_out_fd, &saved_out_path, &opened_out, fd, rfile);
						else if(rfd == 2) redir_install(&hs_err_fd, &hs_err_path, &saved_err_fd, &saved_err_path, &opened_err, fd, rfile);
					}
					else { redir_failed = TRUE; redir_failed_path = rfile; }
				}
				else if(rfile[0] == '>')
				{
					if(rfd < 0) rfd = 1;
					rfile = strip_quotes(expand_word(rfile + 1));
					if(rfile[0] == '&')
					{
						/* N>&M dup. Snapshot the source state before
						 * touching the target so the int copy is correct. */
						int src_m;
						src_m = str_to_int(rfile + 1);
						if(src_m == 1)
						{
							src_fd = hs_out_fd;
							src_path = hs_out_path;
						}
						else
						{
							src_fd = hs_err_fd;
							src_path = hs_err_path;
						}

						/* Parent-side int update so builtins merge. */
						if(rfd == 1)
						{
							redir_save(&hs_out_fd, &hs_out_path, &saved_out_fd, &saved_out_path);
							hs_out_fd = src_fd;
							hs_out_path = src_path;
						}
						else if(rfd == 2)
						{
							redir_save(&hs_err_fd, &hs_err_path, &saved_err_fd, &saved_err_path);
							hs_err_fd = src_fd;
							hs_err_path = src_path;
						}
					}
					else
					{
						/* Plain `>` truncate: O_TRUNC only (no O_APPEND), matching
						   exec_redir and POSIX -- a command that seeks backward in
						   its own stdout must overwrite, not append. */
						fd = open(rfile, O_WRONLY | O_CREAT | O_TRUNC, 420);
						if(fd >= 0)
						{
							if(rfd == 1) redir_install(&hs_out_fd, &hs_out_path, &saved_out_fd, &saved_out_path, &opened_out, fd, rfile);
							else if(rfd == 2) redir_install(&hs_err_fd, &hs_err_path, &saved_err_fd, &saved_err_path, &opened_err, fd, rfile);
						}
						else { redir_failed = TRUE; redir_failed_path = rfile; }
					}
				}
				i = i + 1;
			}
			g_field_split = TRUE;
		}

		/* A redirect target that couldn't be opened fails the command without
		   running it (POSIX), instead of silently letting output go to the
		   inherited fd. Restores fds via the shared cleanup. */
		if(redir_failed)
		{
			sh_err_puts("hs: cannot open redirect target: ");
			sh_err_puts(redir_failed_path);
			sh_err_puts("\n");
			last_status = 1;
			goto exec_simple_cleanup;
		}

		/* Pipe right-side stdin: CMD_PIPE staged a tmp file path here for
		 * us to consume as if it were `< file`, feeding the right side's
		 * builtins through hs_in_fd. (An external on the right inherits the
		 * real stdin and does not read the staged file -- pipelines connect
		 * builtins; see KERNEL.md.) Skip if an explicit input redirect
		 * already claimed stdin this command (saved_in_fd >= 0). */
		if(pending_pipe_stdin_file != NULL && saved_in_fd < 0)
		{
			char* pif;
			int pif_fd;
			pif = pending_pipe_stdin_file;
			pending_pipe_stdin_file = NULL;
			pif_fd = open(pif, O_RDONLY, 0);
			if(pif_fd >= 0) redir_install(&hs_in_fd, &hs_in_path, &saved_in_fd, &saved_in_path, &opened_in, pif_fd, pif);
		}

		result = exec_builtin(argv, exp_argc);
		if(result == -1 && match(cmd, "command") && exp_argc >= 2)
		{
			/* `command NAME args...` skips function lookup. */
			skip_fn = TRUE;
			if(match(argv[1], "-v") || match(argv[1], "-p"))
			{
				/* exec_builtin already handled these. */
			}
			else
			{
				argv = argv_drop(argv, exp_argc, 1);
				exp_argc = exp_argc - 1;
				cmd = argv[0];
				result = exec_builtin(argv, exp_argc);
			}
		}
		if(result >= 0)
		{
			sh_flush(stdout);
			goto exec_simple_cleanup;
		}

		/* Try function */
		if(!skip_fn)
		{
			fi = fn_find(cmd);
			if(fi >= 0)
			{
				/* Save and set positional params */
				save_argc = pp_argc;
				save_argv = pp_argv;
				pp_argv = calloc(MAX_ARGV, sizeof(char*));
				pp_argv[0] = cmd;
				pp_argc = 1;
				i = 1;
				while(i < exp_argc)
				{
					pp_argv[pp_argc] = argv[i];
					pp_argc = pp_argc + 1;
					i = i + 1;
				}

				lsc_push();
				in_function = in_function + 1;

				/* Inline assignments become locals inside the function. */
				apply_assigns(raw_assigns, assign_count, TRUE);

				fn_return_flag = FALSE;
				body = fn_bodies[fi];
				if(body[0] == '\x02')
				{
					/* AST-form: body is "\x02<node-index>". */
					{
						int body_idx;
						body_idx = str_to_int(body + 1);
						if(body_idx >= nd_count || body_idx < 0)
						{
							sh_err_puts("hs: invalid fn body node ");
							sh_err_puts(body + 1);
							sh_err_puts(" (nd_count=");
							sh_err_puts(int_to_str(nd_count));
							sh_err_puts(")\n");
							result = 1;
						}
						else
						{
							result = exec_node(body_idx);
						}
					}
				}
				else
				{
					result = expand_and_exec(body);
				}
				fn_return_flag = FALSE;
				/* A function-call boundary consumes the &&/|| short-circuit
				   exemption: the call's own status is judged fresh by the
				   caller's set -e check, even if the body's last command was a
				   short-circuited `&&` operand. (Brace groups and loops, by
				   contrast, are transparent and keep propagating the flag.)
				   Matches POSIX: `set -e; f(){ false && true; }; f` exits. */
				g_andor_shortcircuit = FALSE;

				in_function = in_function - 1;
				lsc_pop();
				free(pp_argv);
				pp_argc = save_argc;
				pp_argv = save_argv;

				last_status = result;
				sh_flush(stdout);
				goto exec_simple_cleanup;
			}
		}

		/* External command. Resolve via PATH first so any
		 * "command not found" error respects the redirected stderr. */
		envp = var_build_envp();

		path = var_get("PATH");
		pathbuf = NULL;

		if(cmd[0] == '/' || cmd[0] == '.')
		{
			pathbuf = cmd;
		}
		else if(path[0] != 0)
		{
			/* Walk PATH for the first hit. */
			{
				char* p;
				char* trial;
				char* colon;
				char* mpath;

				mpath = str_dup(path);
				p = mpath;
				/* Nested ifs avoid M2-Planet miscompiling `&&` when the
				 * left operand is a non-{0,1} pointer cast. */
				while(p != NULL)
				{
					if(p[0] == 0) break;
					colon = strchr(p, ':');
					if(colon != NULL) colon[0] = 0;
					trial = calloc(strlen(p) + strlen(cmd) + 2, 1);
					strcpy(trial, p);
					strcat(trial, "/");
					strcat(trial, cmd);
					if(access(trial, 0) == 0) { pathbuf = trial; break; }
					free(trial);
					if(colon != NULL) p = colon + 1;
					else p = NULL;
				}
			}
		}

		if(pathbuf == NULL)
		{
			sh_err_puts("hs: command not found: ");
			sh_err_puts(cmd);
			sh_err_puts("\n");
			last_status = 127;
			goto exec_simple_cleanup;
		}

		/* Externals cannot honor a redirect or capture on the target kernels
		   (fd 0/1/2 are the console, and there is no dup2 -- KERNEL.md K1).
		   Rather than run the external with the redirect silently ineffective
		   (an empty `$(extprog)`, an empty `extprog >file`), refuse: a non-NULL
		   std-stream path means this external's stdin/stdout/stderr is bound to
		   a file, temp, or pipe it cannot reach -- a direct `>`/`<`/`2>`, a
		   `$(extprog)`/pipe capture, or an enclosing redirected group / prior
		   `exec >file`. Builtins and functions are unaffected (they reached this
		   point only by not being one). See README.md / KERNEL.md. */
		if(hs_in_path != NULL || hs_out_path != NULL || hs_err_path != NULL)
		{
			sh_err_puts("hs: ");
			sh_err_puts(cmd);
			sh_err_puts(": cannot redirect or capture an external command (unsupported)\n");
			last_status = 1;
			goto exec_simple_cleanup;
		}

		/* Restore parent stdio state before the external runs. Externals are
		 * never redirected (they inherit the real fds), so there is no child
		 * spec to apply -- the parent's hs_*_fd just go back to what they
		 * were. Keep opened_* alive until the shared cleanup. */
		if(saved_in_fd >= 0)  { hs_in_fd  = saved_in_fd;  hs_in_path  = saved_in_path;  saved_in_fd  = -1; }
		if(saved_out_fd >= 0) { hs_out_fd = saved_out_fd; hs_out_path = saved_out_path; saved_out_fd = -1; }
		if(saved_err_fd >= 0) { hs_err_fd = saved_err_fd; hs_err_path = saved_err_path; saved_err_fd = -1; }

		/* Flush hs's buffered stdout before the external runs: builtins
		 * write through a FILE* buffer, so without this their output would
		 * interleave out of order with the external's console output. */
		sh_flush(stdout);

		{
			int cap_held;
			/* When we are inside a builtin capture (stdout points at a writable
			   temp fd, e.g. a `$()` or pipe-left context), the parent must not
			   hold that fd open across the external's fork: some kernels'
			   continuation-fork crashes with a live writable temp fd
			   (KERNEL.md K1). Close it first, reopen-append after, so later
			   builtin output in the same context is still captured. The
			   external's own output goes to the console regardless. */
			cap_held = -1;
			if(hs_out_path != NULL && hs_out_fd >= 3)
			{
				cap_held = hs_out_fd;
				close(hs_out_fd);
			}

			last_status = exec_external(pathbuf, argv, envp);

			if(cap_held >= 0) hs_out_fd = open_append(hs_out_path);
		}

		/* Fall through to the shared cleanup (also the `goto` target for the
		   builtin / function / command-not-found paths). The external path
		   already restored saved_*_fd above (setting them to -1), so the
		   saved_*_fd restores below are no-ops here; only the opened_* close,
		   heredoc unlink, prefix-assignment restore and arena rewind run. */

exec_simple_cleanup:
		if(saved_in_fd  >= 0) { hs_in_fd  = saved_in_fd;  hs_in_path  = saved_in_path;  }
		if(saved_out_fd >= 0) { hs_out_fd = saved_out_fd; hs_out_path = saved_out_path; }
		if(saved_err_fd >= 0) { hs_err_fd = saved_err_fd; hs_err_path = saved_err_path; }
		if(opened_in  >= 0) close(opened_in);
		if(opened_out >= 0) close(opened_out);
		if(opened_err >= 0) close(opened_err);
		while(heredoc_ntmp > 0)
		{
			heredoc_ntmp = heredoc_ntmp - 1;
			unlink(heredoc_tmps[heredoc_ntmp]);
			free(heredoc_tmps[heredoc_ntmp]);
		}
		/* Restore variables shadowed by command-prefix assignments. */
		while(pre_n > 0)
		{
			pre_n = pre_n - 1;
			if(pre_had[pre_n])
			{
				int rvi;
				var_set(pre_names[pre_n], pre_old[pre_n]);
				rvi = var_find(pre_names[pre_n]);
				if(rvi >= 0) var_exported[rvi] = pre_exp[pre_n];
			}
			else
			{
				var_unset(pre_names[pre_n]);
			}
			free(pre_names[pre_n]);
			if(pre_old[pre_n] != NULL) free(pre_old[pre_n]);
		}
		tmp_cur = exec_save_tmp;
		return last_status;
	}
}

/* Execute a compound command carrying trailing redirects (CMD_REDIR,
 * e.g. `while read x; do ...; done < file`). The redirects are applied to
 * the shell-level stdio fds (hs_in_fd/out/err) for the duration of the
 * child, then restored -- so builtins like `read` inside the body see the
 * redirected stream. (Externals inside a redirected compound still
 * inherit the real fds; shpack's compound redirects feed `read`, which is
 * a builtin, so this is sufficient.) */
int exec_redir(int idx)
{
	int child;
	char** redirs;
	int rc;
	int i;
	char* rfile;
	int rfd;
	int fd;
	int result;
	int saved_in_fd;
	int saved_out_fd;
	int saved_err_fd;
	char* saved_in_path;
	char* saved_out_path;
	char* saved_err_path;
	int opened_in;
	int opened_out;
	int opened_err;
	char** heredoc_tmps;  /* heap, not a local array (M2-Planet) */
	int heredoc_ntmp;
	char* p;
	int redir_failed;
	char* redir_failed_path;

	child = nd_a[idx];
	redirs = nd_argv[idx];
	rc = nd_b[idx];

	heredoc_tmps = tmp_alloc(MAX_HEREDOC * sizeof(char*));
	saved_in_fd = -1; saved_out_fd = -1; saved_err_fd = -1;
	saved_in_path = NULL; saved_out_path = NULL; saved_err_path = NULL;
	opened_in = -1; opened_out = -1; opened_err = -1;
	heredoc_ntmp = 0;
	redir_failed = FALSE;
	redir_failed_path = NULL;

	/* Redirect targets are a single-word context (no field splitting), so an
	   unquoted $var / $(cmd) target keeps its whitespace verbatim instead of
	   getting a split-marker byte baked into the filename. */
	g_field_split = FALSE;
	i = 0;
	while(i < rc)
	{
		rfile = redirs[i] + 1; /* skip \x06 marker */
		rfd = -1;
		if(rfile[0] >= '0' && rfile[0] <= '9')
		{
			rfd = 0;
			while(rfile[0] >= '0' && rfile[0] <= '9')
			{
				rfd = rfd * 10 + (rfile[0] - '0');
				rfile = rfile + 1;
			}
		}

		if(rfile[0] == 'h' || rfile[0] == 'H')
		{
			char* htmp;

			fd = heredoc_body_to_fd(rfile, heredoc_tmps, &heredoc_ntmp, &htmp);
			if(fd >= 0) redir_install(&hs_in_fd, &hs_in_path, &saved_in_fd, &saved_in_path, &opened_in, fd, htmp);
		}
		else if(rfile[0] == '<' && rfile[1] == '&')
		{
			/* N<&M input dup (e.g. `<&0`, `exec 7<&0`): make fd rfd read where
			   fd M does. Mirrors the `>&` output dup below. */
			int srcm;
			int srcfd;
			char* srcpath;
			if(rfd < 0) rfd = 0;
			srcm = str_to_int(rfile + 2);
			if(srcm == 0) { srcfd = hs_in_fd; srcpath = hs_in_path; }
			else if(srcm == 1) { srcfd = hs_out_fd; srcpath = hs_out_path; }
			else if(srcm == 2) { srcfd = hs_err_fd; srcpath = hs_err_path; }
			else { srcfd = -1; srcpath = NULL; }
			if(srcfd >= 0)
			{
				if(rfd == 0)
				{
					redir_save(&hs_in_fd, &hs_in_path, &saved_in_fd, &saved_in_path);
					hs_in_fd = srcfd; hs_in_path = srcpath;
				}
				else if(rfd == 1)
				{
					redir_save(&hs_out_fd, &hs_out_path, &saved_out_fd, &saved_out_path);
					hs_out_fd = srcfd; hs_out_path = srcpath;
				}
				else if(rfd == 2)
				{
					redir_save(&hs_err_fd, &hs_err_path, &saved_err_fd, &saved_err_path);
					hs_err_fd = srcfd; hs_err_path = srcpath;
				}
			}
		}
		else if(rfile[0] == '<')
		{
			p = strip_quotes(expand_word(rfile + 1));
			fd = open(p, O_RDONLY, 0);
			if(fd >= 0) redir_install(&hs_in_fd, &hs_in_path, &saved_in_fd, &saved_in_path, &opened_in, fd, p);
			else { redir_failed = TRUE; redir_failed_path = p; }
		}
		else if(rfile[0] == '>' && rfile[1] == '>')
		{
			if(rfd < 0) rfd = 1;
			p = strip_quotes(expand_word(rfile + 2));
			fd = open_append(p);
			if(fd >= 0)
			{
				if(rfd == 2) redir_install(&hs_err_fd, &hs_err_path, &saved_err_fd, &saved_err_path, &opened_err, fd, p);
				else redir_install(&hs_out_fd, &hs_out_path, &saved_out_fd, &saved_out_path, &opened_out, fd, p);
			}
			else { redir_failed = TRUE; redir_failed_path = p; }
		}
		else if(rfile[0] == '>' && rfile[1] == '&')
		{
			/* N>&M dup (e.g. `2>&1`): make fd rfd point where fd M does. */
			int srcm;
			int srcfd;
			char* srcpath;
			if(rfd < 0) rfd = 1;
			srcm = str_to_int(rfile + 2);
			if(srcm == 1) { srcfd = hs_out_fd; srcpath = hs_out_path; }
			else if(srcm == 2) { srcfd = hs_err_fd; srcpath = hs_err_path; }
			else if(srcm == 0) { srcfd = hs_in_fd; srcpath = hs_in_path; }
			else { srcfd = -1; srcpath = NULL; }
			if(srcfd >= 0)
			{
				if(rfd == 2)
				{
					redir_save(&hs_err_fd, &hs_err_path, &saved_err_fd, &saved_err_path);
					hs_err_fd = srcfd; hs_err_path = srcpath;
				}
				else if(rfd == 1)
				{
					redir_save(&hs_out_fd, &hs_out_path, &saved_out_fd, &saved_out_path);
					hs_out_fd = srcfd; hs_out_path = srcpath;
				}
			}
		}
		else if(rfile[0] == '>')
		{
			if(rfd < 0) rfd = 1;
			p = strip_quotes(expand_word(rfile + 1));
			fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 420);
			if(fd >= 0)
			{
				if(rfd == 2) redir_install(&hs_err_fd, &hs_err_path, &saved_err_fd, &saved_err_path, &opened_err, fd, p);
				else redir_install(&hs_out_fd, &hs_out_path, &saved_out_fd, &saved_out_path, &opened_out, fd, p);
			}
			else { redir_failed = TRUE; redir_failed_path = p; }
		}
		i = i + 1;
	}
	g_field_split = TRUE;

	/* A redirect that couldn't be opened fails the compound without running
	   its body (POSIX), rather than silently leaving the fd inherited. */
	if(redir_failed)
	{
		sh_err_puts("hs: cannot open redirect target: ");
		sh_err_puts(redir_failed_path);
		sh_err_puts("\n");
		result = 1;
		/* Skipping exec_node means the body never runs to set $?, so publish
		   the failure status ourselves (the success path lets the child do it). */
		last_status = 1;
	}
	else
	{
		result = exec_node(child);
	}
	sh_flush(stdout);

	if(saved_in_fd  >= 0) { hs_in_fd  = saved_in_fd;  hs_in_path  = saved_in_path;  }
	if(saved_out_fd >= 0) { hs_out_fd = saved_out_fd; hs_out_path = saved_out_path; }
	if(saved_err_fd >= 0) { hs_err_fd = saved_err_fd; hs_err_path = saved_err_path; }
	if(opened_in  >= 0) close(opened_in);
	if(opened_out >= 0) close(opened_out);
	if(opened_err >= 0) close(opened_err);
	while(heredoc_ntmp > 0)
	{
		heredoc_ntmp = heredoc_ntmp - 1;
		unlink(heredoc_tmps[heredoc_ntmp]);
		free(heredoc_tmps[heredoc_ntmp]);
	}
	return result;
}

/* Run a subshell body -- a `( )` group, a `$( )`/backtick capture, or a pipe
   stage -- IN THE SAME PROCESS, isolating its side effects, instead of forking.
   The builder-hex0-arch kernels corrupt a parent that forks and exits without
   execve (KERNEL.md K1), so hs cannot use fork-without-exec for subshells.

   Exactly one of node_idx (>= 0) or expr (non-NULL) selects the body. If
   out_fd >= 0 the body's stdout is redirected to it (the capture temp file),
   with out_path tracked for `2>&1` path-reopen.

   Isolation, restored on return: variables (in_subshell routes every var_set
   through lsc, lsc_pop rewinds them), the fd/path redirection state, the working
   directory (getcwd/chdir), field splitting, and the EXIT trap (reset in a
   subshell). An `exit` inside the body sets g_subshell_exit, which the exec
   loops treat like a return and which we convert here into the body's status. */
int run_isolated(int node_idx, char* expr, int out_fd, char* out_path)
{
	int result;
	int s_out_fd; char* s_out_path;
	int s_in_fd;  char* s_in_path;
	int s_err_fd; char* s_err_path;
	char* s_trap;
	int s_field_split;
	int s_sub_exit;
	int s_sub_status;
	int s_errexit;
	int s_nounset;
	int s_suppress;
	int s_andor;
	char* s_cwd;

	sh_flush(stdout);

	s_out_fd = hs_out_fd; s_out_path = hs_out_path;
	s_in_fd = hs_in_fd;   s_in_path = hs_in_path;
	s_err_fd = hs_err_fd; s_err_path = hs_err_path;
	s_trap = trap_exit_action;
	s_field_split = g_field_split;
	s_sub_exit = g_subshell_exit;
	s_sub_status = g_subshell_exit_status;
	/* `set` options are inherited by the subshell, but its changes must not leak
	   back (POSIX: `(set -e)` does not turn on errexit for the parent). The
	   errexit-suppression / short-circuit state is per-construct, so the body
	   starts those fresh. */
	s_errexit = flag_errexit;
	s_nounset = flag_nounset;
	s_suppress = suppress_errexit;
	s_andor = g_andor_shortcircuit;
	s_cwd = getcwd(calloc(4096, 1), 4096);

	lsc_push();
	in_subshell = in_subshell + 1;
	subshell_depth = subshell_depth + 1;
	trap_exit_action = NULL;
	g_subshell_exit = FALSE;
	suppress_errexit = 0;
	g_andor_shortcircuit = FALSE;

	if(out_fd >= 0) { hs_out_fd = out_fd; hs_out_path = out_path; g_field_split = TRUE; }

	if(expr != NULL) result = expand_and_exec(expr);
	else result = exec_node(node_idx);
	sh_flush(stdout);

	if(g_subshell_exit) result = g_subshell_exit_status;

	subshell_depth = subshell_depth - 1;
	in_subshell = in_subshell - 1;
	lsc_pop();

	if(s_cwd != NULL) chdir(s_cwd);
	hs_out_fd = s_out_fd; hs_out_path = s_out_path;
	hs_in_fd = s_in_fd;   hs_in_path = s_in_path;
	hs_err_fd = s_err_fd; hs_err_path = s_err_path;
	trap_exit_action = s_trap;
	g_field_split = s_field_split;
	g_subshell_exit = s_sub_exit;
	g_subshell_exit_status = s_sub_status;
	flag_errexit = s_errexit;
	flag_nounset = s_nounset;
	suppress_errexit = s_suppress;
	g_andor_shortcircuit = s_andor;

	last_status = result;
	return result;
}

int exec_node(int idx)
{
	int type;
	int result;
	int save_argc;
	char** save_argv;
	char* word_raw;
	char* expanded;
	char* stripped;
	char** word_list;
	int wli;
	int wi;
	int wstart;
	int wc;
	int item;
	char* case_val;
	char* pat;

	if(idx < 0) return 0;
	if(idx >= MAX_ND)
	{
		sh_err_puts("hs: node index out of range: ");
		sh_err_puts(int_to_str(idx));
		sh_err_puts("\n");
		return 1;
	}
	if(fn_return_flag || g_subshell_exit) return last_status;
	if(loop_break_level > 0) return last_status;

	type = nd_type[idx];

	/* Default: this result is not an &&/|| short-circuit. CMD_AND re-sets it
	   when its left operand fails; compounds/lists leave it reflecting their
	   last executed command, so the exemption propagates outward. */
	g_andor_shortcircuit = FALSE;

	if(type == CMD_SIMPLE) return exec_simple(idx);

	if(type == CMD_REDIR) return exec_redir(idx);

	if(type == CMD_LIST)
	{
		result = exec_node(nd_a[idx]);
		if(fn_return_flag || g_subshell_exit || loop_break_level > 0 || loop_continue_flag) return result;
		/* A failed non-final `&&`/`||` operand is exempt (g_andor_shortcircuit). */
		if(flag_errexit && !suppress_errexit && result != 0 && !g_andor_shortcircuit)
		{
			if(subshell_depth > 0)
			{
				g_subshell_exit = TRUE;
				g_subshell_exit_status = result;
				return result;
			}
			exit(result);
		}
		result = exec_node(nd_b[idx]);
		return result;
	}

	if(type == CMD_AND)
	{
		suppress_errexit = suppress_errexit + 1;
		result = exec_node(nd_a[idx]);
		suppress_errexit = suppress_errexit - 1;
		if(result == 0)
		{
			result = exec_node(nd_b[idx]);
		}
		else
		{
			/* Left operand failed -> short-circuit. Its nonzero status is
			   exempt from set -e (it is a non-final && operand). */
			g_andor_shortcircuit = TRUE;
		}
		last_status = result;
		return result;
	}

	if(type == CMD_OR)
	{
		suppress_errexit = suppress_errexit + 1;
		result = exec_node(nd_a[idx]);
		suppress_errexit = suppress_errexit - 1;
		if(result != 0)
		{
			result = exec_node(nd_b[idx]);
		}
		last_status = result;
		return result;
	}

	if(type == CMD_NOT)
	{
		result = exec_node(nd_a[idx]);
		last_status = invert_status(result);
		return last_status;
	}

	if(type == CMD_PIPE)
	{
		/* Capture the left side to a tmp file, then make that file the
		 * stdin of the WHOLE right side. Earlier this was passed via
		 * pending_pipe_stdin_file (consumed by the first simple command);
		 * that broke `cmd | while read; done`, where only the first read
		 * saw the pipe and later reads fell through to the real stdin.
		 * Setting hs_in_fd/hs_in_path for the right side's duration makes
		 * every read inside it (builtin or, via inheritance, external)
		 * consume the pipe. */
		{
			char* pp_tmpfile;
			int pp_fd;
			int pp_saved_in_fd;
			char* pp_saved_in_path;
			char* pp_saved_pending;

			pp_tmpfile = capture_node_to_tmpfile(nd_a[idx]);

			pp_saved_in_fd = hs_in_fd;
			pp_saved_in_path = hs_in_path;
			pp_saved_pending = pending_pipe_stdin_file;
			pending_pipe_stdin_file = NULL;
			pp_fd = open(pp_tmpfile, O_RDONLY, 0);
			if(pp_fd >= 0)
			{
				hs_in_fd = pp_fd;
				hs_in_path = pp_tmpfile;
			}

			result = exec_node(nd_b[idx]);

			hs_in_fd = pp_saved_in_fd;
			hs_in_path = pp_saved_in_path;
			pending_pipe_stdin_file = pp_saved_pending;
			if(pp_fd >= 0) close(pp_fd);

			unlink(pp_tmpfile);
			free(pp_tmpfile);
			last_status = result;
			return result;
		}
	}

	if(type == CMD_IF)
	{
		suppress_errexit = suppress_errexit + 1;
		result = exec_node(nd_a[idx]);
		suppress_errexit = suppress_errexit - 1;
		if(result == 0)
		{
			result = exec_node(nd_b[idx]);
		}
		else if(nd_c[idx] >= 0)
		{
			result = exec_node(nd_c[idx]);
		}
		else
		{
			result = 0;
		}
		last_status = result;
		return result;
	}

	if(type == CMD_WHILE)
	{
		int body_status;
		body_status = 0;
		while(TRUE)
		{
			suppress_errexit = suppress_errexit + 1;
			result = exec_node(nd_a[idx]);
			suppress_errexit = suppress_errexit - 1;
			if(nd_c[idx]) result = !result; /* until inverts the condition. */
			if(result != 0) break;
			body_status = exec_node(nd_b[idx]);
			result = body_status;
			if(fn_return_flag || g_subshell_exit) break;
			if(loop_break_level > 0)
			{
				loop_break_level = loop_break_level - 1;
				break;
			}
			if(loop_continue_flag)
			{
				loop_continue_flag = FALSE;
			}
		}
		/* POSIX: status is the last body command, or 0 if never ran. */
		last_status = body_status;
		return body_status;
	}

	if(type == CMD_FOR)
	{
		word_raw = nd_str[nd_b[idx]];
		expanded = expand_word(word_raw);

		/* Split on Q_SPLIT markers from the expansion. */
		word_list = tmp_alloc(MAX_ARGV * sizeof(char*));
		wli = 0;
		wi = 0;
		wstart = 0;
		while(TRUE)
		{
			wc = expanded[wi];
			if(wc == Q_SPLIT || wc == 0)
			{
				if(wi > wstart)
				{
					stripped = tmp_alloc(wi - wstart + 1);
					memcpy(stripped, expanded + wstart, wi - wstart);
					stripped[wi - wstart] = 0;
					wli = emit_field(word_list, wli, stripped);
				}
				if(wc == 0) break;
				wstart = wi + 1;
				wi = wi + 1;
			}
			else
			{
				wi = wi + 1;
			}
		}

		result = 0;
		wi = 0;
		while(wi < wli)
		{
			var_set(nd_str[idx], word_list[wi]);
			result = exec_node(nd_a[idx]);
			if(fn_return_flag || g_subshell_exit) break;
			if(loop_break_level > 0)
			{
				loop_break_level = loop_break_level - 1;
				break;
			}
			if(loop_continue_flag)
			{
				loop_continue_flag = FALSE;
			}
			wi = wi + 1;
		}
		last_status = result;
		return result;
	}

	if(type == CMD_CASE)
	{
		int case_save_tmp;
		case_save_tmp = tmp_cur;
		/* The case subject is not field-split. */
		g_field_split = FALSE;
		case_val = strip_quotes(expand_word(nd_str[idx]));
		g_field_split = TRUE;
		item = nd_a[idx];
		result = 0;
		while(item >= 0)
		{
			pat = expand_word(nd_str[item]);
			if(case_match(pat, case_val))
			{
				tmp_cur = case_save_tmp;
				result = exec_node(nd_a[item]);
				break;
			}
			item = nd_b[item];
		}
		tmp_cur = case_save_tmp;
		last_status = result;
		return result;
	}

	if(type == CMD_FUNC)
	{
		if(nd_c[idx])
		{
			/* Persistent body: store as "\x02<ast-node-index>".
			   Build the marker first, then hand it to fn_set/
			   fn_replace_body so the previous body (if any) is
			   freed cleanly instead of leaked. */
			char* marker;
			int fn_idx2;
			marker = calloc(32, 1);
			marker[0] = '\x02';
			strcpy(marker + 1, int_to_str(nd_a[idx]));
			fn_idx2 = fn_find(nd_str[idx]);
			if(fn_idx2 >= 0)
			{
				/* Existing slot: free old body, install marker
				   directly without going through str_dup so the
				   AST-form invariant (body[0] == '\x02') holds. */
				if(fn_bodies[fn_idx2] != NULL) free(fn_bodies[fn_idx2]);
				fn_bodies[fn_idx2] = marker;
			}
			else
			{
				require(fn_count < MAX_FN, "fn_set: too many functions\n");
				fn_names[fn_count] = str_dup(nd_str[idx]);
				fn_bodies[fn_count] = marker;
				fn_count = fn_count + 1;
			}
		}
		/* Non-persistent bodies were stored by parse_simple via fn_set. */
		last_status = 0;
		return 0;
	}

	if(type == CMD_SUBSH)
	{
		/* A subshell `( ... )` runs in-process via run_isolated so its state
		   changes -- variable assignments, `cd`, `set`, redirections, the EXIT
		   trap, and an inner `exit` -- stay isolated from the parent, without a
		   fork-without-exec the bootstrap kernels can't survive (KERNEL.md K1).
		   (configure's PATH_SEPARATOR probe, `(PATH='/bin;/bin'; ...)`, depends
		   on this; without it the bogus PATH leaks and every later tool lookup
		   fails.) */
		last_status = run_isolated(nd_a[idx], NULL, -1, NULL);
		return last_status;
	}

	sh_err_puts("hs: unknown node type\n");
	return 1;
}

int main(int argc, char** argv, char** envp)
{
	char* script;
	int i;
	char* name;
	char* val;
	char* eq;

	tmp_init();
	perm_init(8388608);

	/* Seed the tracked file-creation mask to a conventional 022 and apply it,
	   so the value `umask` reports is truthful on the POSIX host too (the
	   minimal kernels have no umask syscall to read back -- see KERNEL.md).
	   M2-Planet has no octal literals: 18 == 022. */
	g_umask = 18;
	umask(g_umask);

	lex_stack_src = calloc(LEX_STACK_MAX, sizeof(char*));
	lex_stack_pos = calloc(LEX_STACK_MAX, sizeof(int));
	lex_stack_len = calloc(LEX_STACK_MAX, sizeof(int));
	lex_stack_cmd = calloc(LEX_STACK_MAX, sizeof(int));
	hd_tok      = calloc(MAX_HEREDOC, sizeof(int));
	hd_strip    = calloc(MAX_HEREDOC, sizeof(int));
	hd_expand   = calloc(MAX_HEREDOC, sizeof(int));
	hd_delim    = calloc(MAX_HEREDOC, sizeof(char*));
	hd_delim_buf = calloc(MAX_HEREDOC_BUF, 1);
	fce_hd_dp   = calloc(MAX_HEREDOC, sizeof(char*));
	fce_hd_s    = calloc(MAX_HEREDOC, sizeof(int));
	fce_hd_d    = calloc(MAX_HEREDOC_BUF, 1);
	var_init();
	fn_init();
	al_init();
	lsc_init();
	tok_init();
	nd_init();

	flag_errexit = FALSE;
	suppress_errexit = 0;
	g_andor_shortcircuit = FALSE;
	flag_nounset = FALSE;
	g_field_split = TRUE;
	fn_return_flag = FALSE;
	parsing_persistent = FALSE;
	loop_break_level = 0;
	loop_continue_flag = FALSE;
	in_function = 0;
	in_subshell = 0;
	pending_pipe_stdin_file = NULL;
	trap_exit_action = NULL;
	tmp_counter = 0;
	hs_in_fd = 0;
	hs_out_fd = 1;
	hs_err_fd = 2;
	hs_in_path = NULL;
	hs_out_path = NULL;
	hs_err_path = NULL;
	global_envp = envp;

	/* Import environment into the var table. */
	i = 0;
	while(envp[i] != NULL)
	{
		eq = strchr(envp[i], '=');
		if(eq != NULL)
		{
			name = calloc(eq - envp[i] + 1, 1);
			memcpy(name, envp[i], eq - envp[i]);
			name[eq - envp[i]] = 0;
			var_set(name, eq + 1);
			var_export(name);
			free(name);
		}
		i = i + 1;
	}

	pp_argv = calloc(MAX_ARGV, sizeof(char*));
	pp_argc = 0;

	if(argc < 2)
	{
		sh_err_puts("Usage: hs <script> [args...]\n");
		sh_err_puts("       hs -c <command> [name [args...]]\n");
		exit(1);
	}

	{
		char* cwd;
		cwd = getcwd(calloc(4096, 1), 4096);
		if(cwd != NULL) var_set("PWD", cwd);
	}

	/* `hs -c <command> [name [args...]]`: run the command string instead
	 * of a script file. POSIX puts the command name in $0 (argv[3] if
	 * given, else the shell name) and the remaining operands in $1.. .
	 * This is the form ./configure and make recipes use via /bin/sh -c. */
	if(match(argv[1], "-c"))
	{
		char* cmd_str;
		if(argc < 3)
		{
			sh_err_puts("hs: -c requires an argument\n");
			exit(2);
		}
		cmd_str = argv[2];
		if(argc > 3) pp_argv[0] = argv[3];
		else pp_argv[0] = "hs";
		pp_argc = 1;
		i = 4;
		while(i < argc)
		{
			pp_argv[pp_argc] = argv[i];
			pp_argc = pp_argc + 1;
			i = i + 1;
		}
		last_status = exec_buffer(cmd_str, strlen(cmd_str), NULL);
		run_exit_trap();
		sh_flush(stdout);
		return last_status;
	}

	/* $0 is the script path; $1.. follow. */
	pp_argv[0] = argv[1];
	pp_argc = 1;
	i = 2;
	while(i < argc)
	{
		pp_argv[pp_argc] = argv[i];
		pp_argc = pp_argc + 1;
		i = i + 1;
	}

	script = argv[1];
	last_status = exec_source(script);

	run_exit_trap();
	sh_flush(stdout);
	return last_status;
}
