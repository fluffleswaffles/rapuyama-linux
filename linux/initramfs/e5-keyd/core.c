/* e5-keyd core: the Fn one-shot state (mirrors the console's sticky SCtrlR:
 * Fn toggles, any other key press ends it), the power key screen toggle, and
 * the key picture. No I/O here (host tests: test_core.c). */
#include <string.h>
#include "keyd.h"
#include "font.h"	/* font16 (Terminus 8x16), font8 (VGA 8x8) */
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

/* the letters are "bold" by the font: VGA 8x8 has 2 px strokes */
static void text(picture px, int x, int y, const char *s, int h, uint32_t col)
{
	for (; *s; s++, x += 8) {
		const unsigned char *g = *s >= 32 && *s < 127 ?
			(h == 16 ? font16[*s - 32] : font8[*s - 32]) : NULL;

		for (int r = 0; g && r < h; r++) {
			unsigned bits = g[r];

			for (int b = 0; b < 8; b++)
				if (bits & (0x80 >> b) && x + b < KEYD_W && y + r < KEYD_H)
					px[y + r][x + b] = col;
		}
	}
}

static int centre(int x, int w, const char *s) { return x + (w - 8 * (int)strlen(s)) / 2; }

void keyd_render(picture px)
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
				     KEYD_WHITE);
			/* empty keys: the letter only, at half brightness */
			text(px, centre(x, KEYD_CELL_W, k->label), y + 24, k->label, 8,
			     empty ? (KEYD_AMBER >> 1) & 0x7f7f7fu : KEYD_AMBER);
		}
	}
	text(px, centre(0, KEYD_W, keyd_navline), KEYD_NAV_Y, keyd_navline, 8,
	     KEYD_WHITE);
}
