/* host tests for e5-display-core.h (run via run.sh) */
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
#include "e5-display-core.h"

#define W 320
#define H 240

static int fails, runs, tests;
#define CHECK(c) do { runs++; if (!(c)) { fails++; \
	printf("FAIL %s:%d: %s\n", __func__, __LINE__, #c); } } while (0)

static u32 pat_xrgb(u32 x, u32 y)
{
	return ((x * 7 + y * 3) & 0xff) << 16 | ((x * 5 + y * 11) & 0xff) << 8 |
	       ((x * 13 + y) & 0xff);
}

static void fill_xrgb(u32 *p, u32 w, u32 h)
{
	for (u32 y = 0; y < h; y++)
		for (u32 x = 0; x < w; x++)
			p[y * w + x] = pat_xrgb(x, y);
}

static void fill_565(u16 *p, u32 w, u32 h)
{
	for (u32 y = 0; y < h; y++)
		for (u32 x = 0; x < w; x++)
			p[y * w + x] = e5d_xrgb_to_565(pat_xrgb(x, y));
}

static void test_xrgb_to_565(void)
{
	CHECK(e5d_xrgb_to_565(0x00ffffff) == 0xffff);
	CHECK(e5d_xrgb_to_565(0x00ff0000) == 0xf800);
	CHECK(e5d_xrgb_to_565(0x0000ff00) == 0x07e0);
	CHECK(e5d_xrgb_to_565(0x000000ff) == 0x001f);
	CHECK(e5d_xrgb_to_565(0x00808080) == 0x8410);
}

static void test_565_to_xrgb(void)
{
	CHECK(e5d_565_to_xrgb(0xffff) == 0x00ffffff);
	CHECK(e5d_565_to_xrgb(0xf800) == 0x00ff0000);
	CHECK(e5d_565_to_xrgb(0x0000) == 0);
}

static void test_roundtrip_565(void)
{
	int bad = 0;

	for (u32 v = 0; v < 65536; v++)
		if (e5d_xrgb_to_565(e5d_565_to_xrgb((u16)v)) != v)
			bad++;
	CHECK(bad == 0);
}

static void test_band_clamp(void)
{
	struct e5d_band b = { 0, 0 };

	e5d_band_add(&b, 230, 300);
	CHECK(b.y0 == 230 && b.y1 == 240);
	b = (struct e5d_band){ 0, 0 };
	e5d_band_add(&b, 5, 5);
	CHECK(b.y0 >= b.y1);
	u32 a, c;
	CHECK(!e5d_band_take(&b, &a, &c));
}

static void test_band_merge(void)
{
	struct e5d_band b = { 0, 0 };
	u32 a = 99, c = 99;

	e5d_band_add(&b, 10, 20);
	e5d_band_add(&b, 50, 60);
	CHECK(b.y0 == 10 && b.y1 == 60);
	CHECK(e5d_band_take(&b, &a, &c));
	CHECK(a == 10 && c == 60);
	CHECK(!e5d_band_take(&b, &a, &c));
}

static void test_copy_xrgb_to_565(void)
{
	static u32 src[W * H];
	static u16 dst[W * H];
	static u8 tmp[W * 4];
	struct e5d_band ch = { 0, 0 };
	int bad = 0;

	fill_xrgb(src, W, H);
	e5d_copy_rows((u8 *)dst, W * 2, 2, (u8 *)src, W * 4, 4, 0, W, 0, H, tmp, &ch);
	for (u32 i = 0; i < W * H; i++)
		if (dst[i] != e5d_xrgb_to_565(src[i]))
			bad++;
	CHECK(bad == 0);
	CHECK(ch.y0 == 0 && ch.y1 == H);
}

static void test_copy_565_to_565(void)
{
	static u16 src[W * H], dst[W * H];
	static u8 tmp[W * 4];
	struct e5d_band ch = { 0, 0 };

	fill_565(src, W, H);
	e5d_copy_rows((u8 *)dst, W * 2, 2, (u8 *)src, W * 2, 2, 0, W, 0, H, tmp, &ch);
	CHECK(memcmp(src, dst, sizeof(src)) == 0);
	CHECK(ch.y0 == 0 && ch.y1 == H);
}

static void test_copy_xrgb_to_xrgb(void)
{
	static u32 src[W * H], dst[W * H];
	static u8 tmp[W * 4];
	struct e5d_band ch = { 0, 0 };

	fill_xrgb(src, W, H);
	for (u32 i = 0; i < W * H; i++)
		src[i] |= 0xff000000;	/* dst keeps the top byte as is */
	e5d_copy_rows((u8 *)dst, W * 4, 4, (u8 *)src, W * 4, 4, 0, W, 0, H, tmp, &ch);
	CHECK(memcmp(src, dst, sizeof(src)) == 0);
	CHECK(ch.y0 == 0 && ch.y1 == H);
}

static void test_copy_565_to_xrgb(void)
{
	static u16 src[W * H];
	static u32 dst[W * H];
	static u8 tmp[W * 4];
	struct e5d_band ch = { 0, 0 };
	int bad = 0;

	fill_565(src, W, H);
	e5d_copy_rows((u8 *)dst, W * 4, 4, (u8 *)src, W * 2, 2, 0, W, 0, H, tmp, &ch);
	for (u32 i = 0; i < W * H; i++)
		if (dst[i] != e5d_565_to_xrgb(src[i]))
			bad++;
	CHECK(bad == 0);
	CHECK(ch.y0 == 0 && ch.y1 == H);
}

static void test_no_change_no_push(void)
{
	static u32 src[W * H];
	static u16 dst[W * H];
	static u8 tmp[W * 4];
	struct e5d_band ch = { 0, 0 };

	fill_xrgb(src, W, H);
	e5d_copy_rows((u8 *)dst, W * 2, 2, (u8 *)src, W * 4, 4, 0, W, 0, H, tmp, &ch);
	ch = (struct e5d_band){ 0, 0 };
	e5d_copy_rows((u8 *)dst, W * 2, 2, (u8 *)src, W * 4, 4, 0, W, 0, H, tmp, &ch);
	CHECK(ch.y0 >= ch.y1);
}

static void test_partial_rows(void)
{
	static u32 src[W * H];
	static u16 dst[W * H];
	static u8 tmp[W * 4];
	struct e5d_band ch = { 0, 0 };

	fill_xrgb(src, W, H);
	e5d_copy_rows((u8 *)dst, W * 2, 2, (u8 *)src, W * 4, 4, 0, W, 0, H, tmp, &ch);
	ch = (struct e5d_band){ 0, 0 };
	for (u32 y = 100; y < 103; y++)
		src[y * W + 17] ^= 0x00ffffff;
	e5d_copy_rows((u8 *)dst, W * 2, 2, (u8 *)src, W * 4, 4, 0, W, 0, H, tmp, &ch);
	CHECK(ch.y0 == 100 && ch.y1 == 103);
}

static void test_overlay_rgb565_size(void)
{
	/* an XRGB8888 overlay of 120 rows into a 565 region of 120 rows */
	static u32 src[W * 120];
	static u16 dst[W * 120];
	static u8 tmp[W * 4];
	struct e5d_band ch = { 0, 0 };

	CHECK(sizeof(src) == 320 * 4 * 120);
	CHECK(sizeof(dst) == 320 * 2 * 120);
	fill_xrgb(src, W, 120);
	e5d_copy_rows((u8 *)dst, W * 2, 2, (u8 *)src, W * 4, 4, 0, W, 0, 120, tmp, &ch);
	CHECK(dst[W * 120 - 1] == e5d_xrgb_to_565(src[W * 120 - 1]));
	CHECK(ch.y0 == 0 && ch.y1 == 120);
}

#define RUN(t) do { tests++; t(); } while (0)

int main(void)
{
	RUN(test_xrgb_to_565);
	RUN(test_565_to_xrgb);
	RUN(test_roundtrip_565);
	RUN(test_band_clamp);
	RUN(test_band_merge);
	RUN(test_copy_xrgb_to_565);
	RUN(test_copy_565_to_565);
	RUN(test_copy_xrgb_to_xrgb);
	RUN(test_copy_565_to_xrgb);
	RUN(test_no_change_no_push);
	RUN(test_partial_rows);
	RUN(test_overlay_rgb565_size);
	if (fails) {
		printf("%d of %d checks FAILED\n", fails, runs);
		return 1;
	}
	printf("%d passed\n", tests);
	return 0;
}
