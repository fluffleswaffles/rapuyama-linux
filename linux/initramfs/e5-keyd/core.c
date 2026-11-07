/* e5-keyd core: the Fn one-shot state (mirrors the console's sticky SCtrlR:
 * Fn toggles, any other key press ends it), the power key screen toggle, and
 * the key picture and its fonts (psf files or the console font). No I/O
 * here (host tests: test_core.c). */
#include <string.h>
#include "keyd.h"
#include "layer.h"	/* keyd_cells, keyd_navline (from tools/e5_keys.py) */

unsigned keyd_event(struct keyd_state *s, int code, int value)
{
	unsigned a = 0;

	if (value != 1)		/* releases and autorepeat change nothing */
		return 0;
	if (s->blanked) {	/* any key wakes the screen */
		s->blanked = false;
		a |= KEYD_UNBLANK;
		if (code == KEYD_POWER_CODE)
			return a;
	}
	if (code == KEYD_FN_CODE) {
		s->fn = !s->fn;
		return a | (s->fn ? KEYD_SHOW : KEYD_HIDE);
	}
	if (code == KEYD_POWER_CODE) {
		/* grabbed by main.c: the console never sees it, its sticky Fn stays */
		s->blanked = true;
		return a | KEYD_BLANK;
	}
	if (s->fn) {		/* the console clears its sticky Fn on any key */
		s->fn = false;
		a |= KEYD_HIDE;
	}
	return a;
}

bool keyd_power_edge(struct keyd_state *s, long ms)
{
	bool act = !s->power_ms || ms - s->power_ms > KEYD_POWER_GAP_MS;

	s->power_ms = ms;
	return act;
}

typedef uint32_t picture[KEYD_H][KEYD_W];

static void fill(picture px, int x0, int y0, int x1, int y1, uint32_t col)
{
	for (int y = y0; y < y1; y++)
		for (int x = x0; x < x1; x++)
			px[y][x] = col;
}

static unsigned le32(const unsigned char *p)
{
	return p[0] | p[1] << 8 | p[2] << 16 | (unsigned)p[3] << 24;
}

/* glyph number -> ASCII: the first plain mapping wins (psf2h.py did the
 * same); characters the table leaves out keep glyph = code */
static void ascii(struct keyd_font *f, const unsigned char *g0, size_t size,
		  unsigned n, unsigned c, unsigned glyph, bool *seen)
{
	if (c >= 32 && c < 127 && glyph < n && !seen[c - 32]) {
		f->g[c - 32] = g0 + glyph * size;
		seen[c - 32] = true;
	}
}

int keyd_font_psf(struct keyd_font *f, const unsigned char *buf, size_t len)
{
	const unsigned char *g0, *t, *end = buf + len;
	unsigned n, h, size, uni;
	bool seen[95] = { false };

	if (len >= 4 && buf[0] == 0x36 && buf[1] == 0x04) {
		n = buf[2] & 1 ? 512 : 256;
		h = size = buf[3];
		uni = buf[2] & 2;
		g0 = buf + 4;
	} else if (len >= 32 && le32(buf) == 0x864ab572) {
		n = le32(buf + 16);
		size = le32(buf + 20);
		h = le32(buf + 24);
		uni = le32(buf + 12) & 1;
		if (le32(buf + 28) > 8 || size != h || le32(buf + 8) > len)
			return -1;
		g0 = buf + le32(buf + 8);
	} else {
		return -1;
	}
	if (!h || h > 32 || n > 1024 || (size_t)(end - g0) < (size_t)n * size)
		return -1;
	t = g0 + (size_t)n * size;
	f->h = h;
	for (unsigned i = 0; i < 95; i++)
		f->g[i] = NULL;
	/* unicode table: per glyph, its characters, then sequences, then end */
	for (unsigned glyph = 0; uni && glyph < n; glyph++) {
		bool seq = false;

		if (buf[0] == 0x36) {		/* psf1: u16, 0xfffe seq, 0xffff end */
			for (;; t += 2) {
				if (end - t < 2)
					return -1;
				unsigned u = t[0] | t[1] << 8;

				if (u == 0xffff)
					break;
				seq |= u == 0xfffe;
				if (!seq)
					ascii(f, g0, size, n, u, glyph, seen);
			}
			t += 2;
		} else {			/* psf2: utf-8, 0xfe seq, 0xff end */
			for (;; t++) {
				if (t >= end)
					return -1;
				if (*t == 0xff)
					break;
				seq |= *t == 0xfe;
				if (!seq && *t < 0x80)
					ascii(f, g0, size, n, *t, glyph, seen);
			}
			t++;
		}
	}
	for (unsigned c = 32; c < 127; c++)
		ascii(f, g0, size, n, c, c, seen);
	return 0;
}

int keyd_font_console(struct keyd_font *f, const unsigned char *data,
		      unsigned w, unsigned h, unsigned count)
{
	if (w > 8 || !h || h > 32)
		return -1;
	f->h = h;
	for (unsigned c = 32; c < 127; c++)
		f->g[c - 32] = c < count ? data + c * 32 : NULL;
	return 0;
}

/* text h px tall: glyph rows scaled to h (an 8 px glyph doubled for 16) */
static void text(picture px, int x, int y, const char *s, int h,
		 const struct keyd_font *f, uint32_t col)
{
	for (; f && *s; s++, x += 8) {
		const unsigned char *g = *s >= 32 && *s < 127 ? f->g[*s - 32] : NULL;

		for (int r = 0; g && r < h; r++) {
			unsigned bits = g[r * f->h / h];

			for (int b = 0; b < 8; b++)
				if (bits & (0x80 >> b) && x + b < KEYD_W && y + r < KEYD_H)
					px[y + r][x + b] = col;
		}
	}
}

static int centre(int x, int w, const char *s) { return x + (w - 8 * (int)strlen(s)) / 2; }

void keyd_render(picture px, const struct keyd_font *big,
		 const struct keyd_font *small)
{
	fill(px, 0, 0, KEYD_W, KEYD_H, KEYD_PANEL);
	fill(px, 0, 0, KEYD_W, KEYD_TOP, KEYD_AMBER);
	for (int r = 0; r < 3; r++) {
		for (int c = 0; c < 10; c++) {
			const struct keyd_cell *k = &keyd_cells[r][c];
			int x = c * KEYD_CELL_W, y = KEYD_TOP + r * KEYD_CELL_H;
			bool empty = !k->sym[0];

			fill(px, x + 1, y + 1, x + KEYD_CELL_W - 1, y + KEYD_CELL_H - 1, KEYD_BORDER);
			fill(px, x + 2, y + 2, x + KEYD_CELL_W - 2, y + KEYD_CELL_H - 2, KEYD_KEY);
			if (k->accent)
				fill(px, x + 1, y + 1, x + KEYD_CELL_W - 1, y + 3, KEYD_AMBER);
			if (!empty)
				text(px, centre(x, KEYD_CELL_W, k->sym), y + 5, k->sym, 16,
				     big, KEYD_WHITE);
			/* empty keys: the letter only, at half brightness */
			text(px, centre(x, KEYD_CELL_W, k->label), y + 24, k->label, 8, small,
			     empty ? (KEYD_AMBER >> 1) & 0x7f7f7fu : KEYD_AMBER);
		}
	}
	text(px, centre(0, KEYD_W, keyd_navline), KEYD_NAV_Y, keyd_navline, 8,
	     small, KEYD_WHITE);
}
