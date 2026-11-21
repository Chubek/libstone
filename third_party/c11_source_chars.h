#ifndef C11_SOURCE_CHARS_H
#define C11_SOURCE_CHARS_H

/* Shared character rules for ucpp (phases 1–4) and cparser (phases 5–7). */
static inline int c11_hex_value(int c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

static inline int c11_valid_ucn(unsigned long c)
{
	/* 6.4.3, with ISO/IEC 10646 code space bounded at U+10FFFF. */
	return (c >= 0xa0 || c == '$' || c == '@' || c == '`')
		&& (c < 0xd800 || c > 0xdfff) && c <= 0x10ffff;
}

static inline int c11_identifier_ucn(unsigned long c)
{
	/* Annex D.1: deliberately not Unicode's XID_Continue. */
	static const unsigned long ranges[][2] = {
		{0x00a8, 0x00a8}, {0x00aa, 0x00aa}, {0x00ad, 0x00ad},
		{0x00af, 0x00af}, {0x00b2, 0x00b5}, {0x00b7, 0x00ba},
		{0x00bc, 0x00be}, {0x00c0, 0x00d6}, {0x00d8, 0x00f6},
		{0x00f8, 0x00ff}, {0x0100, 0x167f}, {0x1681, 0x180d},
		{0x180f, 0x1fff}, {0x200b, 0x200d}, {0x202a, 0x202e},
		{0x203f, 0x2040}, {0x2054, 0x2054}, {0x2060, 0x206f},
		{0x2070, 0x218f}, {0x2460, 0x24ff}, {0x2776, 0x2793},
		{0x2c00, 0x2dff}, {0x2e80, 0x2fff}, {0x3004, 0x3007},
		{0x3021, 0x302f}, {0x3031, 0x303f}, {0x3040, 0xd7ff},
		{0xf900, 0xfd3d}, {0xfd40, 0xfdcf}, {0xfdf0, 0xfe44},
		{0xfe47, 0xfffd}
	};
	unsigned i;
	if (c >= 0x10000)
		return c <= 0xefffd && (c & 0xffff) <= 0xfffd;
	for (i = 0; i < sizeof ranges / sizeof ranges[0]; i++) {
		if (c < ranges[i][0]) return 0;
		if (c <= ranges[i][1]) return 1;
	}
	return 0;
}

static inline int c11_initial_ucn(unsigned long c)
{
	/* Annex D.2. The caller has already checked D.1. */
	return !((c >= 0x0300 && c <= 0x036f) ||
	         (c >= 0x1dc0 && c <= 0x1dff) ||
	         (c >= 0x20d0 && c <= 0x20ff) ||
	         (c >= 0xfe20 && c <= 0xfe2f));
}

/* Decode a Unicode scalar from UTF-8. Reject overlong encodings, surrogates
   and values outside the code space. */
struct c11_utf8_state {
	unsigned long value;
	unsigned remaining, low, high;
};

/* Return 1 at a scalar boundary, 0 for an incomplete scalar, -1 on error. */
static inline int c11_utf8_feed(struct c11_utf8_state *state, unsigned c)
{
	if (state->remaining) {
		if (c < state->low || c > state->high) {
			state->remaining = 0;
			return -1;
		}
		state->value = (state->value << 6) | (c & 0x3f);
		state->low = 0x80;
		state->high = 0xbf;
		return --state->remaining == 0;
	}
	if (c < 0x80) {
		state->value = c;
		return 1;
	}
	if (c >= 0xc2 && c <= 0xdf) {
		state->value = c & 0x1f;
		state->remaining = 1;
	} else if (c >= 0xe0 && c <= 0xef) {
		state->value = c & 0x0f;
		state->remaining = 2;
	} else if (c >= 0xf0 && c <= 0xf4) {
		state->value = c & 7;
		state->remaining = 3;
	} else return -1;
	state->low = c == 0xe0 ? 0xa0 : c == 0xf0 ? 0x90 : 0x80;
	state->high = c == 0xed ? 0x9f : c == 0xf4 ? 0x8f : 0xbf;
	return 0;
}

static inline unsigned c11_encode_utf8(unsigned long c, char out[4])
{
	unsigned n, i;
	if (c < 0x80) {
		out[0] = (char)c;
		return 1;
	}
	n = c < 0x800 ? 2 : c < 0x10000 ? 3 : 4;
	for (i = n - 1; i > 0; i--) {
		out[i] = (char)(0x80 | (c & 0x3f));
		c >>= 6;
	}
	out[0] = (char)((n == 2 ? 0xc0 : n == 3 ? 0xe0 : 0xf0) | c);
	return n;
}

#endif
