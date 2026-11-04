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

static void test_render(void)
{
	memset(px, 0, sizeof(px));
	keyd_render(px);
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
	test_render();
	printf("%d/%d passed\n", runs - fails, runs);
	return fails != 0;
}
