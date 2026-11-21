/*
 * (c) Thomas Pornin 1999 - 2002
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 4. The name of the authors may not be used to endorse or promote
 *    products derived from this software without specific prior written
 *    permission.
 *
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND WITHOUT ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHORS OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT
 * OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
 * EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include "tune.h"
#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include <limits.h>
#include "ucppi.h"
#include "mem.h"
#include "nhash.h"

/*
 * we store macros in a hash table, and retrieve them using their name
 * as identifier.
 */
static HTT macros;
static int macros_init_done = 0;

static void put_macro(struct macro *m, char *name)
{
	char *key = identifier_key(name);
	HTT_put(&macros, m, key);
	if (key != name) freemem(key);
}

static void remove_macro(char *name)
{
	char *key = identifier_key(name);
	HTT_del(&macros, key);
	if (key != name) freemem(key);
}

static int same_identifier(char *a, char *b)
{
	char *ka, *kb;
	int equal;
	if (!strcmp(a, b)) return 1;
	if (!strchr(a, '\\') && !strchr(b, '\\')) return 0;
	ka = identifier_key(a);
	kb = identifier_key(b);
	equal = !strcmp(ka, kb);
	if (ka != a) freemem(ka);
	if (kb != b) freemem(kb);
	return equal;
}

static void del_macro(void *m)
{
	struct macro *n = m;
	size_t i;

	for (i = 0; (int)i < n->narg; i ++) freemem(n->arg[i]);
	if (n->narg > 0) freemem(n->arg);
#ifdef LOW_MEM
	if (n->cval.length) freemem(n->cval.t);
#else
	if (n->val.nt) {
		for (i = 0; i < n->val.nt; i ++)
			if (S_TOKEN(n->val.t[i].type))
				freemem(n->val.t[i].name);
		freemem(n->val.t);
	}
#endif
	freemem(n);
}

static inline struct macro *new_macro(void)
{
	struct macro *m = getmem(sizeof(struct macro));

	m->narg = -1;
	m->nest = 0;
#ifdef LOW_MEM
	m->cval.length = 0;
#else
	m->val.nt = m->val.art = 0;
#endif
	m->vaarg = 0;
	return m;
}

/*
 * for special macros, and the "defined" operator
 */
enum {
	MAC_NONE, MAC_DEFINED,
	MAC_LINE, MAC_FILE, MAC_DATE, MAC_TIME, MAC_STDC, MAC_PRAGMA
};
#define MAC_SPECIAL	MAC_LINE

/*
 * returns 1 for "defined"
 * returns x > 1 for a special macro such as __FILE__
 * returns 0 otherwise
 */
static inline int check_special_macro(char *name)
{
	if (!strcmp(name, "defined")) return MAC_DEFINED;
	if (*name != '_') return MAC_NONE;
	if (*(name + 1) == 'P') {
		if (!strcmp(name, "_Pragma")) return MAC_PRAGMA;
		return MAC_NONE;
	} else if (*(name + 1) != '_') return MAC_NONE;
	if (no_special_macros) return MAC_NONE;
	if (!strcmp(name, "__LINE__")) return MAC_LINE;
	else if (!strcmp(name, "__FILE__")) return MAC_FILE;
	else if (!strcmp(name, "__DATE__")) return MAC_DATE;
	else if (!strcmp(name, "__TIME__")) return MAC_TIME;
	else if (!strcmp(name, "__STDC__")) return MAC_STDC;
	return MAC_NONE;
}

#ifdef UCPP_C11_MINIMAL_FEATURES
static char *c11_unavailable[] = {
	"__STDC_NO_ATOMICS__", "__STDC_NO_COMPLEX__", "__STDC_NO_THREADS__",
	"__STDC_NO_VLA__"
};

static const char *const c11_reserved[] = {
	"__STDC_ISO_10646__", "__STDC_MB_MIGHT_NEQ_WC__", "__STDC_UTF_16__",
	"__STDC_UTF_32__", "__STDC_ANALYZABLE__", "__STDC_IEC_559__",
	"__STDC_IEC_559_COMPLEX__", "__STDC_LIB_EXT1__"
};
#endif

/* These predefined macros expand normally but are still reserved. */
static int reserved_macro_name(char *name)
{
	if (check_special_macro(name)) return 1;
	if (no_special_macros || strncmp(name, "__STDC_", 7)) return 0;
	if (!strcmp(name, "__STDC_VERSION__") || !strcmp(name, "__STDC_HOSTED__"))
		return 1;
#ifdef UCPP_C11_MINIMAL_FEATURES
	{
		size_t i;
		for (i = 0; i < sizeof c11_unavailable / sizeof *c11_unavailable; i++)
			if (!strcmp(name, c11_unavailable[i])) return 1;
		for (i = 0; i < sizeof c11_reserved / sizeof *c11_reserved; i++)
			if (!strcmp(name, c11_reserved[i])) return 1;
	}
#endif
	return 0;
}

int c99_compliant = 1;
int c99_hosted = 1;

#ifndef UCPP_STDC_VERSION
#define UCPP_STDC_VERSION "199901L"
#endif

static void add_predefined_number(char *name, char *number)
{
	struct macro *m = new_macro();
#ifdef LOW_MEM
	size_t size = strlen(number) + 1;
	m->cval.t = getmem(1 + size);
	m->cval.t[0] = NUMBER;
	mmv(m->cval.t + 1, number, size);
	m->cval.length = 1 + size;
#else
	struct token t;
	t.type = NUMBER;
	t.line = 0;
	t.name = sdup(number);
	aol(m->val.t, m->val.nt, t, TOKEN_LIST_MEMG);
#endif
	HTT_put(&macros, m, name);
}

/*
 * add the special macros to the macro table
 */
static void add_special_macros(void)
{
	struct macro *m;

	HTT_put(&macros, new_macro(), "__LINE__");
	HTT_put(&macros, new_macro(), "__FILE__");
	HTT_put(&macros, new_macro(), "__DATE__");
	HTT_put(&macros, new_macro(), "__TIME__");
	HTT_put(&macros, new_macro(), "__STDC__");
	m = new_macro(); m->narg = 1;
	m->arg = getmem(sizeof(char *)); m->arg[0] = sdup("foo");
	HTT_put(&macros, m, "_Pragma");
	if (c99_compliant) add_predefined_number("__STDC_VERSION__", UCPP_STDC_VERSION);
	if (c99_hosted >= 0)
		add_predefined_number("__STDC_HOSTED__", c99_hosted > 0 ? "1" : "0");
#ifdef UCPP_C11_MINIMAL_FEATURES
	{
		size_t i;
		for (i = 0; i < sizeof c11_unavailable / sizeof *c11_unavailable; i++)
			add_predefined_number(c11_unavailable[i], "1");
		add_predefined_number("__STDC_UTF_16__", "1");
		add_predefined_number("__STDC_UTF_32__", "1");
	}
#endif
}

#ifdef LOW_MEM
/*
 * We store macro arguments as a single-byte token MACROARG, followed
 * by the argument number as a one or two-byte value. If the argument
 * number is between 0 and 127 (inclusive), it is stored as such in
 * a single byte. Otherwise, it is supposed to be a 14-bit number, with
 * the 7 upper bits stored in the first byte (with the high bit set to 1)
 * and the 7 lower bits in the second byte.
 */
#endif

/*
 * print the content of a macro, in #define form
 */
static void print_macro(void *vm)
{
	struct macro *m = vm;
	char *mname = HASH_ITEM_NAME(m);
	int x = check_special_macro(mname);
	size_t i;

	if (x != MAC_NONE) {
		fprintf(emit_output, "/* #define %s */ /* special */\n",
			mname);
		return;
	}
	fprintf(emit_output, "#define %s", mname);
	if (m->narg >= 0) {
		fprintf(emit_output, "(");
		for (i = 0; i < (size_t)(m->narg); i ++) {
			fprintf(emit_output, i ? ", %s" : "%s", m->arg[i]);
		}
		if (m->vaarg) {
			fputs(m->narg ? ", ..." : "...", emit_output);
		}
		fprintf(emit_output, ")");
	}
#ifdef LOW_MEM
	if (m->cval.length == 0) {
		fputc('\n', emit_output);
		return;
	}
	fputc(' ', emit_output);
	for (i = 0; i < m->cval.length;) {
		int tt = m->cval.t[i ++];

		if (tt == MACROARG) {
			unsigned anum = m->cval.t[i];

			if (anum >= 128) anum = ((anum & 127U) << 8)
				| m->cval.t[++ i];
			if (anum == (unsigned)m->narg)
				fputs("__VA_ARGS__", emit_output);
			else
				fputs(m->arg[anum], emit_output);
			i ++;
		}
		else if (S_TOKEN(tt)) {
			fputs((char *)(m->cval.t + i), emit_output);
			i += 1 + strlen((char *)(m->cval.t + i));
		} else fputs(operators_name[tt], emit_output);
	}
#else
	if (m->val.nt == 0) {
		fputc('\n', emit_output);
		return;
	}
	fputc(' ', emit_output);
	for (i = 0; i < m->val.nt; i ++) {
		if (m->val.t[i].type == MACROARG) {
			if (m->val.t[i].line == m->narg)
				fputs("__VA_ARGS__", emit_output);
			else
				fputs(m->arg[(size_t)(m->val.t[i].line)],
					emit_output);
		} else fputs(token_name(m->val.t + i), emit_output);
	}
#endif
	fputc('\n', emit_output);
}

/*
 * Send a token to the output (a token_fifo in lexer mode, the output
 * buffer in stand alone mode).
 */
void print_token(struct lexer_state *ls, struct token *t, long uz_line)
{
	char *x = t->name;

	if (uz_line && t->line < 0) t->line = uz_line;
	if (ls->flags & LEXER) {
		struct token at;

		at = *t;
		if (S_TOKEN(t->type)) {
			at.name = sdup(at.name);
			throw_away(ls->gf, at.name);
		}
		aol(ls->output_fifo->t, ls->output_fifo->nt, at,
			TOKEN_LIST_MEMG);
		return;
	}
	if (ls->flags & KEEP_OUTPUT) {
		for (; ls->oline < ls->line;) put_char(ls, '\n');
	}
	if (!S_TOKEN(t->type)) x = operators_name[t->type];
	for (; *x; x ++) put_char(ls, *x);
}

/*
 * Send a token to the output at a given line (this is for text output
 * and unreplaced macros due to lack of arguments).
 */
static void print_token_nailed(struct lexer_state *ls, struct token *t,
	long nail_line)
{
	char *x = t->name;

	if (ls->flags & LEXER) {
		print_token(ls, t, 0);
		return;
	}
	if (ls->flags & KEEP_OUTPUT) {
		for (; ls->oline < nail_line;) put_char(ls, '\n');
	}
	if (!S_TOKEN(t->type)) x = operators_name[t->type];
	for (; *x; x ++) put_char(ls, *x);
}

/*
 * send a reduced whitespace token to the output
 */
#define print_space(ls)	do { \
		struct token lt; \
		lt.type = OPT_NONE; \
		lt.line = (ls)->line; \
		print_token((ls), &lt, 0); \
	} while (0)

/*
 * We found a #define directive; parse the end of the line, perform
 * sanity checks, store the new macro into the "macros" hash table.
 *
 * In case of a redefinition of a macro: we enforce the rule that a
 * macro should be redefined identically, including the spelling of
 * parameters. We emit an error on offending code; dura lex, sed lex.
 * After all, it is easy to avoid such problems, with a #undef directive.
 */
int handle_define(struct lexer_state *ls)
{
	struct macro *m = 0, *n;
#ifdef LOW_MEM
	struct token_fifo mv;
#endif
	int ltwws = 1, redef = 0;
	char *mname = 0;
	int narg, status;
	size_t nt;
	long l = ls->line;
	
#ifdef LOW_MEM
	mv.art = mv.nt = 0;
#endif
	/* find the next non-white token on the line, this should be
	   the macro name */
	while (!next_token(ls) && ls->ctok->type != NEWLINE) {
		if (ttMWS(ls->ctok->type)) continue;
		if (ls->ctok->type == NAME) mname = sdup(ls->ctok->name);
		break;
	}
	if (mname == 0) {
		error(l, "missing macro name");
		goto warp_error;
	}
	if (reserved_macro_name(mname)) {
		error(l, "trying to redefine the special macro %s", mname);
		goto warp_error;
	}
	/*
	 * If a macro with this name was already defined: the K&R
	 * states that the new macro should be identical to the old one
	 * (with some arcane rule of equivalence of whitespace); otherwise,
	 * redefining the macro is an error. Most preprocessors would
	 * only emit a warning (or nothing at all) on an unidentical
	 * redefinition.
	 *
	 * Since it is easy to avoid this error (with a #undef directive),
	 * we choose to enforce the rule and emit an error.
	 */
	if ((n = get_macro(mname)) != 0) {
		/* redefinition of a macro: we must check that we define
		   it identical */
		redef = 1;
#ifdef LOW_MEM
		n->cval.rp = 0;
#endif
		freemem(mname);
		mname = 0;
	}
	if (!redef) {
		m = new_macro();
		m->narg = -1;
#ifdef LOW_MEM
#define mval	mv
#else
#define mval	(m->val)
#endif
	}
	status = next_token(ls);
	if (status) {
		if (redef) {
			if (n->narg >= 0) goto redef_error_2;
#ifdef LOW_MEM
			if (n->cval.length) goto redef_error_2;
#else
			if (n->val.nt) goto redef_error_2;
#endif
			return status == 1;
		}
		if (status == 1) goto define_error;
		goto define_end;
	}
	/*
	 * Check if the token immediately following the macro name is
	 * a left parenthesis; if so, then this is a macro with arguments.
	 * Collect their names and try to match the next parenthesis.
	 */
	if (ls->ctok->type == LPAR) {
		int i, j;
		int need_comma = 0, saw_mdots = 0;

		if (redef && n->narg < 0) goto redef_error;
		narg = 0;
		while (!(status = next_token(ls))) {
			if (ls->ctok->type == NEWLINE) {
				error(l, "truncated macro definition");
				goto define_error;
			}
			if (ls->ctok->type == COMMA) {
				if (saw_mdots) {
					error(l, "'...' must end the macro "
						"argument list");
					goto warp_error;
				}
				if (!need_comma) {
					error(l, "void macro argument");
					goto warp_error;
				}
				need_comma = 0;
				continue;
			} else if (ls->ctok->type == NAME) {
				if (saw_mdots) {
					error(l, "'...' must end the macro "
						"argument list");
					goto warp_error;
				}
				if (need_comma) {
					error(l, "missing comma in "
						"macro argument list");
					goto warp_error;
				}
				if (!redef) {
					aol(m->arg, narg,
						sdup(ls->ctok->name), 8);
					/* we must keep track of m->narg
					   so that cleanup in case of
					   error works. */
					m->narg = narg;
					if (narg == 128
						&& (ls->flags & WARN_STANDARD))
						warning(l, "more arguments to "
							"macro than the ISO "
							"limit (127)");
#ifdef LOW_MEM
					if (narg == 32767) {
						error(l, "too many arguments "
							"in macro definition "
							"(max 32766)");
						goto warp_error;
					}
#endif
				} else {
					/* this is a redefinition of the
					   macro; check equality between
					   old and new definitions */
					if (narg >= n->narg) goto redef_error;
					if (strcmp(ls->ctok->name,
						n->arg[narg ++]))
						goto redef_error;
				}
				need_comma = 1;
				continue;
			} else if ((ls->flags & MACRO_VAARG)
				&& ls->ctok->type == MDOTS) {
				if (need_comma) {
					error(l, "missing comma before '...'");
					goto warp_error;
				}
				if (redef && !n->vaarg) goto redef_error;
				if (!redef) m->vaarg = 1;
				saw_mdots = 1;
				need_comma = 1;
				continue;
			} else if (ls->ctok->type == RPAR) {
				if (narg > 0 && !need_comma) {
					error(l, "void macro argument");
					goto warp_error;
				}
				if (redef && (n->narg != narg || n->vaarg != saw_mdots))
					goto redef_error;
				break;
			} else if (ttMWS(ls->ctok->type)) {
				continue;
			}
			error(l, "invalid macro argument");
			goto warp_error;
		}
		if (status) {
			if (status != 1) error(l, "truncated macro definition");
			goto define_error;
		}
		if (!redef) {
			for (i = 1; i < narg; i ++) for (j = 0; j < i; j ++)
				if (same_identifier(m->arg[i], m->arg[j])) {
					error(l, "duplicate macro "
						"argument");
					goto warp_error;
				}
		}
		if (!redef) m->narg = narg;
	} else {
		if (redef && n->narg >= 0) goto redef_error;
		if (!ttWHI(ls->ctok->type) && (ls->flags & C11_CONSTRAINTS)) {
			error(l, "missing whitespace after object-like macro name");
			goto warp_error;
		}
		if (!ttWHI(ls->ctok->type) && (ls->flags & WARN_STANDARD))
			warning(ls->line, "identifier not followed by "
				"whitespace in #define");
		ls->flags |= READ_AGAIN;
		narg = 0;
	}
	if (redef) nt = 0;

	/* now, we have the arguments. Let's get the macro contents. */
	if (redef ? n->vaarg : m->vaarg) ls->flags |= VA_ARGS_REPLACEMENT;
	while (!(status = next_token(ls)) && ls->ctok->type != NEWLINE) {
		struct token t;

		t.type = ls->ctok->type;
		if (ltwws && ttMWS(t.type)) continue;
		t.line = 0;
		if (t.type == NAME) {
			int i;

			if ((ls->flags & MACRO_VAARG)
				&& !strcmp(ls->ctok->name, "__VA_ARGS__")) {
				if (redef) {
					if (!n->vaarg) goto redef_error;
				} else if (!m->vaarg) {
					if (!(ls->flags & C11_CONSTRAINTS))
						error(l, "'__VA_ARGS__' is forbidden "
							"in macros with a fixed "
							"number of arguments");
					goto warp_error;
				}
				t.type = MACROARG;
				t.line = redef ? n->narg : m->narg;
			}
			for (i = 0; i < narg; i ++)
				if (same_identifier(redef ? n->arg[i] : m->arg[i],
					ls->ctok->name)) {
					t.type = MACROARG;
					/* this is a hack: we store the
					   argument number in the line field */
					t.line = i;
					break;
				}
		}
		if (!redef && S_TOKEN(t.type)) t.name = sdup(ls->ctok->name);
		if (ttMWS(t.type)) {
			if (ltwws) continue;
#ifdef SEMPER_FIDELIS
			t.type = OPT_NONE;
#else
			t.type = NONE;
#endif
			ltwws = 1;
		} else ltwws = 0;
		if (!redef) {
			/* we ensure that each macro token has a correct
			   line number */
			if (t.type != MACROARG) t.line = 1;
			aol(mval.t, mval.nt, t, TOKEN_LIST_MEMG);
		} else {
#ifdef LOW_MEM
			int tt;

			if (n->cval.rp >= n->cval.length) {
#ifdef SEMPER_FIDELIS
				if (t.type != OPT_NONE) goto redef_error;
#else
				if (t.type != NONE) goto redef_error;
#endif
				continue; /* Only trailing whitespace beyond the old list. */
			} else if (t.type != n->cval.t[n->cval.rp]) {
				goto redef_error;
			} else if (t.type == MACROARG) {
				unsigned anum = n->cval.t[n->cval.rp + 1];

				if (anum >= 128U) anum = ((anum & 127U) << 8)
					| n->cval.t[n->cval.rp + 2];
				if (anum != (unsigned)t.line) goto redef_error;
			} else if (S_TOKEN(t.type) && strcmp(ls->ctok->name,
				   (char *)(n->cval.t + n->cval.rp + 1))) {
				goto redef_error;
			}
			tt = n->cval.t[n->cval.rp ++];
			if (S_TOKEN(tt)) n->cval.rp += 1
				+ strlen((char *)(n->cval.t + n->cval.rp));
			else if (tt == MACROARG) {
				if (n->cval.t[n->cval.rp ++] >= 128)
					n->cval.rp ++;
			}
#else
			if (nt >= n->val.nt) {
#ifdef SEMPER_FIDELIS
				if (t.type != OPT_NONE) goto redef_error;
#else
				if (t.type != NONE) goto redef_error;
#endif
			} else if (t.type != n->val.t[nt].type
				|| (t.type == MACROARG
				    && t.line != n->val.t[nt].line)
				|| (S_TOKEN(t.type) && strcmp(ls->ctok->name,
				   n->val.t[nt].name))) {
				goto redef_error;
			}
#endif
			nt ++;
		}
	}

	ls->flags &= ~VA_ARGS_REPLACEMENT;
	if (status == 1) goto define_error;
	if (redef) {
#ifdef LOW_MEM
		if (n->cval.rp < n->cval.length) goto redef_error_2;
#else
		if (nt < n->val.nt) goto redef_error_2;
#endif
		return 0;
	}

	/* now we have the complete macro; perform some checks about
	   the operators # and ##, and, if everything is ok,
	   store the macro into the hash table */
define_end:
#ifdef SEMPER_FIDELIS
	if (mval.nt && mval.t[mval.nt - 1].type == OPT_NONE) {
#else
	if (mval.nt && mval.t[mval.nt - 1].type == NONE) {
#endif
		mval.nt --;
		if (mval.nt == 0) freemem(mval.t);
	}
	if (mval.nt != 0) {
		size_t i;

		/* some checks about the macro */
		if (mval.t[0].type == DSHARP
			|| mval.t[0].type == DIG_DSHARP
			|| mval.t[mval.nt - 1].type == DSHARP
			|| mval.t[mval.nt - 1].type == DIG_DSHARP) {
			error(l, "operator '##' may neither begin "
				"nor end a macro");
			goto define_error;
		}
		if (m->narg >= 0) for (i = 0; i < mval.nt; i ++)
			if ((mval.t[i].type == SHARP
				|| mval.t[i].type == DIG_SHARP) &&
				(i == (mval.nt - 1)
				|| (ttMWS(mval.t[i + 1].type) &&
				    (i == mval.nt - 2
				     || mval.t[i + 2].type != MACROARG))
				|| (!ttMWS(mval.t[i + 1].type)
				     && mval.t[i + 1].type != MACROARG))) {
				error(l, "operator '#' not followed "
					"by a macro argument");
				goto define_error;
			}
	}
#ifdef LOW_MEM
	{
		size_t i, l;

		for (i = 0, l = 0; i < mval.nt; i ++) {
			l ++;
			if (S_TOKEN(mval.t[i].type))
				l += 1 + strlen(mval.t[i].name);
			else if (mval.t[i].type == MACROARG) {
				l ++;
				if (mval.t[i].line >= 128) l ++;
			}
		}
		m->cval.length = l;
		if (l) m->cval.t = getmem(l);
		for (i = 0, l = 0; i < mval.nt; i ++) {
			m->cval.t[l ++] = mval.t[i].type;
			if (S_TOKEN(mval.t[i].type)) {
				size_t x = 1 + strlen(mval.t[i].name);

				mmv(m->cval.t + l, mval.t[i].name, x);
				l += x;
				freemem(mval.t[i].name);
			}
			else if (mval.t[i].type == MACROARG) {
				unsigned anum = mval.t[i].line;

				if (anum >= 128) {
					m->cval.t[l ++] = 128 | (anum >> 8);
					m->cval.t[l ++] = anum & 0xFF;
				} else {
					m->cval.t[l ++] = anum;
				}
			}
		}
		if (mval.nt) freemem(mval.t);
	}
#endif
	put_macro(m, mname);
	freemem(mname);
	if (emit_defines) print_macro(m);
	return 0;

redef_error:
	while (ls->ctok->type != NEWLINE && !next_token(ls));
redef_error_2:
	error(l, "macro '%s' redefined unidentically", HASH_ITEM_NAME(n));
	return 1;
warp_error:
	while (ls->ctok->type != NEWLINE && !next_token(ls));
define_error:
	ls->flags &= ~VA_ARGS_REPLACEMENT;
	if (m) del_macro(m);
	if (mname) freemem(mname);
#ifdef LOW_MEM
	if (mv.nt) {
		size_t i;

		for (i = 0; i < mv.nt; i ++)
			if (S_TOKEN(mv.t[i].type)) freemem(mv.t[i].name);
		freemem(mv.t);
	}
#endif
	return 1;
#undef mval
}

/*
 * Get the arguments for a macro. This code is tricky because there can
 * be multiple sources for these arguments, if we are in the middle of
 * a macro replacement; arguments are macro-replaced before inclusion
 * into the macro replacement.
 *
 * return value:
 * 1	no argument (last token read from next_token())
 * 2    no argument (last token read from tfi)
 * 3    no argument (nothing read)
 * 4	error
 *
 * Void arguments are allowed in C99.
 */
static int collect_arguments(struct lexer_state *ls, struct token_fifo *tfi,
	int penury, struct token_fifo *atl, int narg, int vaarg, int *wr)
{
	int ltwws = 1, npar = 0, i;
	struct token *ct = 0;
	int read_from_fifo = 0;
	long begin_line = ls->line;

#define unravel(ls)	(read_from_fifo = 0, !((tfi && tfi->art < tfi->nt \
	&& (read_from_fifo = 1) != 0 && (ct = tfi->t + (tfi->art ++))) \
	|| ((!tfi || penury) && !next_token(ls) && (ct = (ls)->ctok))))

	/*
	 * collect_arguments() is assumed to setup correctly atl
	 * (this is not elegant, but it works)
	 */
	for (i = 0; i < narg; i ++) atl[i].art = atl[i].nt = 0;
	if (vaarg) atl[narg].art = atl[narg].nt = 0;
	*wr = 0;
	while (!unravel(ls)) {
		if (!read_from_fifo && ct->type == NEWLINE) ls->ltwnl = 1;
		if (ttWHI(ct->type)) {
			*wr = 1;
			continue;
		}
		if (ct->type == LPAR) {
			npar = 1;
		}
		break;
	}
	if (!npar) {
		if (ct == ls->ctok) return 1;
		if (read_from_fifo) return 2;
		return 3;
	}
	if (!read_from_fifo && ct == ls->ctok) ls->ltwnl = 0;
	i = 0;
	if ((narg + vaarg) == 0) {
		while(!unravel(ls)) {
			if (ttWHI(ct->type)) continue;
			if (ct->type == RPAR) goto harvested;
			npar = 1;
			goto too_many_args;
		}
	}
	while (!unravel(ls)) {
		struct token t;

		if (ct->type == LPAR) npar ++;
		else if (ct->type == RPAR && (-- npar) == 0) {
			if (atl[i].nt != 0
				&& ttMWS(atl[i].t[atl[i].nt - 1].type))
					atl[i].nt --;
			i ++;
			/*
			 * C99 standard states that at least one argument
			 * should be present for the ... part; to relax
			 * this behaviour, change 'narg + vaarg' to 'narg'.
			 */
			if (i < (narg + vaarg)) {
				error(begin_line, "not enough arguments "
					"to macro");
				return 4;
			}
			if (i > narg) {
				if (!(ls->flags & MACRO_VAARG) || !vaarg)
					goto too_many_args;
			}
			goto harvested;
		} else if (ct->type == COMMA && npar <= 1 && i < narg) {
			if (atl[i].nt != 0
				&& ttMWS(atl[i].t[atl[i].nt - 1].type))
					atl[i].nt --;
			if (++ i == narg) {
				if (!(ls->flags & MACRO_VAARG) || !vaarg)
					goto too_many_args;
			}
			if (i > 30000) goto too_many_args;
			ltwws = 1;
			continue;
		} else if (ltwws && ttWHI(ct->type)) continue;

		t.type = ct->type;
		t.line = ct->line;
		/*
		 * Stringification applies only to macro arguments;
		 * so we handle here OPT_NONE.
		 * OPT_NONE is kept, but does not count as whitespace,
		 * and merges with other whitespace to give a fully
		 * qualified NONE token. Two OPT_NONE tokens merge.
		 * Initial and final OPT_NONE are discarded (initial
		 * is already done, as OPT_NONE is matched by ttWHI).
		 */
		if (ttWHI(t.type)) {
			if (t.type != OPT_NONE) {
				t.type = NONE;
#ifdef SEMPER_FIDELIS
				t.name = sdup(" ");
				throw_away(ls->gf, t.name);
#endif
				ltwws = 1;
			}
			if (atl[i].nt > 0
				&& atl[i].t[atl[i].nt - 1].type == OPT_NONE)
					atl[i].nt --;
		} else { 
			ltwws = 0;
			if (S_TOKEN(t.type)) {
				t.name = ct->name;
				if (ct == (ls)->ctok) {
					t.name = sdup(t.name);
					throw_away(ls->gf, t.name);
				}
			}
		}
		aol(atl[i].t, atl[i].nt, t, TOKEN_LIST_MEMG);
	}
	error(begin_line, "unfinished macro call");
	return 4;
too_many_args:
	error(begin_line, "too many arguments to macro");
	while (npar && !unravel(ls)) {
		if (ct->type == LPAR) npar ++;
		else if (ct->type == RPAR) npar --;
	}
	return 4;
harvested:
	if (i > 127 && (ls->flags & WARN_STANDARD))
		warning(begin_line, "macro call with %d arguments (ISO "
			"specifies 127 max)", i);
	return 0;
#undef unravel
}

/*
 * concat_token() is called when the ## operator is used. It uses
 * the struct lexer_state dsharp_lexer to parse the result of the
 * concatenation.
 *
 * Law enforcement: if the whole string does not produce a valid
 * single token, an error (non-zero result) is returned.
 */
struct lexer_state dsharp_lexer;

static inline int concat_token(struct lexer_state *ls, struct token *t1,
	struct token *t2)
{
	char *n1 = token_name(t1), *n2 = token_name(t2);
	size_t l1 = strlen(n1), l2 = strlen(n2);
	unsigned char *x = getmem(l1 + l2 + 1);
	int r;

	mmv(x, n1, l1);
	mmv(x + l1, n2, l2);
	x[l1 + l2] = 0;
	dsharp_lexer.input = 0;
	dsharp_lexer.input_string = x;
	dsharp_lexer.pbuf = 0;
	dsharp_lexer.ebuf = l1 + l2;
	dsharp_lexer.discard = 1;
	dsharp_lexer.flags = (DEFAULT_LEXER_FLAGS & ~HANDLE_TRIGRAPHS)
		| PHASE3_INPUT | (ls->flags & UTF8_SOURCE);
	dsharp_lexer.line = t1->line > 0 ? t1->line : t2->line;
	dsharp_lexer.nlka = 0;
	dsharp_lexer.macfile = 0;
	dsharp_lexer.pending_token = 0;
	dsharp_lexer.character_error = 0;
	r = next_token(&dsharp_lexer);
	ls->character_error |= dsharp_lexer.character_error;
	freemem(x);
	/* An identifier/number usually leaves EOF in last, whereas a
	   closed string or punctuator can finish without looking past it. */
	return r != 0 || dsharp_lexer.pbuf < l1 + l2 || dsharp_lexer.nlka
		|| dsharp_lexer.pending_token || ttWHI(dsharp_lexer.ctok->type)
		|| (!dsharp_lexer.discard && dsharp_lexer.last >= 0);
}

#ifdef PRAGMA_TOKENIZE
/*
 * Tokenize _Pragma's destringized spelling in phase 3 only (6.10.9).
 */
struct lexer_state tokenize_lexer;

static void free_pragma_tokens(struct token_fifo *tf)
{
	size_t i;

	for (i = 0; i < tf->nt; i ++)
		if (S_TOKEN(tf->t[i].type)) freemem(tf->t[i].name);
	if (tf->nt) freemem(tf->t);
}

static int tokenize_pragma(struct lexer_state *ls, char *buf, long line,
	struct token_fifo *tf)
{
	int r;
	size_t length = strlen(buf);

	/* Treat the spelling as the contents of one complete directive. */
	buf[length] = '\n';
	tokenize_lexer.input = 0;
	tokenize_lexer.input_string = (unsigned char *)buf;
	tokenize_lexer.pbuf = 0;
	tokenize_lexer.ebuf = length + 1;
	tokenize_lexer.discard = 1;
	tokenize_lexer.line = line;
	tokenize_lexer.flags = (ls->flags & ~(HANDLE_TRIGRAPHS | READ_AGAIN))
		| LEXER | PHASE3_INPUT;
	tokenize_lexer.nlka = 0;
	tokenize_lexer.macfile = 0;
	tokenize_lexer.pending_token = 0;
	tokenize_lexer.character_error = 0;
	tf->art = tf->nt = 0;
	while (!(r = next_token(&tokenize_lexer))) {
		struct token t, *ct = tokenize_lexer.ctok;

		if (ttWHI(ct->type)) continue;
		t = *ct;
		if (S_TOKEN(t.type)) t.name = sdup(t.name);
		aol(tf->t, tf->nt, t, TOKEN_LIST_MEMG);
	}
	buf[length] = 0;
	return r == 1 || tokenize_lexer.character_error;
}
#endif

/*
 * stringify_string() has a self-explanatory name. It is called when
 * the # operator is used in a macro and a string constant must be
 * stringified.
 */
static inline char *stringify_string(char *x)
{
	size_t l;
	int i, inside_str = 0, inside_cc = 0, must_quote, has_quoted = 0;
	char *y, *d;

	for (i = 0; i < 2; i ++) {
		if (i) d[0] = '"';
		for (l = 1, y = x; *y; y ++, l ++) {
			must_quote = 0;
			if (inside_cc) {
				if (*y == '\\') {
					must_quote = 1;
					has_quoted = 1;
				} else if (!has_quoted && *y == '\'')
					inside_cc = 0;
			} else if (inside_str) {
				if (*y == '"' || *y == '\\') must_quote = 1;
				if (*y == '\\') has_quoted = 1;
				else if (!has_quoted && *y == '"')
					inside_str = 0;
			} else if (*y == '"') {
				inside_str = 1;
				must_quote = 1;
			} else if (*y == '\'') {
				inside_cc = 1;
			}
			if (must_quote) {
				if (i) d[l] = '\\';
				l ++;
			}
			if (i) d[l] = *y;
		}
		if (!i) d = getmem(l + 2);
		if (i) {
			d[l] = '"';
			d[l + 1] = 0;
		}
	}
	return d;
}

/*
 * stringify() produces a constant string, result of the # operator
 * on a list of tokens.
 */
static char *stringify(struct token_fifo *tf)
{
	size_t tlen;
	size_t i;
	char *x, *y;

	for (tlen = 0, i = 0; i < tf->nt; i ++)
		if (tf->t[i].type < CPPERR && tf->t[i].type != OPT_NONE)
			tlen += strlen(token_name(tf->t + i));
	if (tlen == 0) return sdup("\"\"");
	x = getmem(tlen + 1);
	for (tlen = 0, i = 0; i < tf->nt; i ++) {
		if (tf->t[i].type >= CPPERR || tf->t[i].type == OPT_NONE)
			continue;
		strcpy(x + tlen, token_name(tf->t + i));
		tlen += strlen(token_name(tf->t + i));
	}
	/* no need to add a trailing 0: strcpy() did that (and the string
	   is not empty) */
	y = stringify_string(x);
	freemem(x);
	return y;
}

/*
 * Two strings evaluated at initialization time, to handle the __TIME__
 * and __DATE__ special macros.
 *
 * C99 specifies that these macros should remain constant throughout
 * the whole preprocessing.
 */
char compile_time[12], compile_date[24];

/*
 * substitute_macro() performs the macro substitution. It is called when
 * an identifier recognized as a macro name has been found; this function
 * tries to collect the arguments (if needed), applies # and ## operators
 * and perform recursive and nested macro expansions.
 *
 * In the substitution of a macro, we remove all newlines that were in the
 * arguments. This might confuse error reporting (which could report
 * erroneous line numbers) or have worse effect is the preprocessor is
 * used for another language pickier than C. Since the interface between
 * the preprocessor and the compiler is not fully specified, I believe
 * that this is no violation of the standard. Comments welcome.
 *
 * We take tokens from tfi. If tfi has no more tokens to give: we may
 * take some tokens from ls to complete a call (fetch arguments) if
 * and only if penury is non zero.
 */
int substitute_macro(struct lexer_state *ls, struct macro *m, char *spelling,
	struct token_fifo *tfi, int penury, int reject_nested, long l)
{
	char *mname = HASH_ITEM_NAME(m);
	struct token_fifo *atl, etl;
	struct token t, *ct;
	int i, save_nest = m->nest;
	size_t save_art, save_tfi, etl_limit;
	int ltwds, ntwds, ltwws;
	int pragma_op = 0;

	/*
	 * Reject the replacement, if we are already inside the macro.
	 */
	if (m->nest > reject_nested) {
		t.type = NAME;
		t.line = ls->line;
		t.name = spelling;
		print_token(ls, &t, 0);
		return 0;
	}

	/*
	 * put a separation from preceeding tokens
	 */
	print_space(ls);

	/*
	 * Check if the macro is a special one.
	 */
	if ((i = check_special_macro(mname)) >= MAC_SPECIAL) {
		/* we have a special macro */
		switch (i) {
			char buf[30], *bbuf;

		case MAC_LINE:
			t.type = NUMBER;
			t.line = l;
			sprintf(buf, "%ld", l);
			t.name = buf;
			print_space(ls);
			print_token(ls, &t, 0);
			break;
		case MAC_FILE:
			t.type = STRING;
			t.line = l;
			bbuf = quote_filename(current_filename);
			t.name = bbuf;
			print_space(ls);
			print_token(ls, &t, 0);
			freemem(bbuf);
			break;
		case MAC_DATE:
			t.type = STRING;
			t.line = l;
			t.name = compile_date;
			print_space(ls);
			print_token(ls, &t, 0);
			break;
		case MAC_TIME:
			t.type = STRING;
			t.line = l;
			t.name = compile_time;
			print_space(ls);
			print_token(ls, &t, 0);
			break;
		case MAC_STDC:
			t.type = NUMBER;
			t.line = l;
			t.name = "1";
			print_space(ls);
			print_token(ls, &t, 0);
			break;
		case MAC_PRAGMA:
			if (reject_nested > 0) {
				/* do not replace _Pragma() unless toplevel */
				t.type = NAME;
				t.line = l;
				t.name = mname;
				print_token(ls, &t, 0);
				return 0;
			}
			pragma_op = 1;
			goto collect_args;
#ifdef AUDIT
		default:
			ouch("unbekanntes fliegendes macro");
#endif
		}
		return 0;
	}

	/*
	 * If the macro has arguments, collect them.
	 */
collect_args:
	if (m->narg >= 0) {
		unsigned long save_flags = ls->flags;
		int wr = 0;

		/* A function-like macro without '(' retains its original token
		   spelling, even when lookup used a different canonical key. */
		if (!tfi) {
			if (strcmp(spelling, mname)) {
				spelling = sdup(spelling);
				throw_away(ls->gf, spelling);
			} else spelling = mname;
		}
		ls->flags |= LEXER;
		if (m->narg > 0 || m->vaarg)
			atl = getmem((m->narg + m->vaarg)
				* sizeof(struct token_fifo));
		switch (collect_arguments(ls, tfi, penury, atl,
			m->narg, m->vaarg, &wr)) {
		case 1:
			/* the macro expected arguments, but we did not
			   find any; the last read token should be read
			   again. */
			ls->flags = save_flags | READ_AGAIN;
			goto no_argument_next;
		case 2:
			tfi->art --;
			/* fall through */
		case 3:
			ls->flags = save_flags;
		no_argument_next:
			t.type = NAME;
			t.line = l;
			t.name = spelling;
			print_token_nailed(ls, &t, l);
			if (wr) {
				t.type = NONE;
				t.line = l;
#ifdef SEMPER_FIDELIS
				t.name = " ";
#endif
				print_token(ls, &t, 0);
				goto exit_macro_2;
			}
			goto exit_macro_1;
		case 4:
			ls->flags = save_flags;
			goto exit_error_1;
		}
		ls->flags = save_flags;
	}

	/*
	 * If the macro is _Pragma, and we got here, then we have
	 * exactly one argument. We check it, unstringize it, and
	 * emit a PRAGMA token.
	 */
	if (pragma_op) {
		char *buf, *d;
		const char *c;

		if (atl[0].nt != 1 || atl[0].t[0].type != STRING) {
			error(l, "invalid argument to _Pragma");
			if (atl[0].nt) freemem(atl[0].t);
			freemem(atl);
			goto exit_error;
		}
		/* Destringizing deletes any C11 encoding prefix. */
		c = strchr(atl[0].t[0].name, '"') + 1;
		if (*c == '"') {
			/* void pragma -- just ignore it */
			freemem(atl[0].t);
			freemem(atl);
			return 0;
		}
		for (buf = d = getmem(strlen(c)); *c != '"'; c ++) {
			if (*c == '\\' && (*(c + 1) == '\\' || *(c + 1) == '"'))
				c ++;
			*(d ++) = *c;
		}
		*d = 0;
		freemem(atl[0].t);
		freemem(atl);
#ifdef PRAGMA_TOKENIZE
		if (ls->flags & PROCESS_TEXT_PRAGMAS) {
			struct token_fifo tf;
			int failed = tokenize_pragma(ls, buf, l, &tf);
			if (!failed) failed = check_pragma(&tf, l);
			free_pragma_tokens(&tf);
			freemem(buf);
			return failed;
		}
#endif
		if (ls->flags & TEXT_OUTPUT) {
#ifdef PRAGMA_DUMP
	/*
	 * This code works because we actually evaluate arguments in a
	 * lazy way: we scan a macro argument only if it appears in the
	 * output, and exactly as many times as it appears. Therefore,
	 * _Pragma() will get evaluated just like they should.
	 */
			for (d = "\n#pragma "; *d; d ++) put_char(ls, *d);
			for (d = buf; *d; d ++) put_char(ls, *d);
			put_char(ls, '\n');
			ls->oline = ls->line;
			enter_file(ls, ls->flags);
#else
			if (ls->flags & WARN_PRAGMA)
				warning(ls->line,
					"_Pragma() ignored and not dumped");
#endif
		} else if (ls->flags & HANDLE_PRAGMA) {
			struct token t;

			t.type = PRAGMA;
			t.line = l;
#ifdef PRAGMA_TOKENIZE
			{
				struct token_fifo tf;
				int failed = tokenize_pragma(ls, buf, l, &tf);
				freemem(buf);
				if (failed) {
					free_pragma_tokens(&tf);
					goto exit_error;
				}
				buf = (char *)compress_token_list(&tf).t;
			}
#endif
			t.name = buf;
			aol(ls->toplevel_of->t, ls->toplevel_of->nt,
				t, TOKEN_LIST_MEMG);
			throw_away(ls->gf, buf);
			return 0;
		}
		freemem(buf);
		return 0;
	}

	/*
	 * Now we expand and replace the arguments in the macro; we
	 * also handle '#' and '##'. If we find an argument, that has
	 * to be replaced, we expand it in its own token list, then paste
	 * it. Tricky point: when we paste an argument, we must scan
	 * again the resulting list for further replacements. This
	 * implies problems with regards to nesting self-referencing
	 * macros.
	 *
	 * We do then YAUH (yet another ugly hack): if a macro is replaced,
	 * and nested replacement exhibit the same macro, we mark it with
	 * a negative line number. All produced negative line numbers
	 * must be cleaned in the end.
	 */

#define ZAP_LINE(t)	do { \
		if ((t).type == NAME && (t).line >= 0) { \
			struct macro *zlm = get_macro((t).name); \
			if (zlm && zlm->nest > reject_nested) \
				(t).line = -1 - (t).line; \
		} \
	} while (0)

#ifdef LOW_MEM
	save_art = m->cval.rp;
	m->cval.rp = 0;
#else
	save_art = m->val.art;
	m->val.art = 0;
#endif
	etl.art = etl.nt = 0;
	m->nest = reject_nested + 1;
	ltwds = ntwds = 0;
#ifdef LOW_MEM
	while (m->cval.rp < m->cval.length) {
#else
	while (m->val.art < m->val.nt) {
#endif
		size_t next, z;
#ifdef LOW_MEM
		struct token uu;

		ct = &uu;
		ct->line = 1;
		t.type = ct->type = m->cval.t[m->cval.rp ++];
		if (ct->type == MACROARG) {
			unsigned anum = m->cval.t[m->cval.rp ++];

			if (anum >= 128U) anum = ((anum & 127U) << 8)
				| (unsigned)m->cval.t[m->cval.rp ++];
			ct->line = anum;
		} else if (S_TOKEN(ct->type)) {
			t.name = ct->name = (char *)(m->cval.t + m->cval.rp);
			m->cval.rp += 1 + strlen(ct->name);
		}
#ifdef SEMPER_FIDELIS
		else if (ct->type == OPT_NONE) {
			t.type = ct->type = NONE;
			t.name = ct->name = " ";
		}
#endif
		t.line = ls->line;
		next = m->cval.rp;
		if ((next < m->cval.length && (m->cval.t[z = next] == DSHARP
			|| m->cval.t[z = next] == DIG_DSHARP))
			|| ((next + 1) < m->cval.length
			   && ttWHI(m->cval.t[next])
			   && (m->cval.t[z = next + 1] == DSHARP
			    || m->cval.t[z = next + 1] == DIG_DSHARP))) {
			ntwds = 1;
			m->cval.rp = z;
		} else ntwds = 0;
#else
		ct = m->val.t + (m->val.art ++);
		next = m->val.art;
		t.type = ct->type;
		t.line = ls->line;
#ifdef SEMPER_FIDELIS
		if (t.type == OPT_NONE) {
			t.type = NONE;
			t.name = " ";
		} else
#endif
		t.name = ct->name;
		if ((next < m->val.nt && (m->val.t[z = next].type == DSHARP
			|| m->val.t[z = next].type == DIG_DSHARP))
			|| ((next + 1) < m->val.nt
			   && ttWHI(m->val.t[next].type)
			   && (m->val.t[z = next + 1].type == DSHARP
			    || m->val.t[z = next + 1].type == DIG_DSHARP))) {
			ntwds = 1;
			m->val.art = z;
		} else ntwds = 0;
#endif
		if (ct->type == MACROARG) {
#ifdef DSHARP_TOKEN_MERGE
			int need_opt_space = 1;
#endif
			z = ct->line;	/* the argument number is there */
			if (ltwds && atl[z].nt != 0 && etl.nt) {
				if (concat_token(ls, etl.t + (-- etl.nt),
					atl[z].t)) {
					warning(ls->line, "operator '##' "
						"produced the invalid token "
						"'%s%s'",
						token_name(etl.t + etl.nt),
						token_name(atl[z].t));
#if 0
/* obsolete */
#ifdef LOW_MEM
					m->cval.rp = save_art;
#else
					m->val.art = save_art;
#endif
					etl.nt ++;
					goto exit_error_2;
#endif
					etl.nt ++;
					atl[z].art = 0;
#ifdef DSHARP_TOKEN_MERGE
					need_opt_space = 0;
#endif
				} else {
					if (etl.nt == 0) freemem(etl.t);
					else if (!ttWHI(etl.t[etl.nt - 1]
						.type)) {
						t.type = OPT_NONE;
						t.line = ls->line;
						aol(etl.t, etl.nt, t,
							TOKEN_LIST_MEMG);
					}
					t.type = dsharp_lexer.ctok->type;
					t.line = ls->line;
					if (S_TOKEN(t.type)) {
						t.name = sdup(dsharp_lexer
							.ctok->name);
						throw_away(ls->gf, t.name);
					}
					ZAP_LINE(t);
					aol(etl.t, etl.nt, t, TOKEN_LIST_MEMG);
					atl[z].art = 1;
				}
			} else atl[z].art = 0;
			if (
#ifdef DSHARP_TOKEN_MERGE
				need_opt_space &&
#endif
				atl[z].art < atl[z].nt && (!etl.nt
					|| !ttWHI(etl.t[etl.nt - 1].type))) {
				t.type = OPT_NONE;
				t.line = ls->line;
				aol(etl.t, etl.nt, t, TOKEN_LIST_MEMG);
			}
			if (ltwds || ntwds) {
				while (atl[z].art < atl[z].nt) {
					t = atl[z].t[atl[z].art ++];
					ZAP_LINE(t);
					aol(etl.t, etl.nt, t, TOKEN_LIST_MEMG);
				}
			} else {
				struct token_fifo *save_tf;
				unsigned long save_flags;
				int ret = 0;

				atl[z].art = 0;
				save_tf = ls->output_fifo;
				ls->output_fifo = &etl;
				save_flags = ls->flags;
				ls->flags |= LEXER;
				while (atl[z].art < atl[z].nt) {
					struct macro *nm;
					struct token *cct;

					cct = atl[z].t + (atl[z].art ++);
					if (cct->type == NAME
						&& cct->line >= 0
						&& (nm = get_macro(cct->name))
						&& nm->nest <=
						    (reject_nested + 1)) {
						ret |= substitute_macro(ls,
							nm, cct->name, atl + z, 0,
							reject_nested + 1,
							cct->line ? cct->line : l);
						continue;
					}
					t = *cct;
					ZAP_LINE(t);
					aol(etl.t, etl.nt, t, TOKEN_LIST_MEMG);
				}
				ls->output_fifo = save_tf;
				ls->flags = save_flags;
				if (ret) {
#ifdef LOW_MEM
					m->cval.rp = save_art;
#else
					m->val.art = save_art;
#endif
					goto exit_error_2;
				}
			}
			if (!ntwds && (!etl.nt
				|| !ttWHI(etl.t[etl.nt - 1].type))) {
				t.type = OPT_NONE;
				t.line = ls->line;
				aol(etl.t, etl.nt, t, TOKEN_LIST_MEMG);
			}
			ltwds = 0;
			continue;
		}
		/*
		 * This code is definitely cursed.
		 *
		 * For the extremely brave reader who tries to understand
		 * what is happening: ltwds is a flag meaning "last token
		 * was double-sharp" and ntwds means "next token will be
		 * double-sharp". The tokens are from the macro definition,
		 * and scanned from left to right. Arguments that are
		 * not implied into a #/## construction are macro-expanded
		 * seperately, then included into the token stream.
		 */
		if (ct->type == DSHARP || ct->type == DIG_DSHARP) {
			if (ltwds) {
				error(ls->line, "quad sharp");
#ifdef LOW_MEM
				m->cval.rp = save_art;
#else
				m->val.art = save_art;
#endif
				goto exit_error_2;
			}
#ifdef LOW_MEM
			if (m->cval.rp < m->cval.length
				&& ttMWS(m->cval.t[m->cval.rp]))
					m->cval.rp ++;
#else
			if (m->val.art < m->val.nt
				&& ttMWS(m->val.t[m->val.art].type))
					m->val.art ++;
#endif
			ltwds = 1;
			continue;
		} else if (ltwds && etl.nt != 0) {
			if (concat_token(ls, etl.t + (-- etl.nt), ct)) {
				warning(ls->line, "operator '##' produced "
					"the invalid token '%s%s'",
					token_name(etl.t + etl.nt),
					token_name(ct));
#if 0
/* obsolete */
#ifdef LOW_MEM
				m->cval.rp = save_art;
#else
				m->val.art = save_art;
#endif
				etl.nt ++;
				goto exit_error_2;
#endif
				etl.nt ++;
			} else {
				if (etl.nt == 0) freemem(etl.t);
				t.type = dsharp_lexer.ctok->type;
				t.line = ls->line;
				if (S_TOKEN(t.type)) {
					t.name = sdup(dsharp_lexer.ctok->name);
					throw_away(ls->gf, t.name);
				}
				ct = &t;
			}
		}
		ltwds = 0;
#ifdef LOW_MEM
		if ((ct->type == SHARP || ct->type == DIG_SHARP)
			&& next < m->cval.length
			&& (m->cval.t[next] == MACROARG
			|| (ttMWS(m->cval.t[next])
			&& (next + 1) < m->cval.length
			&& m->cval.t[next + 1] == MACROARG))) {

			unsigned anum;
#else
		if ((ct->type == SHARP || ct->type == DIG_SHARP)
			&& next < m->val.nt
			&& (m->val.t[next].type == MACROARG
			|| (ttMWS(m->val.t[next].type)
			&& (next + 1) < m->val.nt
			&& m->val.t[next + 1].type == MACROARG))) {
#endif
			/*
			 * We have a # operator followed by (an optional
			 * whitespace and) a macro argument; this means
			 * stringification. So be it.
			 */
#ifdef LOW_MEM
			if (ttMWS(m->cval.t[next])) m->cval.rp ++;
#else
			if (ttMWS(m->val.t[next].type)) m->val.art ++;
#endif
			t.type = STRING;
#ifdef LOW_MEM
			anum = m->cval.t[++ m->cval.rp];
			if (anum >= 128U) anum = ((anum & 127U) << 8)
				| (unsigned)m->cval.t[++ m->cval.rp];
			t.name = stringify(atl + anum);
			m->cval.rp ++;
#else
			t.name = stringify(atl +
				(size_t)(m->val.t[m->val.art ++].line));
#endif
			throw_away(ls->gf, t.name);
			ct = &t;
			/*
			 * There is no need for extra spaces here.
			 */
		}
		t = *ct;
		/* Replacement-list tokens inherit the expansion location;
		   tokens supplied by arguments keep their own line. */
		t.line = l;
		ZAP_LINE(t);
		aol(etl.t, etl.nt, t, TOKEN_LIST_MEMG);
	}
#ifdef LOW_MEM
	m->cval.rp = save_art;
#else
	m->val.art = save_art;
#endif

	/*
	 * Now etl contains the expanded macro, to be parsed again for
	 * further expansions -- much easier, since '#' and '##' have
	 * already been handled.
	 * However, we might need some input from tfi. So, we paste
	 * the contents of tfi after etl, and we put back what was
	 * not used.
	 *
	 * Some adjacent spaces are merged; only unique NONE, or sequences
	 * OPT_NONE NONE are emitted.
	 */
	etl_limit = etl.nt;
	if (tfi) {
		save_tfi = tfi->art;
		while (tfi->art < tfi->nt) aol(etl.t, etl.nt,
			tfi->t[tfi->art ++], TOKEN_LIST_MEMG);
	}
	ltwws = 0;
	while (etl.art < etl_limit) {
		struct macro *nm;

		ct = etl.t + (etl.art ++);
		if (ct->type == NAME && ct->line >= 0
			&& (nm = get_macro(ct->name))) {
			if (substitute_macro(ls, nm, ct->name, &etl,
				penury, reject_nested, ct->line ? ct->line : l)) {
				m->nest = save_nest;
				goto exit_error_2;
			}
			ltwws = 0;
			continue;
		}
		if (ttMWS(ct->type)) {
			if (ltwws == 1) {
				if (ct->type == OPT_NONE) continue;
				ltwws = 2;
			} else if (ltwws == 2) continue;
			else if (ct->type == OPT_NONE) ltwws = 1;
			else ltwws = 2;
		} else ltwws = 0;
		print_token(ls, ct, reject_nested ? 0 : l);
	}
	if (etl.nt) freemem(etl.t);
	if (tfi) {
		tfi->art = save_tfi + (etl.art - etl_limit);
	}

exit_macro_1:
	print_space(ls);
exit_macro_2:
	for (i = 0; i < (m->narg + m->vaarg); i ++)
		if (atl[i].nt) freemem(atl[i].t);
	if (m->narg > 0 || m->vaarg) freemem(atl);
	m->nest = save_nest;
	return 0;

exit_error_2:
	if (etl.nt) freemem(etl.t);
exit_error_1:
	for (i = 0; i < (m->narg + m->vaarg); i ++)
		if (atl[i].nt) freemem(atl[i].t);
	if (m->narg > 0 || m->vaarg) freemem(atl);
	m->nest = save_nest;
exit_error:
	return 1;
}

/*
 * print already defined macros
 */
void print_defines(void)
{
	HTT_scan(&macros, print_macro);
}

/*
 * define_macro() defines a new macro, whom definition is given in
 * the command-line syntax: macro=def
 * The '=def' part is optional.
 *
 * It returns non-zero on error.
 */
int define_macro(struct lexer_state *ls, char *def)
{
	struct lexer_state lls;
	const char *eq = strchr(def, '=');
	size_t n = strlen(def);
	char *c;
	int ret;

	if (!n || eq == def) {
		error(-1, "void macro name");
		return 1;
	}
	c = getmem(n + 3);
	mmv(c, def, n);
	if (eq) c[eq - def] = ' ';
	else {
		c[n++] = ' ';
		c[n++] = '1';
	}
	c[n++] = '\n';
	init_buf_lexer_state(&lls, 0);
	lls.flags = ls->flags | LEXER;
	lls.input_string = (unsigned char *)c;
	lls.ebuf = n;
	lls.line = -1;
	ret = handle_define(&lls) || lls.character_error;
	free_lexer_state(&lls);
	freemem(c);
	return ret;
}

/*
 * undef_macro() undefines the macro whom name is given as "def";
 * it is not an error to try to undef a macro that does not exist.
 *
 * It returns non-zero on error (undefinition of a special macro,
 * void macro name).
 */
int undef_macro(struct lexer_state *ls, char *def)
{
	struct lexer_state lls;
	int ret = 0;

	if (!*def) {
		error(-1, "void macro name");
		return 1;
	}
	init_buf_lexer_state(&lls, 0);
	lls.flags = ls->flags | LEXER;
	lls.input_string = (unsigned char *)def;
	lls.ebuf = strlen(def);
	lls.line = -1;
	if (next_token(&lls) || lls.ctok->type != NAME
		|| lls.pending_token || lls.last >= 0) {
		error(-1, "invalid macro name for undefinition");
		ret = 1;
	} else if (!lls.character_error) {
		if (reserved_macro_name(lls.ctok->name)) {
			error(-1, "trying to undef special macro %s", lls.ctok->name);
			ret = 1;
		} else remove_macro(lls.ctok->name);
	}
	ret |= lls.character_error;
	free_lexer_state(&lls);
	return ret;
}

/* #ifdef, #ifndef, and #undef each take exactly one unexpanded identifier. */
static char *directive_macro_name(struct lexer_state *ls, char *directive)
{
	long line = ls->line;
	int status, trailing = 0;
	char *name = 0;

	do status = next_token(ls);
	while (!status && ttMWS(ls->ctok->type));
	if (status || ls->ctok->type != NAME) {
		if (status != 1) error(line, "expected a macro name for #%s", directive);
		if (!status && ls->ctok->type != NEWLINE)
			while (!next_token(ls) && ls->ctok->type != NEWLINE);
		return 0;
	}

	name = sdup(ls->ctok->name);
	while (!(status = next_token(ls)) && ls->ctok->type != NEWLINE)
		if (!ttMWS(ls->ctok->type)) trailing = 1;
	if (trailing) {
		if (ls->flags & C11_CONSTRAINTS)
			error(line, "trailing tokens in #%s", directive);
		else if (ls->flags & WARN_STANDARD)
			warning(line, "trailing garbage in #%s", directive);
	}
	if (status == 1 || (trailing && (ls->flags & C11_CONSTRAINTS))) {
		freemem(name);
		return 0;
	}
	return name;
}

int handle_ifdef(struct lexer_state *ls)
{
	char *name = directive_macro_name(ls, "ifdef");
	int defined;
	if (!name) return -1;
	defined = get_macro(name) != 0;
	freemem(name);
	return defined;
}

int handle_undef(struct lexer_state *ls)
{
	long line = ls->line;
	char *name = directive_macro_name(ls, "undef");
	int ret = 0;
	if (!name) return 1;
	if (reserved_macro_name(name)) {
		error(line, "trying to undef special macro %s", name);
		ret = 1;
	} else if (get_macro(name)) {
		if (emit_defines) fprintf(emit_output, "#undef %s\n", name);
		remove_macro(name);
	}
	freemem(name);
	return ret;
}

int handle_ifndef(struct lexer_state *ls)
{
	char *name = directive_macro_name(ls, "ifndef");
	int undefined;
	if (!name) return -1;
	undefined = get_macro(name) == 0;
	if (protect_detect.state == 1) {
		protect_detect.state = 2;
		protect_detect.macro = name;
	} else {
		freemem(name);
	}
	return undefined;
}

/*
 * erase the macro table.
 */
void wipe_macros(void)
{
	if (macros_init_done) HTT_kill(&macros);
	macros_init_done = 0;
}

/*
 * initialize the macro table
 */
void init_macros(void)
{
	wipe_macros();
	HTT_init(&macros, del_macro);
	macros_init_done = 1;
	if (!no_special_macros) add_special_macros();
}

/*
 * find a macro from its name
 */
struct macro *get_macro(char *name)
{
	char *key = identifier_key(name);
	struct macro *m = HTT_get(&macros, key);
	if (key != name) freemem(key);
	return m;
}
