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
#include <sys/stat.h>
#ifndef __M2__
#include <sys/wait.h>
#endif
#include "bootstrappable.h"

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

/* Linux x86_64 open() flag not in M2libc's fcntl.c. We use it to
   refuse symlink resolution on temp-file opens, defeating the
   classic /tmp symlink-attack against predictable filenames. */
#define HS_O_NOFOLLOW 0400000

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

/* Quoting sentinel bytes - using high values to avoid M2-Planet byte-handling edge cases */
#define Q_SQ_OPEN  '\x01'
#define Q_SQ_CLOSE '\x02'
#define Q_DQ_OPEN  '\x03'
#define Q_DQ_CLOSE '\x04'
#define Q_SPLIT    '\x05'

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

void tmp_init()
{
	tmp_size = TMP_SIZE;
	tmp_base = calloc(tmp_size, 1);
	require(tmp_base != NULL, "tmp_init: calloc failed\n");
	tmp_cur = 0;
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

/* Copy src into dst. At most size-1 bytes are written; dst is always
   NUL-terminated when size > 0. Returns the number of bytes written
   (excluding the NUL) on success, or -1 if src didn't fit. */
int str_cpy_safe(char* dst, char* src, int size)
{
	int i;
	if(size <= 0) return -1;
	i = 0;
	while(src[i] != 0)
	{
		if(i + 1 >= size)
		{
			dst[size - 1] = 0;
			return -1;
		}
		dst[i] = src[i];
		i = i + 1;
	}
	dst[i] = 0;
	return i;
}

/* Append src onto dst. Returns the new dst length on success, -1 on
   truncation. dst must be NUL-terminated within the first size bytes. */
int str_cat_safe(char* dst, char* src, int size)
{
	int dlen;
	int i;
	if(size <= 0) return -1;
	dlen = 0;
	while(dlen < size && dst[dlen] != 0) dlen = dlen + 1;
	if(dlen >= size) return -1;
	i = 0;
	while(src[i] != 0)
	{
		if(dlen + i + 1 >= size)
		{
			dst[size - 1] = 0;
			return -1;
		}
		dst[dlen + i] = src[i];
		i = i + 1;
	}
	dst[dlen + i] = 0;
	return dlen + i;
}

/* Append a single byte to a buffer that the caller is filling
   sequentially. *pos is advanced on success. Returns TRUE on success,
   FALSE if there is no room. The caller is responsible for the
   trailing NUL. This helper just guards against writing past the
   end. */
int buf_putc(char* dst, int* pos, int size, int c)
{
	int p;
	p = pos[0];
	if(p + 1 >= size) return FALSE;
	dst[p] = c;
	pos[0] = p + 1;
	return TRUE;
}

/* Append a single byte to the expand_word output buffer. Bails via
   require() on overflow rather than silently corrupting memory.
   Reserves one byte for the trailing NUL the caller writes. */
int out_put(char* out, int oi, int outsize, int c)
{
	require(oi + 1 < outsize, "hs: expansion overflow\n");
	out[oi] = c;
	return oi + 1;
}

/* Append a NUL-terminated string. Returns new oi. Bails on overflow. */
int out_puts(char* out, int oi, int outsize, char* s)
{
	int n;
	n = strlen(s);
	require(oi + n + 1 <= outsize, "hs: expansion overflow\n");
	memcpy(out + oi, s, n);
	return oi + n;
}

/* String helpers. */
int _dbg_calloc_count;
int _dbg_calloc_bytes;

char* str_dup(char* s)
{
	int len;
	char* r;
	if(s == NULL) return NULL;
	len = strlen(s);
	r = calloc(len + 1, 1);
	_dbg_calloc_count = _dbg_calloc_count + 1;
	_dbg_calloc_bytes = _dbg_calloc_bytes + len + 1;
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

/* Redirection spec kinds passed from exec_simple to exec_external_redir.
 *   REDIR_NONE    inherit the parent's fd, no action.
 *   REDIR_PATH    close(N) + open(path, mode) in the child.
 *   REDIR_CAPTURE same as PATH, plus a post-waitpid replay from the
 *                 tmp file to replay_fd in the parent. */
#define REDIR_NONE    0
#define REDIR_PATH    1
#define REDIR_CAPTURE 2

/* External command execution. */
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
	return (status >> 8) & 255;
}

/* External command with child-process redirections. The child uses
 * close(N)+open() to land a fresh fd at slot N (since open() returns
 * the lowest unused fd). For REDIR_CAPTURE slots the parent also
 * reads the tmp file back and writes it to replay_fd[i] after
 * waitpid. Co-captured slots share a single path allocation, marked
 * by shares_path[i]; replay and free happen only on the owning slot. */
int exec_external_redir(char* path, char** argv, char** envp,
                        int* kind, char** paths, int* modes,
                        int* replay_fd, int* shares_path)
{
	int child;
	int status;
	int fd;
	int i;
	child = fork();
	if(child == 0)
	{
		i = 0;
		while(i < 3)
		{
			if(kind[i] != REDIR_NONE)
			{
				close(i);
				fd = open(paths[i], modes[i], 420);
				if(fd < 0) _exit(1);
			}
			i = i + 1;
		}
		execve(path, argv, envp);
		_exit(127);
	}
	waitpid(child, &status, 0);

	/* Replay captured slots to their replay target. */
	i = 0;
	while(i < 3)
	{
		if(kind[i] == REDIR_CAPTURE && shares_path[i] == 0)
		{
			int rfd;
			int n;
			char buf[4096];
			rfd = open(paths[i], O_RDONLY, 0);
			if(rfd >= 0)
			{
				n = read(rfd, buf, 4096);
				while(n > 0)
				{
					write(replay_fd[i], buf, n);
					n = read(rfd, buf, 4096);
				}
				close(rfd);
			}
			unlink(paths[i]);
			free(paths[i]);
		}
		i = i + 1;
	}
	return (status >> 8) & 255;
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
int    flag_nounset;
int    flag_noglob;
int    fn_return_flag;
int    loop_break_level;
int    loop_continue_flag;
int    loop_depth;
int    in_function;
char*  cached_ifs;
char** global_envp;

/* Pipe left-side output: CMD_PIPE writes the left side to a tmp file
 * and stores the path here; exec_simple picks it up on the right side
 * as an implicit `< file` redirect. */
char*  pending_pipe_stdin_file;
int    tmp_counter;

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

/* Forward declarations */
int exec_node(int idx);
int exec_source(char* path);
void parse_init(char* input);
int parse_list();
int expand_and_exec(char* code);

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

char* capture_expr_to_tmpfile(char* expr)
{
	char* tmpfile;
	int child;
	int status;
	int fd;

	tmpfile = make_cap_tmpfile_path();
	child = fork();
	if(child == 0)
	{
		close(1);
		fd = open(tmpfile, O_WRONLY | O_TRUNC | HS_O_NOFOLLOW, 384);
		if(fd < 0) _exit(1);
		/* hs_out_path = tmpfile so a nested `2>&1` resolves via
		 * path-reopen. */
		hs_in_fd = 0;
		hs_out_fd = 1;
		hs_err_fd = 2;
		hs_in_path = NULL;
		hs_out_path = tmpfile;
		hs_err_path = NULL;
		expand_and_exec(expr);
		_exit(last_status);
	}
	waitpid(child, &status, 0);
	return tmpfile;
}

char* capture_node_to_tmpfile(int node_idx)
{
	char* tmpfile;
	int child;
	int status;
	int fd;

	tmpfile = make_cap_tmpfile_path();
	child = fork();
	if(child == 0)
	{
		close(1);
		fd = open(tmpfile, O_WRONLY | O_TRUNC | HS_O_NOFOLLOW, 384);
		if(fd < 0) _exit(1);
		hs_in_fd = 0;
		hs_out_fd = 1;
		hs_err_fd = 2;
		hs_in_path = NULL;
		hs_out_path = tmpfile;
		hs_err_path = NULL;
		exec_node(node_idx);
		_exit(last_status);
	}
	waitpid(child, &status, 0);
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

	i = var_find(name);
	if(i < 0)
	{
		if(check_nounset && flag_nounset)
		{
			sh_err_puts("hs: ");
			sh_err_puts(name);
			sh_err_puts(": unbound variable\n");
			if(flag_errexit) exit(1);
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
						while(lex_pos < lex_len && depth > 0)
						{
							c = lex_src[lex_pos];
							bi = lex_put(buf, bi, c);
							lex_pos = lex_pos + 1;
							if(c == '(') depth = depth + 1;
							else if(c == ')') depth = depth - 1;
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
				while(lex_pos < lex_len && depth > 0)
				{
					c = lex_src[lex_pos];
					bi = lex_put(buf, bi, c);
					lex_pos = lex_pos + 1;
					if(c == '(') depth = depth + 1;
					else if(c == ')') depth = depth - 1;
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
					if(c == '{') depth = depth + 1;
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
		else if(c == '<')
		{
			tok_add(T_LT, "<");
			lex_pos = lex_pos + 1;
			cmd_pos = FALSE;
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
				if(all_digits && lex_pos < lex_len)
				{
					c = lex_src[lex_pos];
					if(c == '>')
					{
						lex_pos = lex_pos + 1;
						if(lex_pos < lex_len && lex_src[lex_pos] == '>')
						{
							tok_add(T_WORD, word);
							tok_add(T_GTGT, ">>");
							lex_pos = lex_pos + 1;
							cmd_pos = FALSE;
							continue;
						}
						else if(lex_pos < lex_len && lex_src[lex_pos] == '&')
						{
							lex_pos = lex_pos + 1;
							tok_add(T_WORD, word);
							tok_add(T_GT, ">");
							tok_add(T_WORD, tmp_cat2("&", lex_word()));
							cmd_pos = FALSE;
							continue;
						}
						else
						{
							tok_add(T_WORD, word);
							tok_add(T_GT, ">");
							cmd_pos = FALSE;
							continue;
						}
					}
					else if(c == '<')
					{
						tok_add(T_WORD, word);
						tok_add(T_LT, "<");
						lex_pos = lex_pos + 1;
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
int parse_compound();
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
		if(t == T_SEMI || t == T_NL)
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

int parse_command()
{
	char* w;
	int t;
	int r;

	parse_depth = parse_depth + 1;
	require(parse_depth < MAX_DEPTH, "hs: nested commands too deep\n");

	t = tok_peek();
	w = tok_peek_val();

	if(match(w, "if")) r = parse_if();
	else if(match(w, "while") || match(w, "until")) r = parse_while();
	else if(match(w, "for")) r = parse_for();
	else if(match(w, "case")) r = parse_case();
	else if(match(w, "{")) r = parse_brace();
	else if(t == T_LPAREN) r = parse_subsh();
	else r = parse_simple();

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

	/* One pattern buffer reused across items. */
	pat_buf = tmp_alloc(2048);

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
			require(pbi + pvlen + 1 < 2048, "hs: case: pattern list too long\n");
			memcpy(pat_buf + pbi, pv, pvlen);
			pbi = pbi + pvlen;
			if(tok_peek() == T_PIPE)
			{
				tok_next();
				require(pbi + 1 < 2048, "hs: case: pattern list too long\n");
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
	argv = tmp_alloc(32 * sizeof(char*));
	assigns = tmp_alloc(16 * sizeof(char*));
	argc = 0;
	assign_count = 0;
	saw_word = FALSE;

	/* Redirects are stored as \x06-prefixed pseudo-args inside argv. */
	while(TRUE)
	{
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
			/* If the previous word is an fd number, attach to it. */
			tok_next();
			rfile = tok_next_val();
			if(argc > 0 && argv[argc - 1] != NULL && argv[argc - 1][0] >= '0' && argv[argc - 1][0] <= '9')
			{
				/* Fd-number redirect: N<file becomes \x06N<file */
				argv[argc - 1] = tmp_cat3("\x06", argv[argc - 1], tmp_cat2("<", rfile));
			}
			else
			{
				argv[argc] = tmp_cat2("\x06<", rfile);
				argc = argc + 1;
			}
		}
		else if(t == T_GT)
		{
			tok_next();
			rfile = tok_next_val();
			if(argc > 0 && argv[argc - 1] != NULL && argv[argc - 1][0] >= '0' && argv[argc - 1][0] <= '9')
			{
				argv[argc - 1] = tmp_cat3("\x06", argv[argc - 1], tmp_cat2(">", rfile));
			}
			else
			{
				argv[argc] = tmp_cat2("\x06>", rfile);
				argc = argc + 1;
			}
		}
		else if(t == T_GTGT)
		{
			tok_next();
			rfile = tok_next_val();
			if(argc > 0 && argv[argc - 1] != NULL && argv[argc - 1][0] >= '0' && argv[argc - 1][0] <= '9')
			{
				argv[argc - 1] = tmp_cat3("\x06", argv[argc - 1], tmp_cat2(">>", rfile));
			}
			else
			{
				argv[argc] = tmp_cat2("\x06>>", rfile);
				argc = argc + 1;
			}
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
			while(pat[0] != 0 && pat[0] != ']')
			{
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
			arith_pos = arith_pos + 1;
			left = left / arith_unary();
		}
		else if(c == '%')
		{
			arith_pos = arith_pos + 1;
			left = left % arith_unary();
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
			arith_pos = arith_pos + 2;
			left = bool_to_int(left && arith_bitor());
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
			arith_pos = arith_pos + 2;
			left = bool_to_int(left || arith_logand());
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
		a = arith_expr();
		arith_skip_space();
		if(arith_pos < arith_len && arith_src[arith_pos] == ':')
		{
			arith_pos = arith_pos + 1;
		}
		b = arith_expr();
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
	return arith_expr();
}

/* Expansion engine. */

/* Copy `val` into `out` at `oi`, inserting Q_SPLIT for IFS chars
 * when not inside quotes. Returns new oi. */
int ifs_copy(char* out, int oi, int outsize, char* val, int in_dq)
{
	int vi;
	int c;
	char* ifs;
	int is_ifs;
	int ji;

	if(in_dq)
	{
		/* No IFS splitting inside double quotes. */
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
	pi = 0;
	pd = 1;
	while(w[*pos] != 0 && pd > 0)
	{
		if(w[*pos] == '{') pd = pd + 1;
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
		if(c == Q_SQ_OPEN || c == Q_SQ_CLOSE || c == Q_DQ_OPEN || c == Q_DQ_CLOSE)
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
	outsize = wlen * 4 + 64;
	if(outsize < 512) outsize = 512;
	if(outsize > MAX_STR * 4) outsize = MAX_STR * 4;
	out = tmp_alloc(outsize);
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
			oi = out_put(out, oi, outsize, c);
			i = i + 1;
		}
		else if(c == Q_SQ_CLOSE && in_sq)
		{
			in_sq = FALSE;
			oi = out_put(out, oi, outsize, c);
			i = i + 1;
		}
		else if(c == Q_DQ_OPEN && !in_sq)
		{
			in_dq = TRUE;
			oi = out_put(out, oi, outsize, c);
			i = i + 1;
		}
		else if(c == Q_DQ_CLOSE && in_dq)
		{
			in_dq = FALSE;
			oi = out_put(out, oi, outsize, c);
			i = i + 1;
		}
		else if(in_sq)
		{
			/* No expansion inside single quotes. */
			oi = out_put(out, oi, outsize, c);
			i = i + 1;
		}
		else if(c == '$')
		{
			i = i + 1;
			if(w[i] == 0)
			{
				oi = out_put(out, oi, outsize, '$');
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
				oi = out_puts(out, oi, outsize, val);
			}
			else if(w[i] == '(')
			{
				i = i + 1;
				sub_expr = tmp_alloc(MAX_STR);
				si = 0;
				depth = 1;
				while(w[i] != 0 && depth > 0)
				{
					if(w[i] == '(') depth = depth + 1;
					else if(w[i] == ')') depth = depth - 1;
					if(depth > 0)
					{
						require(si + 1 < MAX_STR, "hs: command substitution too long\n");
						sub_expr[si] = w[i];
						si = si + 1;
					}
					i = i + 1;
				}
				sub_expr[si] = 0;

				/* Capture the substitution into a tmp file via fork+close+open,
				 * then splice its contents into the output. */
				{
					char* cs_tmpfile;
					int cs_fd;
					int cs_rd;
					int cs_len;
					char* cs_buf;

					cs_tmpfile = capture_expr_to_tmpfile(sub_expr);
					cs_buf = calloc(MAX_STR, 1);
					cs_len = 0;
					cs_fd = open(cs_tmpfile, O_RDONLY, 0);
					if(cs_fd >= 0)
					{
						while(cs_len < MAX_STR - 1)
						{
							cs_rd = read(cs_fd, cs_buf + cs_len, MAX_STR - 1 - cs_len);
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

					j = 0;
					while(j < cs_len)
					{
						oi = out_put(out, oi, outsize, cs_buf[j]);
						j = j + 1;
					}
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
					oi = out_puts(out, oi, outsize, val);
				}
				else
				{
					name = tmp_alloc(256);
					ni = 0;
					while(w[i] != 0 && w[i] != '}' && w[i] != '#' && w[i] != '%' && w[i] != ':' && w[i] != '+')
					{
						/* `-` only stops the name after the first char,
						 * so ${VAR-default} parses but `-foo` does not. */
						if(w[i] == '-' && ni > 0) break;
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
						oi = ifs_copy(out, oi, outsize, val, in_dq);
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
						oi = out_puts(out, oi, outsize, val);
					}
					else if(w[i] == '-')
					{
						/* ${VAR-default}: only used if unset. */
						i = i + 1;
						sub_expr = tmp_alloc(MAX_STR);
						extract_brace_pattern(w, &i, sub_expr, MAX_STR);
						j = var_find(name);
						if(j < 0)
						{
							val = expand_word(sub_expr);
							val = strip_quotes(val);
						}
						else
						{
							val = var_vals[j];
						}
						oi = out_puts(out, oi, outsize, val);
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
						oi = out_puts(out, oi, outsize, val);
						/* sub_expr from tmp arena */
					}
					else if(w[i] == '+')
					{
						/* ${VAR+alt}: alt if VAR is set, empty otherwise. */
						i = i + 1;
						sub_expr = tmp_alloc(MAX_STR);
						extract_brace_pattern(w, &i, sub_expr, MAX_STR);
						j = var_find(name);
						if(j >= 0)
						{
							val = expand_word(sub_expr);
							val = strip_quotes(val);
						}
						else
						{
							val = "";
						}
						oi = out_puts(out, oi, outsize, val);
						/* sub_expr from tmp arena */
					}
					else if(w[i] == '#' && w[i+1] == '#')
					{
						/* ${VAR##pat}: longest matching prefix removal. */
						i = i + 2;
						pat = tmp_alloc(MAX_STR);
						extract_brace_pattern(w, &i, pat, MAX_STR);
						val = var_get_safe(name);
						pat = strip_quotes(expand_word(pat));
						vlen = strlen(val);
						sub_expr = tmp_alloc(vlen + 1);
						k = vlen;
						while(k >= 0)
						{
							memcpy(sub_expr, val, k);
							sub_expr[k] = 0;
							if(pat_match(pat, sub_expr))
							{
								val = val + k;
								break;
							}
							k = k - 1;
						}
						oi = out_puts(out, oi, outsize, val);
					}
					else if(w[i] == '#')
					{
						/* ${VAR#pat}: shortest matching prefix removal. */
						i = i + 1;
						pat = tmp_alloc(MAX_STR);
						extract_brace_pattern(w, &i, pat, MAX_STR);
						val = var_get_safe(name);
						pat = strip_quotes(expand_word(pat));
						vlen = strlen(val);
						sub_expr = tmp_alloc(vlen + 1);
						k = 0;
						while(k <= vlen)
						{
							memcpy(sub_expr, val, k);
							sub_expr[k] = 0;
							if(pat_match(pat, sub_expr))
							{
								val = val + k;
								break;
							}
							k = k + 1;
						}
						oi = out_puts(out, oi, outsize, val);
					}
					else if(w[i] == '%' && w[i+1] == '%')
					{
						/* ${VAR%%pat}: longest matching suffix removal. */
						i = i + 2;
						pat = tmp_alloc(MAX_STR);
						extract_brace_pattern(w, &i, pat, MAX_STR);
						val = var_get_safe(name);
						pat = strip_quotes(expand_word(pat));
						vlen = strlen(val);
						sub_expr = tmp_alloc(vlen + 1);
						k = 0;
						while(k <= vlen)
						{
							if(pat_match(pat, val + k))
							{
								memcpy(sub_expr, val, k);
								sub_expr[k] = 0;
								val = sub_expr;
								break;
							}
							k = k + 1;
						}
						oi = out_puts(out, oi, outsize, val);
					}
					else if(w[i] == '%')
					{
						/* ${VAR%pat}: shortest matching suffix removal. */
						i = i + 1;
						pat = tmp_alloc(MAX_STR);
						extract_brace_pattern(w, &i, pat, MAX_STR);
						val = var_get_safe(name);
						pat = strip_quotes(expand_word(pat));
						vlen = strlen(val);
						sub_expr = tmp_alloc(vlen + 1);
						k = vlen;
						while(k >= 0)
						{
							if(pat_match(pat, val + k))
							{
								memcpy(sub_expr, val, k);
								sub_expr[k] = 0;
								val = sub_expr;
								break;
							}
							k = k - 1;
						}
						oi = out_puts(out, oi, outsize, val);
					}
					else
					{
						/* Unknown operator: output ${...} verbatim. */
						oi = out_put(out, oi, outsize, '$');
						oi = out_put(out, oi, outsize, '{');
						oi = out_puts(out, oi, outsize, name);
						{
							int ud;
							ud = 1;
							while(w[i] != 0 && ud > 0)
							{
								if(w[i] == '{') ud = ud + 1;
								else if(w[i] == '}') { ud = ud - 1; if(ud == 0) break; }
								oi = out_put(out, oi, outsize, w[i]);
								i = i + 1;
							}
						}
						if(w[i] == '}')
						{
							oi = out_put(out, oi, outsize, '}');
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
								oi = out_put(out, oi, outsize, cached_ifs[0]);
							}
						}
						else
						{
							oi = out_put(out, oi, outsize, Q_SPLIT);
						}
					}
					val = pp_argv[j];
					oi = out_puts(out, oi, outsize, val);
					j = j + 1;
				}
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
					oi = out_put(out, oi, outsize, '$');
				}
				else
				{
					val = var_get(name);
					oi = ifs_copy(out, oi, outsize, val, in_dq);
				}
			}
		}
		else
		{
			oi = out_put(out, oi, outsize, c);
			i = i + 1;
		}
	}
	require(oi < outsize, "hs: expansion overflow\n");
	out[oi] = 0;
	result = out;
	expand_depth = expand_depth - 1;
	return result;
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
					result[ri] = strip_quotes(stripped);
					ri = ri + 1;
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
	fi = 0;
	ai = 2;

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
			fi = fi + 1;
			c = fmt[fi];
			if(c == 's')
			{
				if(ai < argc)
				{
					sh_puts(argv[ai], stdout);
					ai = ai + 1;
				}
				fi = fi + 1;
			}
			else if(c == 'd')
			{
				if(ai < argc)
				{
					sh_puts(int_to_str(str_to_int(argv[ai])), stdout);
					ai = ai + 1;
				}
				fi = fi + 1;
			}
			else if(c == 'x')
			{
				if(ai < argc)
				{
					char* hex;
					int hi;
					hex = int2str(str_to_int(argv[ai]), 16, FALSE);
					hi = 0;
					while(hex[hi] != 0)
					{
						if(hex[hi] >= 'A' && hex[hi] <= 'F')
						{
							hex[hi] = hex[hi] + 32;
						}
						hi = hi + 1;
					}
					sh_puts(hex, stdout);
					ai = ai + 1;
				}
				fi = fi + 1;
			}
			else if(c == '%')
			{
				sh_putc('%', stdout);
				fi = fi + 1;
			}
			else if(c == 'c')
			{
				if(ai < argc && argv[ai][0] != 0)
				{
					sh_putc(argv[ai][0], stdout);
					ai = ai + 1;
				}
				fi = fi + 1;
			}
			else
			{
				sh_putc('%', stdout);
			}
		}
		else
		{
			sh_putc(c, stdout);
			fi = fi + 1;
		}
	}
	sh_flush(stdout);
	return 0;
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
		if(match(op, "-f")) return bool_to_status(access(argv[2], 0) == 0);
		if(match(op, "-d")) return bool_to_status(access(argv[2], 0) == 0);
		if(match(op, "-e")) return bool_to_status(access(argv[2], 0) == 0);
		if(match(op, "-r")) return bool_to_status(access(argv[2], 4) == 0);
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
		op = argv[2];
		if(match(op, "=")) return bool_to_status(match(argv[1], argv[3]));
		if(match(op, "!=")) return bool_to_status(!match(argv[1], argv[3]));
		if(match(op, "-eq")) return bool_to_status(str_to_int(argv[1]) == str_to_int(argv[3]));
		if(match(op, "-ne")) return bool_to_status(str_to_int(argv[1]) != str_to_int(argv[3]));
		if(match(op, "-lt")) return bool_to_status(str_to_int(argv[1]) < str_to_int(argv[3]));
		if(match(op, "-gt")) return bool_to_status(str_to_int(argv[1]) > str_to_int(argv[3]));
		if(match(op, "-le")) return bool_to_status(str_to_int(argv[1]) <= str_to_int(argv[3]));
		if(match(op, "-ge")) return bool_to_status(str_to_int(argv[1]) >= str_to_int(argv[3]));

		/* `test ! EXPR` form. */
		if(match(argv[1], "!"))
		{
			if(match(argv[2], "-z")) return invert_status(bool_to_status(strlen(argv[3]) == 0));
			if(match(argv[2], "-n")) return invert_status(bool_to_status(strlen(argv[3]) != 0));
			if(match(argv[2], "-f")) return invert_status(bool_to_status(access(argv[3], 0) == 0));
			return bool_to_status(strlen(argv[3]) == 0);
		}
	}

	if(argc >= 5)
	{
		if(match(argv[1], "!") && argc == 5)
		{
			op = argv[3];
			if(match(op, "=")) return invert_status(bool_to_status(match(argv[2], argv[4])));
			if(match(op, "!=")) return invert_status(bool_to_status(!match(argv[2], argv[4])));
			if(match(op, "-eq")) return invert_status(bool_to_status(str_to_int(argv[2]) == str_to_int(argv[4])));
			if(match(op, "-ne")) return invert_status(bool_to_status(str_to_int(argv[2]) != str_to_int(argv[4])));
			if(match(op, "-lt")) return invert_status(bool_to_status(str_to_int(argv[2]) < str_to_int(argv[4])));
			if(match(op, "-gt")) return invert_status(bool_to_status(str_to_int(argv[2]) > str_to_int(argv[4])));
			if(match(op, "-le")) return invert_status(bool_to_status(str_to_int(argv[2]) <= str_to_int(argv[4])));
			if(match(op, "-ge")) return invert_status(bool_to_status(str_to_int(argv[2]) >= str_to_int(argv[4])));
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

int builtin_read(char** argv, int argc)
{
	char* line;
	int li;
	int c;
	int rflag;
	int i;
	char* varname;
	char* ifs;
	int start;
	int vi;

	rflag = FALSE;
	varname = "REPLY";
	i = 1;

	while(i < argc)
	{
		if(match(argv[i], "-r"))
		{
			rflag = TRUE;
		}
		else
		{
			varname = argv[i];
		}
		i = i + 1;
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

	var_set(varname, line);
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
	char* kw;
	int kwi;

	i = start;
	in_sq = FALSE;
	in_dq = FALSE;
	kw_depth = 0;
	brace_depth = 0;

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
			while(i < total && buf[i] != '\n') i = i + 1;
		}
		else if(c == '\n')
		{
			if(kw_depth <= 0 && brace_depth <= 0)
			{
				return i + 1;
			}
			i = i + 1;
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
		/* `case` patterns use ) without matching (, so don't track parens. */
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

int exec_source(char* path)
{
	FILE* f;
	char* buf;
	int len;
	int cap;
	int c;
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
		if(len >= cap - 1)
		{
			cap = cap * 2;
			{
				char* nb;
				nb = calloc(cap, 1);
				memcpy(nb, buf, len);
				free(buf);
				buf = nb;
			}
		}
		buf[len] = c;
		len = len + 1;
	}
	buf[len] = 0;
	fclose(f);

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
			if(flag_errexit && !suppress_errexit && result != 0)
			{
				free(chunk);
				break;
			}
		}

		if(fn_return_flag) { free(chunk); break; }

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
		sh_flush(stdout);
		exit(i);
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
					else if(val[j] == 'f') flag_noglob = TRUE;
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
					else if(val[j] == 'f') flag_noglob = FALSE;
					j = j + 1;
				}
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
			   match(name, "cd") || match(name, "true") || match(name, "false") ||
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
			val = var_get("PATH");
			if(val[0] != 0)
			{
				last_status = 1;
				return 1;
			}
			last_status = 1;
			return 1;
		}
		if(argc >= 3 && match(argv[1], "-p"))
		{
			/* `command -p`: run with the default PATH (we just fall through). */
			argv = argv + 2;
			argc = argc - 2;
			return -1;
		}
		if(argc >= 2)
		{
			/* `command NAME args...`: bypass function lookup. */
			argv = argv + 1;
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
			val = strip_quotes(expand_word(val));
			if(is_local) { lsc_declare(name, val); }
			else { var_set(name, val); }
		}
		i = i + 1;
	}
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
	/* Child redirect spec for external commands */
	int* child_kind;
	char** child_paths;
	int* child_modes;
	int* child_replay_fd;
	int* child_shares_path;

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
			if(raw_argv[i] != NULL && raw_argv[i][0] == '\x06')
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
			/* Assignments-only command. */
			apply_assigns(raw_assigns, assign_count, FALSE);
			last_status = 0;
			tmp_cur = exec_save_tmp;
			return 0;
		}

		if(exp_argc == 0)
		{
			last_status = 0;
			tmp_cur = exec_save_tmp;
			return 0;
		}

		cmd = argv[0];
		skip_fn = FALSE;

		/* Apply redirects for builtins and collect the spec for any
		 * external child. Parent-side state is purely (int fd, char* path)
		 * per std stream; no FILE* manipulation. */
		saved_in_fd = -1;
		saved_out_fd = -1;
		saved_err_fd = -1;
		saved_in_path = NULL;
		saved_out_path = NULL;
		saved_err_path = NULL;
		opened_in = -1;
		opened_out = -1;
		opened_err = -1;
		child_kind = tmp_alloc(3 * sizeof(int));
		child_paths = tmp_alloc(3 * sizeof(char*));
		child_modes = tmp_alloc(3 * sizeof(int));
		child_replay_fd = tmp_alloc(3 * sizeof(int));
		child_shares_path = tmp_alloc(3 * sizeof(int));
		child_kind[0] = REDIR_NONE;
		child_kind[1] = REDIR_NONE;
		child_kind[2] = REDIR_NONE;
		child_paths[0] = NULL;
		child_paths[1] = NULL;
		child_paths[2] = NULL;
		child_modes[0] = 0;
		child_modes[1] = 0;
		child_modes[2] = 0;
		child_replay_fd[0] = -1;
		child_replay_fd[1] = -1;
		child_replay_fd[2] = -1;
		child_shares_path[0] = 0;
		child_shares_path[1] = 0;
		child_shares_path[2] = 0;
		if(rc > 0)
		{
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

				if(rfile[0] == '<')
				{
					rfile = strip_quotes(expand_word(rfile + 1));
					fd = open(rfile, O_RDONLY, 0);
					if(fd >= 0)
					{
						if(saved_in_fd < 0)
						{
							saved_in_fd = hs_in_fd;
							saved_in_path = hs_in_path;
						}
						if(opened_in >= 0) close(opened_in);
						opened_in = fd;
						hs_in_fd = fd;
						hs_in_path = rfile;
					}
					child_kind[0] = REDIR_PATH;
					child_paths[0] = rfile;
					child_modes[0] = O_RDONLY;
				}
				else if(rfile[0] == '>' && rfile[1] == '>')
				{
					if(rfd < 0) rfd = 1;
					rfile = strip_quotes(expand_word(rfile + 2));
					fd = open(rfile, O_WRONLY | O_CREAT | O_APPEND, 420);
					if(fd >= 0)
					{
						if(rfd == 1)
						{
							if(saved_out_fd < 0)
							{
								saved_out_fd = hs_out_fd;
								saved_out_path = hs_out_path;
							}
							if(opened_out >= 0) close(opened_out);
							opened_out = fd;
							hs_out_fd = fd;
							hs_out_path = rfile;
						}
						else if(rfd == 2)
						{
							if(saved_err_fd < 0)
							{
								saved_err_fd = hs_err_fd;
								saved_err_path = hs_err_path;
							}
							if(opened_err >= 0) close(opened_err);
							opened_err = fd;
							hs_err_fd = fd;
							hs_err_path = rfile;
						}
					}
					if(rfd >= 0 && rfd <= 2)
					{
						child_kind[rfd] = REDIR_PATH;
						child_paths[rfd] = rfile;
						child_modes[rfd] = O_WRONLY | O_CREAT | O_APPEND;
					}
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
							if(saved_out_fd < 0)
							{
								saved_out_fd = hs_out_fd;
								saved_out_path = hs_out_path;
							}
							hs_out_fd = src_fd;
							hs_out_path = src_path;
						}
						else if(rfd == 2)
						{
							if(saved_err_fd < 0)
							{
								saved_err_fd = hs_err_fd;
								saved_err_path = hs_err_path;
							}
							hs_err_fd = src_fd;
							hs_err_path = src_path;
						}

						/* Child-side spec, three cases. */
						if(rfd >= 0 && rfd <= 2)
						{
							if(src_path != NULL)
							{
								/* Path-reopen: source file is known. */
								child_kind[rfd] = REDIR_PATH;
								child_paths[rfd] = src_path;
								child_modes[rfd] = O_WRONLY | O_CREAT | O_APPEND;
							}
							else if(src_fd == rfd)
							{
								/* Inherit-unchanged: child's fd N already
								 * points to the right target. */
								child_kind[rfd] = REDIR_NONE;
							}
							else
							{
								/* Co-capture-and-replay. Both fds open the
								 * same tmp file with O_APPEND so the kernel
								 * preserves write ordering; parent reads
								 * the file back to src_fd after exec. */
								char* cap_path;
								cap_path = make_cap_tmpfile_path();
								child_kind[rfd] = REDIR_CAPTURE;
								child_paths[rfd] = cap_path;
								child_modes[rfd] = O_WRONLY | O_CREAT | O_TRUNC | O_APPEND;
								child_replay_fd[rfd] = src_fd;

								/* Co-capture the source slot too, if it
								 * was still inherited and shares dest. */
								if(src_m >= 0 && src_m <= 2)
								{
									if(child_kind[src_m] == REDIR_NONE)
									{
										int src_m_fd;
										if(src_m == 1) src_m_fd = hs_out_fd;
										else src_m_fd = hs_err_fd;
										if(src_m_fd == src_fd)
										{
											child_kind[src_m] = REDIR_CAPTURE;
											child_paths[src_m] = cap_path;
											child_modes[src_m] = O_WRONLY | O_CREAT | O_TRUNC | O_APPEND;
											child_replay_fd[src_m] = src_fd;
											child_shares_path[src_m] = 1;
										}
									}
								}
							}
						}
					}
					else
					{
						fd = open(rfile, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 420);
						if(fd >= 0)
						{
							if(rfd == 1)
							{
								if(saved_out_fd < 0)
								{
									saved_out_fd = hs_out_fd;
									saved_out_path = hs_out_path;
								}
								if(opened_out >= 0) close(opened_out);
								opened_out = fd;
								hs_out_fd = fd;
								hs_out_path = rfile;
							}
							else if(rfd == 2)
							{
								if(saved_err_fd < 0)
								{
									saved_err_fd = hs_err_fd;
									saved_err_path = hs_err_path;
								}
								if(opened_err >= 0) close(opened_err);
								opened_err = fd;
								hs_err_fd = fd;
								hs_err_path = rfile;
							}
						}
						if(rfd >= 0 && rfd <= 2)
						{
							child_kind[rfd] = REDIR_PATH;
							child_paths[rfd] = rfile;
							child_modes[rfd] = O_WRONLY | O_CREAT | O_TRUNC | O_APPEND;
						}
					}
				}
				i = i + 1;
			}
		}

		/* Pipe right-side stdin: CMD_PIPE staged a tmp file path here
		 * for us to consume as if it were `< file`. */
		if(pending_pipe_stdin_file != NULL)
		{
			if(child_kind[0] == REDIR_NONE)
			{
				char* pif;
				int pif_fd;
				pif = pending_pipe_stdin_file;
				pending_pipe_stdin_file = NULL;
				pif_fd = open(pif, O_RDONLY, 0);
				if(pif_fd >= 0)
				{
					if(saved_in_fd < 0)
					{
						saved_in_fd = hs_in_fd;
						saved_in_path = hs_in_path;
					}
					if(opened_in >= 0) close(opened_in);
					opened_in = pif_fd;
					hs_in_fd = pif_fd;
					hs_in_path = pif;
				}
				child_kind[0] = REDIR_PATH;
				child_paths[0] = pif;
				child_modes[0] = O_RDONLY;
			}
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
				argv = argv + 1;
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

		/* Restore parent stdio state. The child-side spec we already
		 * built will be applied by exec_external_redir. Keep opened_*
		 * alive so the child inherits them; parent closes after waitpid. */
		if(saved_in_fd >= 0)  { hs_in_fd  = saved_in_fd;  hs_in_path  = saved_in_path;  saved_in_fd  = -1; }
		if(saved_out_fd >= 0) { hs_out_fd = saved_out_fd; hs_out_path = saved_out_path; saved_out_fd = -1; }
		if(saved_err_fd >= 0) { hs_err_fd = saved_err_fd; hs_err_path = saved_err_path; saved_err_fd = -1; }

		/* Take the redir path if any slot is non-NONE. */
		{
			int use_redir;
			use_redir = 0;
			if(rc > 0) use_redir = 1;
			if(child_kind[0] != REDIR_NONE) use_redir = 1;
			if(child_kind[1] != REDIR_NONE) use_redir = 1;
			if(child_kind[2] != REDIR_NONE) use_redir = 1;
			if(use_redir != 0)
			{
				last_status = exec_external_redir(pathbuf, argv, envp,
				                                  child_kind, child_paths,
				                                  child_modes, child_replay_fd,
				                                  child_shares_path);
			}
			else
			{
				last_status = exec_external(pathbuf, argv, envp);
			}
		}

		if(opened_in  >= 0) { close(opened_in);  opened_in  = -1; }
		if(opened_out >= 0) { close(opened_out); opened_out = -1; }
		if(opened_err >= 0) { close(opened_err); opened_err = -1; }

		tmp_cur = exec_save_tmp;
		return last_status;

exec_simple_cleanup:
		if(saved_in_fd  >= 0) { hs_in_fd  = saved_in_fd;  hs_in_path  = saved_in_path;  }
		if(saved_out_fd >= 0) { hs_out_fd = saved_out_fd; hs_out_path = saved_out_path; }
		if(saved_err_fd >= 0) { hs_err_fd = saved_err_fd; hs_err_path = saved_err_path; }
		if(opened_in  >= 0) close(opened_in);
		if(opened_out >= 0) close(opened_out);
		if(opened_err >= 0) close(opened_err);
		tmp_cur = exec_save_tmp;
		return last_status;
	}
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
	if(fn_return_flag) return last_status;
	if(loop_break_level > 0) return last_status;

	type = nd_type[idx];

	if(type == CMD_SIMPLE) return exec_simple(idx);

	if(type == CMD_LIST)
	{
		result = exec_node(nd_a[idx]);
		if(fn_return_flag || loop_break_level > 0 || loop_continue_flag) return result;
		/* `&&`/`||` chains handle errexit themselves; don't trip here. */
		if(flag_errexit && !suppress_errexit && result != 0)
		{
			if(nd_a[idx] >= 0 && nd_type[nd_a[idx]] != CMD_AND && nd_type[nd_a[idx]] != CMD_OR)
			{
				exit(result);
			}
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
		/* Capture the left side to a tmp file, then pass that path
		 * to the right side via pending_pipe_stdin_file (consumed
		 * inside exec_simple as an implicit `< file`). */
		{
			char* pp_tmpfile;
			char* pp_saved_pending;

			pp_tmpfile = capture_node_to_tmpfile(nd_a[idx]);

			pp_saved_pending = pending_pipe_stdin_file;
			pending_pipe_stdin_file = pp_tmpfile;

			result = exec_node(nd_b[idx]);

			pending_pipe_stdin_file = pp_saved_pending;

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
		loop_depth = loop_depth + 1;
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
			if(fn_return_flag) break;
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
		loop_depth = loop_depth - 1;
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
					word_list[wli] = strip_quotes(stripped);
					wli = wli + 1;
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

		loop_depth = loop_depth + 1;
		result = 0;
		wi = 0;
		while(wi < wli)
		{
			var_set(nd_str[idx], word_list[wi]);
			result = exec_node(nd_a[idx]);
			if(fn_return_flag) break;
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
		loop_depth = loop_depth - 1;
		last_status = result;
		return result;
	}

	if(type == CMD_CASE)
	{
		int case_save_tmp;
		case_save_tmp = tmp_cur;
		case_val = strip_quotes(expand_word(nd_str[idx]));
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
		/* No real subshell isolation; run in the current process. */
		result = exec_node(nd_a[idx]);
		last_status = result;
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
	lex_stack_src = calloc(LEX_STACK_MAX, sizeof(char*));
	lex_stack_pos = calloc(LEX_STACK_MAX, sizeof(int));
	lex_stack_len = calloc(LEX_STACK_MAX, sizeof(int));
	lex_stack_cmd = calloc(LEX_STACK_MAX, sizeof(int));
	var_init();
	fn_init();
	al_init();
	lsc_init();
	tok_init();
	nd_init();

	flag_errexit = FALSE;
	suppress_errexit = 0;
	flag_nounset = FALSE;
	flag_noglob = FALSE;
	fn_return_flag = FALSE;
	parsing_persistent = FALSE;
	loop_break_level = 0;
	loop_continue_flag = FALSE;
	loop_depth = 0;
	in_function = 0;
	in_subshell = 0;
	pending_pipe_stdin_file = NULL;
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
		exit(1);
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

	{
		char* cwd;
		cwd = getcwd(calloc(4096, 1), 4096);
		if(cwd != NULL) var_set("PWD", cwd);
	}

	script = argv[1];
	last_status = exec_source(script);

	sh_flush(stdout);
	return last_status;
}
