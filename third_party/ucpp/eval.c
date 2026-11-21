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
#include <setjmp.h>
#include <limits.h>
#include "ucppi.h"
#include "mem.h"

JMP_BUF eval_exception;
long eval_line;
static int emit_eval_warnings;
static int eval_utf8_source;
static int eval_char_signed;
static int eval_wchar_signed;

/*
 * If you want to hardcode a conversion table, define a static array
 * of 256 int, and make transient_characters point to it.
 */
int *transient_characters = 0;

#define OCTAL(x)       ((x) >= '0' && (x) <= '7')
#define DECIM(x)       ((x) >= '0' && (x) <= '9')
#define HEXAD(x)       (DECIM(x) \
                       || (x) == 'a' || (x) == 'b' || (x) == 'c' \
                       || (x) == 'd' || (x) == 'e' || (x) == 'f' \
                       || (x) == 'A' || (x) == 'B' || (x) == 'C' \
                       || (x) == 'D' || (x) == 'E' || (x) == 'F')
#define OVAL(x)	       ((int)((x) - '0'))
#define DVAL(x)	       ((int)((x) - '0'))
#define HVAL(x)	       (DECIM(x) ? DVAL(x) \
                       : (x) == 'a' || (x) == 'A' ? 10 \
                       : (x) == 'b' || (x) == 'B' ? 11 \
                       : (x) == 'c' || (x) == 'C' ? 12 \
                       : (x) == 'd' || (x) == 'D' ? 13 \
                       : (x) == 'e' || (x) == 'E' ? 14 : 15)

#define ARITH_TYPENAME             big
#define ARITH_FUNCTION_HEADER      static inline

#define ARITH_ERROR(type)          z_error(type)
static void z_error(int type);

#ifdef ARITHMETIC_CHECKS
#define ARITH_WARNING(type)        z_warn(type)
static void z_warn(int type);
#endif

#include "arith.c"

static void z_error(int type)
{
	switch (type) {
	case ARITH_EXCEP_SLASH_D:
		error(eval_line, "division by 0");
		break;
	case ARITH_EXCEP_SLASH_O:
		error(eval_line, "overflow on division");
		break;
	case ARITH_EXCEP_PCT_D:
		error(eval_line, "division by 0 on modulus operator");
		break;
	case ARITH_EXCEP_CONST_O:
		error(eval_line, "constant too large for destination type");
		break;
#ifdef AUDIT
	default:
		ouch("erroneous integer error: %d", type);
#endif
	}
	throw(eval_exception);
}

#ifdef ARITHMETIC_CHECKS
static void z_warn(int type)
{
	switch (type) {
	case ARITH_EXCEP_CONV_O:
		warning(eval_line, "overflow on integer conversion");
		break;
	case ARITH_EXCEP_NEG_O:
		warning(eval_line, "overflow on unary minus");
		break;
	case ARITH_EXCEP_NOT_T:
		warning(eval_line,
			"bitwise inversion yields trap representation");
		break;
	case ARITH_EXCEP_PLUS_O:
		warning(eval_line, "overflow on addition");
		break;
	case ARITH_EXCEP_PLUS_U:
		warning(eval_line, "underflow on addition");
		break;
	case ARITH_EXCEP_MINUS_O:
		warning(eval_line, "overflow on subtraction");
		break;
	case ARITH_EXCEP_MINUS_U:
		warning(eval_line, "underflow on subtraction");
		break;
	case ARITH_EXCEP_AND_T:
		warning(eval_line,
			"bitwise AND yields trap representation");
		break;
	case ARITH_EXCEP_XOR_T:
		warning(eval_line,
			"bitwise XOR yields trap representation");
		break;
	case ARITH_EXCEP_OR_T:
		warning(eval_line,
			"bitwise OR yields trap representation");
		break;
	case ARITH_EXCEP_LSH_W:
		warning(eval_line, "left shift count greater than "
			"or equal to type width");
		break;
	case ARITH_EXCEP_LSH_C:
		warning(eval_line, "left shift count negative");
		break;
	case ARITH_EXCEP_LSH_O:
		warning(eval_line, "overflow on left shift");
		break;
	case ARITH_EXCEP_RSH_W:
		warning(eval_line, "right shift count greater than "
			"or equal to type width");
		break;
	case ARITH_EXCEP_RSH_C:
		warning(eval_line, "right shift count negative");
		break;
	case ARITH_EXCEP_RSH_N:
		warning(eval_line, "right shift of negative value");
		break;
	case ARITH_EXCEP_STAR_O:
		warning(eval_line, "overflow on multiplication");
		break;
	case ARITH_EXCEP_STAR_U:
		warning(eval_line, "underflow on multiplication");
		break;
#ifdef AUDIT
	default:
		ouch("erroneous integer warning: %d", type);
#endif
	}
}
#endif

typedef struct {
	int sign;
	union {
		u_big uv;
		s_big sv;
	} u;
} ppval;

static int boolval(ppval x)
{
	return x.sign ? big_s_lval(x.u.sv) : big_u_lval(x.u.uv);
}

#if !defined(WCHAR_SIGNEDNESS)
#  if CHAR_MIN == 0
#    define WCHAR_SIGNEDNESS	0
#  else
#    define WCHAR_SIGNEDNESS	1
#  endif
#endif

/*
 * Check the suffix, return 1 if it is signed, 0 otherwise. 1 is
 * returned for a void suffix. Legal suffixes are:
 * unsigned: u U ul uL Ul UL lu Lu lU LU ull uLL Ull ULL llu LLu llU LLU
 * signed: l L ll LL
 */
static int pp_suffix(char *d, char *refc)
{
	if (!*d) return 1;
	if (*d == 'u' || *d == 'U') {
		if (!*(++ d)) return 0;
		if (*d == 'l' || *d == 'L') {
			char *e = d + 1;

			if (*e && *e != *d) goto suffix_error;
			if (!*e || !*(e + 1)) return 0;
			goto suffix_error;
		}
		goto suffix_error;
	}
	if (*d == 'l' || *d == 'L') {
		if (!*(++ d)) return 1;
		if (*d == *(d - 1)) {
			d ++;
			if (!*d) return 1;
		}
		if (*d == 'u' || *d == 'U') {
			d ++;
			if (!*d) return 0;
		}
		goto suffix_error;
	}
suffix_error:
	error(eval_line, "invalid integer constant '%s'", refc);
	throw(eval_exception);
	return 666;
}

static unsigned long pp_cchar(char **cursor, const char *refc,
	unsigned long escape_max, int *numeric)
{
	char *c = *cursor;
	unsigned long r = 0;

	*numeric = 0;
	if (*c == '\\') {
		int i;

		c ++;
		switch (*c) {
		case 'n': r = '\n'; c ++; break;
		case 't': r = '\t'; c ++; break;
		case 'v': r = '\v'; c ++; break;
		case 'b': r = '\b'; c ++; break;
		case 'r': r = '\r'; c ++; break;
		case 'f': r = '\f'; c ++; break;
		case 'a': r = '\a'; c ++; break;
		case '\\': r = '\\'; c ++; break;
		case '\?': r = '\?'; c ++; break;
		case '\'': r = '\''; c ++; break;
		case '\"': r = '\"'; c ++; break;
		case 'u':
		case 'U':
		{
			int digits = *c == 'u' ? 4 : 8;
			for (i = 0, c ++; i < digits && HEXAD(*c); i ++, c ++) {
				r = (r * 16) + HVAL(*c);
			}
			if (i != digits) {
				error(eval_line, "malformed UCN in %s", refc);
				throw(eval_exception);
			}
			if (!c11_valid_ucn(r)) {
				error(eval_line, "invalid code point in UCN in %s", refc);
				throw(eval_exception);
			}
			break;
		}
		case 'x':
			*numeric = 1;
			c ++;
			if (!HEXAD(*c)) {
				error(eval_line, "hexadecimal escape requires a digit in %s", refc);
				throw(eval_exception);
			}
			for (; HEXAD(*c); c ++) {
				if (r > (escape_max - HVAL(*c)) / 16)
					goto escape_range_error;
				r = (r * 16) + HVAL(*c);
			}
			break;
		default:
			if (OCTAL(*c)) {
				*numeric = 1;
				r = OVAL(*(c ++));
				if (OCTAL(*c)) r = (r * 8) + OVAL(*(c ++));
				if (OCTAL(*c)) r = (r * 8) + OVAL(*(c ++));
			} else {
				error(eval_line, "invalid escape sequence "
					"'\\%c'", *c);
				throw(eval_exception);
			}
		}
	} else if (eval_utf8_source && (unsigned char)*c >= 0x80) {
		struct c11_utf8_state state = {0};
		int status;
		do {
			status = c11_utf8_feed(&state, (unsigned char)*c++);
		} while (!status);
		if (status < 0) {
			error(eval_line, "invalid UTF-8 character in %s", refc);
			throw(eval_exception);
		}
		r = state.value;
	} else {
		r = *((unsigned char *)(c ++));
	}
	if (*numeric && r > escape_max) goto escape_range_error;

	if (transient_characters && r < 256) {
		r = transient_characters[(size_t)r];
	}
	*cursor = c;
	return r;

escape_range_error:
	if (escape_max == ULONG_MAX) z_error(ARITH_EXCEP_CONST_O);
	error(eval_line, "numeric escape out of range in %s", refc);
	throw(eval_exception);
	return 0;
}

static unsigned long pp_char(char *c, const char *refc, int strict, unsigned *bytes)
{
	unsigned long result = 0, escape_max = ULONG_MAX;
	int numeric, first = 1;

	*bytes = 0;
	if (strict) {
		switch (*refc) {
		case '\'': escape_max = 0xffUL; break;
		case 'u': escape_max = 0xffffUL; break;
		default: escape_max = 0xffffffffUL; break; /* L and U on LP64. */
		}
	}
	if (*++c == '\'') {
		error(eval_line, "empty character constant");
		throw(eval_exception);
	}
	if (!strict) {
		/* Preserve standalone ucpp and assembler preprocessing behavior. */
		result = pp_cchar(&c, refc, escape_max, &numeric);
		if (*c != '\'') {
			if (emit_eval_warnings) warning(eval_line, "multicharacter constant");
			while (*c && *c != '\'') pp_cchar(&c, refc, escape_max, &numeric);
		}
		return result;
	}

	/* Match Cephyr's execution encodings: ordinary constants keep the last
	   four UTF-8 bytes; prefixed constants keep the last code unit. */
	do {
		unsigned long value = pp_cchar(&c, refc, escape_max, &numeric);
		if (first && emit_eval_warnings && *c && *c != '\'')
			warning(eval_line, "multicharacter constant");
		first = 0;
		if (*refc == '\'') {
			char encoded[4];
			unsigned i, count = 1;
			if (numeric) encoded[0] = (char)value;
			else count = c11_encode_utf8(value, encoded);
			for (i = 0; i < count; i ++)
				result = ((result << 8) | (unsigned char)encoded[i]) & 0xffffffffUL;
			if (*bytes < 2) *bytes += count;
		} else {
			result = *refc == 'u' && !numeric && value > 0xffffUL
				? 0xdc00UL + ((value - 0x10000UL) & 0x3ffUL) : value;
		}
	} while (*c && *c != '\'');
	return result;
}

static ppval pp_strtoconst(char *refc, int strict)
{
	ppval q;
	char *c = refc, *d;
	u_big ru;
	s_big rs;
	int sp, dec;

	if (*c == '\'' || *c == 'L' || *c == 'u' || *c == 'U') {
		unsigned bytes;
		unsigned long value;
		q.sign = *c == '\'' || (*c == 'L' &&
			(strict ? eval_wchar_signed : WCHAR_SIGNEDNESS));
		if (*c != '\'' && *(++ c) != '\'') {
			error(eval_line,
				"invalid wide character constant: %s", refc);
			throw(eval_exception);
		}
		value = pp_char(c, refc, strict, &bytes);
		if (q.sign) {
			long signed_value;
			if (strict && bytes == 1 && eval_char_signed && value >= 0x80UL)
				signed_value = -1L - (long)(0xffUL - value);
			else if (strict && value >= 0x80000000UL)
				signed_value = -1L - (long)(0xffffffffUL - value);
			else
				signed_value = (long)value;
			q.u.sv = big_s_fromlong(signed_value);
		} else {
			q.u.uv = big_u_fromulong(value);
		}
		return q;
	}
	if (*c == '0') {
		/* octal or hexadecimal */
		dec = 0;
		c ++;
		if (*c == 'x' || *c == 'X') {
			c ++;
			if (!HEXAD(*c)) {
				error(eval_line, "invalid integer constant '%s'", refc);
				throw(eval_exception);
			}
			d = big_u_hexconst(c, &ru, &rs, &sp);
		} else {
			d = big_u_octconst(c, &ru, &rs, &sp);
		}
	} else {
		dec = 1;
		d = big_u_decconst(c, &ru, &rs, &sp);
	}
	q.sign = pp_suffix(d, refc);
	if (q.sign) {
		if (!sp) {
			if (dec) {
				error(eval_line, "constant too large "
					"for destination type");
				throw(eval_exception);
			} else if (!strict) {
				warning(eval_line, "constant is so large "
					"that it is unsigned");
			}
			q.u.uv = ru;
			q.sign = 0;
		} else {
			q.u.sv = rs;
		}
	} else {
		q.u.uv = ru;
	}
	return q;
}

/*
 * Used by #line directives -- anything beyond what can be put in an
 * unsigned long, is considered absurd.
 */
unsigned long strtoconst(char *c)
{
	ppval q = pp_strtoconst(c, 0);

	if (q.sign) q.u.uv = big_s_to_u(q.u.sv);
	return big_u_toulong(q.u.uv);
}

#define OP_UN(x)	((x) == LNOT || (x) == NOT || (x) == UPLUS \
			|| (x) == UMINUS)

static ppval eval_opun(int op, ppval v, int strict)
{
	if (op == LNOT) {
		int nonzero = boolval(v);
		v.sign = 1;
		v.u.sv = big_s_fromint(!nonzero);
		return v;
	}
	if (v.sign) {
#ifdef NATIVE_SIGNED
		if (strict && op == UMINUS && v.u.sv == NATIVE_SIGNED_MIN) {
			error(eval_line, "overflow on unary minus");
			throw(eval_exception);
		}
#endif
		switch (op) {
		case NOT: v.u.sv = big_s_not(v.u.sv); break;
		case UPLUS: break;
		case UMINUS: v.u.sv = big_s_neg(v.u.sv); break;
		}
	} else {
		switch (op) {
		case NOT: v.u.uv = big_u_not(v.u.uv); break;
		case UPLUS: break;
		case UMINUS: v.u.uv = big_u_neg(v.u.uv); break;
		}
	}
	return v;
}

#define OP_BIN(x)      ((x) == STAR || (x) == SLASH || (x) == PCT \
                       || (x) == PLUS || (x) == MINUS || (x) == LSH \
                       || (x) == RSH || (x) == LT || (x) == LEQ \
                       || (x) == GT || (x) == GEQ || (x) == SAME \
                       || (x) == NEQ || (x) == AND || (x) == CIRC \
                       || (x) == OR || (x) == LAND || (x) == LOR \
                       || (x) == COMMA)

static int bin_sign(int op, int left, int right)
{
	switch (op) {
	case LSH: case RSH: return left;
	case COMMA: return right;
	case STAR: case SLASH: case PCT: case PLUS: case MINUS:
	case AND: case CIRC: case OR:
		return left && right;
	default: return 1; /* Comparison and logical operators yield int. */
	}
}

#ifdef NATIVE_SIGNED
/* Do not execute host operations whose C11 constant value is undefined or
   out of range. Short-circuited operands never reach this function. */
static void check_native_binop(int op, ppval left, ppval right, int shift)
{
	s_big x, y;
	int overflow = 0;

	if ((op == LSH || op == RSH) &&
		(shift < 0 || shift >= NATIVE_UNSIGNED_BITS)) {
		error(eval_line, "shift count out of range in #if expression");
		throw(eval_exception);
	}
	if (!left.sign || (!right.sign && op != LSH)) return;
	x = left.u.sv;
	switch (op) {
	case LSH:
		overflow = x < 0 || x > (NATIVE_SIGNED_MAX >> shift);
		break;
	case PLUS:
		y = right.u.sv;
		overflow = (y > 0 && x > NATIVE_SIGNED_MAX - y)
			|| (y < 0 && x < NATIVE_SIGNED_MIN - y);
		break;
	case MINUS:
		y = right.u.sv;
		overflow = (y < 0 && x > NATIVE_SIGNED_MAX + y)
			|| (y > 0 && x < NATIVE_SIGNED_MIN + y);
		break;
	case STAR:
		y = right.u.sv;
		if (x > 0)
			overflow = y > NATIVE_SIGNED_MAX / x || y < NATIVE_SIGNED_MIN / x;
		else if (x < 0)
			overflow = (y > 0 && x < NATIVE_SIGNED_MIN / y)
				|| (y < 0 && x < NATIVE_SIGNED_MAX / y);
		break;
	}
	if (overflow) {
		error(eval_line, "signed overflow on '%s' in #if expression", operators_name[op]);
		throw(eval_exception);
	}
}
#endif

static ppval eval_opbin(int op, ppval v1, ppval v2, int strict)
{
	ppval r;
	int iv2 = 0;

	switch (op) {
	case STAR:	case SLASH:	case PCT:
	case PLUS:	case MINUS:	case AND:
	case CIRC:	case OR:
		/* promote operands, adjust signedness of result */
		if (!v1.sign || !v2.sign) {
			if (v1.sign) {
				v1.u.uv = big_s_to_u(v1.u.sv);
				v1.sign = 0;
			} else if (v2.sign) {
				v2.u.uv = big_s_to_u(v2.u.sv);
				v2.sign = 0;
			}
			r.sign = 0;
		} else {
			r.sign = 1;
		}
		break;
	case LT:	case LEQ:	case GT:
	case GEQ:	case SAME:	case NEQ:
		/* promote operands */
		if (!v1.sign || !v2.sign) {
			if (v1.sign) {
				v1.u.uv = big_s_to_u(v1.u.sv);
				v1.sign = 0;
			} else if (v2.sign) {
				v2.u.uv = big_s_to_u(v2.u.sv);
				v2.sign = 0;
			}
		}
		/* fall through */
	case LAND:
	case LOR:
		/* result is signed anyway */
		r.sign = 1;
		break;
	case LSH:
	case RSH:
		/* result is as signed as left operand; convert right
		   operand to int */
		r.sign = v1.sign;
		if (v2.sign) {
			iv2 = big_s_toint(v2.u.sv);
		} else {
			iv2 = big_u_toint(v2.u.uv);
		}
		break;
	case COMMA:
		if (strict) {
			error(eval_line, "evaluated comma operator in #if expression");
			throw(eval_exception);
		}
		if (emit_eval_warnings) {
			warning(eval_line, "ISO C forbids evaluated comma "
				"operators in #if expressions");
		}
		r.sign = v2.sign;
		break;
#ifdef AUDIT
	default: ouch("a good operator is a dead operator");
#endif
	}

#ifdef NATIVE_SIGNED
	if (strict) {
		check_native_binop(op, v1, v2, iv2);
		/* Cephyr documents arithmetic right shift, independent of host. */
		if (op == RSH && v1.sign && v1.u.sv < 0) {
			r.u.sv = -1 - ((-1 - v1.u.sv) >> iv2);
			return r;
		}
	}
#endif

#define SBINOP(x)	if (r.sign) r.u.sv = big_s_ ## x (v1.u.sv, v2.u.sv); \
			else r.u.uv = big_u_ ## x (v1.u.uv, v2.u.uv);

#define NSSBINOP(x)	if (v1.sign) r.u.sv = big_s_fromint(big_s_ ## x \
			(v1.u.sv, v2.u.sv)); else r.u.sv = big_s_fromint( \
			big_u_ ## x (v1.u.uv, v2.u.uv));

#define LBINOP(x)	r.u.sv = big_s_fromint(boolval(v1) x boolval(v2));

#define ABINOP(x)	if (r.sign) r.u.sv = big_s_ ## x (v1.u.sv, iv2); \
			else r.u.uv = big_u_ ## x (v1.u.uv, iv2);

	switch (op) {
	case STAR: SBINOP(star); break;
	case SLASH: SBINOP(slash); break;
	case PCT: SBINOP(pct); break;
	case PLUS: SBINOP(plus); break;
	case MINUS: SBINOP(minus); break;
	case LSH: ABINOP(lsh); break;
	case RSH: ABINOP(rsh); break;
	case LT: NSSBINOP(lt); break;
	case LEQ: NSSBINOP(leq); break;
	case GT: NSSBINOP(gt); break;
	case GEQ: NSSBINOP(geq); break;
	case SAME: NSSBINOP(same); break;
	case NEQ: NSSBINOP(neq); break;
	case AND: SBINOP(and); break;
	case CIRC: SBINOP(xor); break;
	case OR: SBINOP(or); break;
	case LAND: LBINOP(&&); break;
	case LOR: LBINOP(||); break;
	case COMMA: r = v2; break;
	}
	return r;
}

#define ttOP(x)		(OP_UN(x) || OP_BIN(x) || (x) == QUEST || (x) == COLON)

static int op_prec(int op)
{
	switch (op) {
	case LNOT:
	case NOT:
	case UPLUS:
	case UMINUS:
		return 13;
	case STAR:
	case SLASH:
	case PCT:
		return 12;
	case PLUS:
	case MINUS:
		return 11;
	case LSH:
	case RSH:
		return 10;
	case LT:
	case LEQ:
	case GT:
	case GEQ:
		return 9;
	case SAME:
	case NEQ:
		return 8;
	case AND:
		return 7;
	case CIRC:
		return 6;
	case OR:
		return 5;
	case LAND:
		return 4;
	case LOR:
		return 3;
	case QUEST:
		return 2;
	case COMMA:
		return 1;
	}
#ifdef AUDIT
	ouch("an unknown species should have a higher precedence");
#endif
	return 666;
}

/*
 * Perform the hard work of evaluation.
 *
 * This function works because:
 * -- all unary operators are right to left associative, and with
 *    identical precedence
 * -- all binary operators are left to right associative
 * -- there is only one non-unary and non-binary operator: the quest-colon
 *
 * If do_eval is 0, still parse and determine the expression's type, but do
 * not perform arithmetic. An unselected arm of ?: contributes its type.
 */
static ppval eval_shrd(struct token_fifo *tf, int minprec, int do_eval, int strict)
{
	ppval top;
	struct token *ct;

	top.sign = 1;
	if (tf->art == tf->nt) goto trunc_err;
	ct = tf->t + (tf->art ++);
	if (ct->type == LPAR) {
		top = eval_shrd(tf, 0, do_eval, strict);
		if (tf->art == tf->nt) goto trunc_err;
		ct = tf->t + (tf->art ++);
		if (ct->type != RPAR) {
			error(eval_line, "a right parenthesis was expected");
			throw(eval_exception);
		}
	} else if (ct->type == NUMBER || ct->type == CHAR) {
		top = pp_strtoconst(ct->name, strict);
	} else if (OP_UN(ct->type)) {
		top = eval_shrd(tf, op_prec(ct->type), do_eval, strict);
		if (do_eval) top = eval_opun(ct->type, top, strict);
		else if (ct->type == LNOT) top.sign = 1;
		goto eval_loop;
	} else if (ttOP(ct->type)) goto rogue_op_err;
	else {
		goto invalid_token_err;
	}

eval_loop:
	if (tf->art == tf->nt) {
		return top;
	}
	ct = tf->t + (tf->art ++);
	if (OP_BIN(ct->type)) {
		int bp = op_prec(ct->type);

		if (bp > minprec) {
			ppval tr;

			int op = ct->type;
			if (do_eval && ((op == LOR && boolval(top))
				|| (op == LAND && !boolval(top)))) {
				eval_shrd(tf, bp, 0, strict);
				top.sign = 1;
				top.u.sv = big_s_fromint(op == LOR);
			} else {
				tr = eval_shrd(tf, bp, do_eval, strict);
				if (do_eval)
					top = eval_opbin(op, top, tr, strict);
				else
					top.sign = bin_sign(op, top.sign, tr.sign);
			}
			goto eval_loop;
		}
	} else if (ct->type == QUEST) {
		int bp = op_prec(QUEST);
		ppval r1, r2;

		if (bp >= minprec) {
			int qv = do_eval && boolval(top);
			int sign;

			/* The middle operand has the grammar of an expression,
			   not a conditional-expression (it may contain commas). */
			r1 = eval_shrd(tf, 0, qv, strict);
			if (tf->art == tf->nt) goto trunc_err;
			ct = tf->t + (tf->art ++);
			if (ct->type != COLON) {
				error(eval_line, "a colon was expected");
				throw(eval_exception);
			}
			r2 = eval_shrd(tf, bp, do_eval && !qv, strict);
			sign = r1.sign && r2.sign;
			if (do_eval) {
				top = qv ? r1 : r2;
				if (top.sign && !sign)
					top.u.uv = big_s_to_u(top.u.sv);
			}
			top.sign = sign;
			goto eval_loop;
		}
	}
	tf->art --;
	return top;

trunc_err:
	error(eval_line, "truncated constant integral expression");
	throw(eval_exception);
rogue_op_err:
	error(eval_line, "rogue operator '%s' in constant integral "
		"expression", operators_name[ct->type]);
	throw(eval_exception);
invalid_token_err:
	error(eval_line, "invalid token in constant integral expression");
	throw(eval_exception);
}

#define UNARY(x)	((x) != NUMBER && (x) != NAME && (x) != CHAR \
			&& (x) != RPAR)

/*
 * Evaluate the integer expression contained in the given token_fifo.
 * Evaluation is made by precedence of operators, as described in the
 * Dragon Book. The unary + and - are distinguished from their binary
 * counterparts using the Fortran way: a + or a - is considered unary
 * if it does not follow a constant, an identifier or a right parenthesis.
 */
unsigned long eval_expr(struct token_fifo *tf, int *ret, unsigned long flags)
{
	size_t sart;
	ppval r;

	emit_eval_warnings = (flags & WARN_STANDARD) != 0;
	eval_utf8_source = (flags & UTF8_SOURCE) != 0;
	eval_char_signed = (flags & PP_CHAR_SIGNED) != 0;
	eval_wchar_signed = (flags & PP_WCHAR_UNSIGNED) == 0;
	if (catch(eval_exception)) goto eval_err;
	/* first, distinguish unary + and - from binary + and - */
	for (sart = tf->art; tf->art < tf->nt; tf->art ++) {
		if (tf->t[tf->art].type == PLUS) {
			if (sart == tf->art || UNARY(tf->t[tf->art - 1].type))
				tf->t[tf->art].type = UPLUS;
		} else if (tf->t[tf->art].type == MINUS) {
			if (sart == tf->art || UNARY(tf->t[tf->art - 1].type))
				tf->t[tf->art].type = UMINUS;
		}
	}
	tf->art = sart;
	r = eval_shrd(tf, (flags & C11_CONSTRAINTS) ? op_prec(COMMA) : 0,
		1, (flags & C11_CONSTRAINTS) != 0);
	if (tf->art < tf->nt) {
		error(eval_line, "trailing garbage in constant integral "
			"expression");
		goto eval_err;
	}
	*ret = 0;
	return boolval(r);
eval_err:
	*ret = 1;
	return 0;
}
