/* host tests for core.c: cc -O1 -Wall -o t core.c test_core.c && ./t */
#include <stdio.h>
#include <string.h>
#include "keyd.h"

static int fails, runs;
#define CHECK(c) do { runs++; if (!(c)) { fails++; \
	printf("FAIL %s:%d: %s\n", __func__, __LINE__, #c); } } while (0)

static struct keyd_state s;
static unsigned ev(int code, int value) { return keyd_event(&s, code, value); }
static void reset(void) { memset(&s, 0, sizeof(s)); }

static void test_fn_toggle(void)
{
	reset();
	CHECK(ev(KEYD_FN_CODE, 1) == KEYD_SHOW);
	CHECK(ev(KEYD_FN_CODE, 0) == 0);
	CHECK(ev(KEYD_FN_CODE, 1) == KEYD_HIDE);
	CHECK(!s.fn);
}

static void test_fn_one_key(void)
{
	reset();
	ev(KEYD_FN_CODE, 1);
	CHECK(ev(34, 1) == KEYD_HIDE);		/* G -> '_' ends the layer */
	CHECK(!s.fn);
	CHECK(ev(34, 0) == 0);
}

static void test_no_fn_no_action(void)
{
	reset();
	CHECK(ev(16, 1) == 0);
}

static void test_repeat_ignored(void)
{
	reset();
	ev(KEYD_FN_CODE, 1);
	CHECK(ev(KEYD_FN_CODE, 2) == 0);
	CHECK(s.fn);
}

static void test_power_toggles(void)
{
	reset();
	CHECK(ev(KEYD_POWER_CODE, 1) == KEYD_BLANK);
	CHECK(ev(KEYD_POWER_CODE, 1) == KEYD_UNBLANK);
	CHECK(!s.blanked);
}

/* e5-keyd grabs the power key, so the console never sees it and keeps its
 * sticky Fn: the picture stays (it shows again on wake), the wake key ends it */
static void test_power_keeps_fn(void)
{
	reset();
	ev(KEYD_FN_CODE, 1);
	CHECK(ev(KEYD_POWER_CODE, 1) == KEYD_BLANK);
	CHECK(s.fn && s.blanked);
	CHECK(ev(34, 1) == (KEYD_UNBLANK | KEYD_HIDE));
	CHECK(!s.fn && !s.blanked);
}

static void test_any_key_wakes(void)
{
	reset();
	ev(KEYD_POWER_CODE, 1);
	CHECK(ev(16, 1) == KEYD_UNBLANK);
	CHECK(!s.blanked);
}

/* the pmic latches on both edges: press and release of one tap are two
 * latches (152 ms apart on the phone; 1.2 s for a 1 s hold) = one action */
static void test_power_edges_debounced(void)
{
	reset();
	CHECK(keyd_power_edge(&s, 359457));
	CHECK(!keyd_power_edge(&s, 359609));		/* its release */
	CHECK(keyd_power_edge(&s, 363713));		/* next press */
	CHECK(!keyd_power_edge(&s, 364929));		/* 1.2 s hold released */
	CHECK(keyd_power_edge(&s, 367000));
}

static void test_layer_table(void)
{
	CHECK(!strcmp(keyd_cells[0][0].sym, "F1") && !strcmp(keyd_cells[0][0].label, "Q"));
	CHECK(!strcmp(keyd_cells[0][9].sym, "F10"));
	CHECK(!strcmp(keyd_cells[1][4].sym, "_"));
	CHECK(!strcmp(keyd_cells[1][9].sym, "Del") && keyd_cells[1][9].accent);
	CHECK(!strcmp(keyd_cells[2][6].sym, "") && !strcmp(keyd_cells[2][6].label, "M"));
	CHECK(!strcmp(keyd_cells[2][7].sym, "F11") && keyd_cells[2][7].accent);
	CHECK(!keyd_cells[1][2].accent);
}

static uint32_t px[KEYD_H][KEYD_W];

static int cx(int c) { return c * KEYD_CELL_W; }
static int cy(int r) { return KEYD_TOP + r * KEYD_CELL_H; }

static int count(int r, int c, int y0, int y1, uint32_t col)
{
	int n = 0;

	for (int y = cy(r) + y0; y < cy(r) + y1; y++)
		for (int x = cx(c); x < cx(c) + KEYD_CELL_W; x++)
			n += px[y][x] == col;
	return n;
}

static long luma(int r, int c)
{
	long sum = 0;

	for (int y = cy(r); y < cy(r) + KEYD_CELL_H; y++)
		for (int x = cx(c); x < cx(c) + KEYD_CELL_W; x++)
			sum += ((px[y][x] >> 16) & 255) + ((px[y][x] >> 8) & 255) + (px[y][x] & 255);
	return sum;
}

/* font files built in memory: glyph i = h bytes of value i (psf1) */
static unsigned char fb[65536];

static size_t psf1(int mode, int h, int map_a_to)
{
	size_t n = 0, glyphs = mode & 1 ? 512 : 256;

	fb[n++] = 0x36, fb[n++] = 0x04, fb[n++] = mode, fb[n++] = h;
	for (size_t i = 0; i < glyphs; i++)
		for (int r = 0; r < h; r++)
			fb[n++] = i;
	for (size_t i = 0; mode & 2 && i < glyphs; i++) {
		if ((int)i == map_a_to)
			fb[n++] = 'A', fb[n++] = 0;
		fb[n++] = 0xff, fb[n++] = 0xff;
	}
	return n;
}

static void le32(unsigned char *p, unsigned v)
{
	p[0] = v, p[1] = v >> 8, p[2] = v >> 16, p[3] = v >> 24;
}

/* psf2, 128 glyphs: glyph 7 = "A", then a sequence "Z" (not a plain 'Z') */
static size_t psf2(unsigned w, unsigned h, int table)
{
	size_t n = 32, size = h * ((w + 7) / 8);

	memset(fb, 0, 32);
	le32(fb, 0x864ab572), le32(fb + 8, 32), le32(fb + 12, table);
	le32(fb + 16, 128), le32(fb + 20, size), le32(fb + 24, h), le32(fb + 28, w);
	for (unsigned i = 0; i < 128; i++)
		for (size_t r = 0; r < size; r++)
			fb[n++] = i;
	for (unsigned i = 0; table && i < 128; i++) {
		if (i == 7)
			fb[n++] = 'A', fb[n++] = 0xfe, fb[n++] = 'Z';
		fb[n++] = 0xff;
	}
	return n;
}

static void test_psf1_plain(void)
{
	struct keyd_font f;

	CHECK(keyd_font_psf(&f, fb, psf1(0, 8, -1)) == 0);
	CHECK(f.h == 8);
	CHECK(f.g['A' - 32] && f.g['A' - 32][0] == 'A' && f.g['A' - 32][7] == 'A');
	CHECK(f.g['~' - 32][0] == '~');
}

static void test_psf1_unicode(void)
{
	struct keyd_font f;

	CHECK(keyd_font_psf(&f, fb, psf1(3, 16, 300)) == 0);	/* 512 + table */
	CHECK(f.h == 16);
	CHECK(f.g['A' - 32] == fb + 4 + 300 * 16);
	CHECK(f.g['B' - 32][0] == 'B');			/* not in the table */
}

static void test_psf2_unicode(void)
{
	struct keyd_font f;

	CHECK(keyd_font_psf(&f, fb, psf2(8, 16, 1)) == 0);
	CHECK(f.h == 16);
	CHECK(f.g['A' - 32] == fb + 32 + 7 * 16);
	CHECK(f.g['Z' - 32][0] == 'Z');			/* a sequence, not 'Z' */
	CHECK(keyd_font_psf(&f, fb, psf2(6, 12, 0)) == 0);	/* narrower is fine */
	CHECK(f.h == 12 && f.g['!' - 32][0] == '!');
}

static void test_psf_rejects(void)
{
	struct keyd_font f;
	size_t n;

	CHECK(keyd_font_psf(&f, fb, psf2(9, 16, 0)) == -1);	/* 2 bytes per row */
	n = psf2(8, 16, 0);
	CHECK(keyd_font_psf(&f, fb, n - 17) == -1);		/* last glyph cut */
	n = psf1(0, 8, -1);
	CHECK(keyd_font_psf(&f, fb, n - 1) == -1);
	CHECK(keyd_font_psf(&f, fb, 3) == -1);
	CHECK(keyd_font_psf(&f, (const unsigned char *)"not a font at all", 17) == -1);
	CHECK(keyd_font_psf(&f, fb, psf1(2, 8, -1) - 1) == -1);	/* table cut */
}

static void test_console_font(void)
{
	struct keyd_font f;

	for (int i = 0; i < 256; i++)
		memset(fb + i * 32, i, 32);
	CHECK(keyd_font_console(&f, fb, 8, 8, 256) == 0);
	CHECK(f.h == 8 && f.g['A' - 32] == fb + 'A' * 32);
	CHECK(keyd_font_console(&f, fb, 6, 10, 100) == 0);	/* short font */
	CHECK(f.h == 10 && f.g['c' - 32] == fb + 'c' * 32 && !f.g['d' - 32]);
	CHECK(keyd_font_console(&f, fb, 9, 16, 256) == -1);
	CHECK(keyd_font_console(&f, fb, 8, 33, 256) == -1);
}

/* solid glyphs: every row lit */
static unsigned char solid[32];
static struct keyd_font solid_font(int h)
{
	struct keyd_font f = { .h = h };

	memset(solid, 0xff, sizeof(solid));
	for (int i = 0; i < 95; i++)
		f.g[i] = solid;
	return f;
}

static void test_render(void)
{
	struct keyd_font big = solid_font(16), small = solid_font(8);

	memset(px, 0, sizeof(px));
	keyd_render(px, &big, &small);
	CHECK(px[0][0] == KEYD_AMBER && px[1][319] == KEYD_AMBER);
	CHECK(count(0, 0, 4, 20, KEYD_WHITE) > 10);		/* "F1" */
	CHECK(count(0, 0, 24, 32, KEYD_AMBER) > 5);		/* "Q" */
	CHECK(px[cy(0) + 2][cx(0) + 15] == KEYD_AMBER);	/* F1: accent edge */
	CHECK(px[cy(1) + 2][cx(9) + 15] == KEYD_AMBER);	/* Del */
	CHECK(px[cy(1) + 2][cx(2) + 15] == KEYD_KEY);		/* '%': no edge */
	CHECK(px[cy(1) + 1][cx(2) + 15] == KEYD_BORDER);
	CHECK(px[cy(2) + 20][cx(5) + 2] == KEYD_KEY);		/* '}' key fill */
	CHECK(luma(2, 6) < luma(2, 5));				/* M dimmed */
	CHECK(count(0, 0, 0, 0, 0) == 0);
	int nav = 0;
	for (int y = KEYD_NAV_Y; y < KEYD_H; y++)
		for (int x = 0; x < KEYD_W; x++)
			nav += px[y][x] == KEYD_WHITE;
	CHECK(nav > 50);					/* the nav line */
}

/* an 8 px font for the Fn symbols (simple mode: the console's 8x8) is
 * stretched to 16 rows: its last row lands on rows 14 + 15 of the symbol */
static void test_render_stretches(void)
{
	static unsigned char last_row[8] = { 0, 0, 0, 0, 0, 0, 0, 0xff };
	struct keyd_font big = { .h = 8 }, small = solid_font(8);

	for (int i = 0; i < 95; i++)
		big.g[i] = last_row;
	memset(px, 0, sizeof(px));
	keyd_render(px, &big, &small);
	CHECK(count(0, 0, 5 + 13, 5 + 14, KEYD_WHITE) == 0);
	CHECK(count(0, 0, 5 + 14, 5 + 15, KEYD_WHITE) == 16);	/* "F1" */
	CHECK(count(0, 0, 5 + 15, 5 + 16, KEYD_WHITE) == 16);
	CHECK(count(0, 0, 5, 5 + 14, KEYD_WHITE) == 0);
}

static void test_render_no_font(void)
{
	memset(px, 0, sizeof(px));
	keyd_render(px, NULL, NULL);
	CHECK(px[0][0] == KEYD_AMBER);
	CHECK(count(0, 0, 4, 20, KEYD_WHITE) == 0);
	CHECK(px[cy(2) + 20][cx(5) + 2] == KEYD_KEY);
}

int main(void)
{
	test_fn_toggle();
	test_fn_one_key();
	test_no_fn_no_action();
	test_repeat_ignored();
	test_power_toggles();
	test_power_keeps_fn();
	test_any_key_wakes();
	test_power_edges_debounced();
	test_layer_table();
	test_psf1_plain();
	test_psf1_unicode();
	test_psf2_unicode();
	test_psf_rejects();
	test_console_font();
	test_render();
	test_render_stretches();
	test_render_no_font();
	printf("%d/%d passed\n", runs - fails, runs);
	return fails != 0;
}
