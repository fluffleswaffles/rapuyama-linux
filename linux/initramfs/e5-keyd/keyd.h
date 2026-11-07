/* e5-keyd: the Fn key picture and the power key screen toggle (Nokia E5).
 * core.c = state machine + picture (no I/O, host-tested), main.c = devices. */
#ifndef E5_KEYD_H
#define E5_KEYD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define KEYD_W		320	/* the fb overlay: panel rows 120..239 */
#define KEYD_H		120
#define KEYD_FN_CODE	97	/* KEY_RIGHTCTRL, the bottom-left arrow key */
#define KEYD_POWER_CODE	116	/* KEY_POWER, "Nokia E5 power key" */

/* colours, xrgb8888 (design 2.4) */
#define KEYD_PANEL	0x1a1b1eu
#define KEYD_KEY	0x26272bu
#define KEYD_BORDER	0x45474du
#define KEYD_AMBER	0xf5b041u
#define KEYD_WHITE	0xffffffu

/* geometry: 2 px amber top border, 3 rows x 10 cells of 32 x 36, nav line */
#define KEYD_TOP	2
#define KEYD_CELL_W	32
#define KEYD_CELL_H	36
#define KEYD_NAV_Y	(KEYD_TOP + 3 * KEYD_CELL_H + 2)

enum { KEYD_SHOW = 1, KEYD_HIDE = 2, KEYD_BLANK = 4, KEYD_UNBLANK = 8 };

struct keyd_state {
	bool fn;	/* the Fn layer is on (the console's sticky SCtrlR) */
	bool blanked;	/* screen off by the power key */
	long power_ms;	/* time of the last power latch, 0 = none */
};

/* the pmic latches the power key on press AND release: a latch within
 * KEYD_POWER_GAP_MS of the previous one is the same tap (or hold) */
#define KEYD_POWER_GAP_MS	1500
bool keyd_power_edge(struct keyd_state *s, long ms);

struct keyd_cell {
	const char *sym;	/* what Fn + this key gives, "" = nothing */
	const char *label;	/* the key's own letter */
	bool accent;		/* F-key or Del: amber top edge */
};

extern const struct keyd_cell keyd_cells[3][10];	/* layer.h */
extern const char keyd_navline[];

/* one input event (EV_KEY): value 0 up, 1 down, 2 repeat -> KEYD_* bits */
unsigned keyd_event(struct keyd_state *s, int code, int value);

/* a font for the picture, at most 8 px wide: one byte per glyph row (msb =
 * leftmost pixel), g[c - 32] = the glyph of ASCII c, NULL = none. The glyphs
 * point into the buffer it was read from */
struct keyd_font {
	int h;
	const unsigned char *g[95];
};
/* a psf1/psf2 file (uncompressed); ASCII through its unicode table when it
 * has one. 0, or -1: not psf, wider than 8 px, truncated */
int keyd_font_psf(struct keyd_font *f, const unsigned char *buf, size_t len);
/* the console font as KD_FONT_OP_GET returns it (32 rows per glyph, glyph
 * number = character code). 0, or -1: wider than 8 px, taller than 32 */
int keyd_font_console(struct keyd_font *f, const unsigned char *data,
		      unsigned w, unsigned h, unsigned count);
/* the picture, rows 0..KEYD_H-1 of the overlay: Fn symbols 16 px tall from
 * big, key letters 8 px tall from small (rows scaled to fit, so an 8x8 font
 * does both); a NULL font leaves that text out */
void keyd_render(uint32_t px[KEYD_H][KEYD_W], const struct keyd_font *big,
		 const struct keyd_font *small);

#endif
