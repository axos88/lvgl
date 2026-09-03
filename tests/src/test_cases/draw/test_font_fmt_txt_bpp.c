#if LV_BUILD_TEST
#include "../lvgl.h"
#include "../../lvgl_private.h"

#include "unity/unity.h"

/* A byte holds a whole number of pixels only for 1, 2, 4 and 8 bpp, and `lv_binfont_loader`
 * takes the bpp from the file header unchecked. Without the guard the first test reads past
 * `opa2_table` and then hangs. */

#define GLYPH_W 8
#define GLYPH_H 8

static lv_font_fmt_txt_glyph_dsc_t glyph_dsc[2];
static lv_font_fmt_txt_dsc_t font_dsc;
static lv_font_t font;
static uint8_t glyph_bitmap[GLYPH_W * GLYPH_H];
static lv_draw_buf_t * draw_buf;

void setUp(void)
{
    lv_memzero(glyph_dsc, sizeof(glyph_dsc));
    lv_memzero(&font_dsc, sizeof(font_dsc));
    lv_memzero(&font, sizeof(font));
    /*A recognizable pattern, so a wrong unpack is not silently all zeros*/
    lv_memset(glyph_bitmap, 0xA5, sizeof(glyph_bitmap));

    /*Index 0 is the "not found" slot*/
    glyph_dsc[1].bitmap_index = 0;
    glyph_dsc[1].adv_w = GLYPH_W << 4;
    glyph_dsc[1].box_w = GLYPH_W;
    glyph_dsc[1].box_h = GLYPH_H;
    glyph_dsc[1].ofs_x = 0;
    glyph_dsc[1].ofs_y = 0;

    font_dsc.glyph_bitmap = glyph_bitmap;
    font_dsc.glyph_dsc = glyph_dsc;
    font_dsc.bitmap_format = LV_FONT_FMT_TXT_PLAIN;

    font.dsc = &font_dsc;
    font.get_glyph_bitmap = lv_font_get_bitmap_fmt_txt;
    font.line_height = GLYPH_H;

    draw_buf = lv_draw_buf_create(GLYPH_W, GLYPH_H, LV_COLOR_FORMAT_A8, LV_STRIDE_AUTO);
}

void tearDown(void)
{
    if(draw_buf) {
        lv_draw_buf_destroy(draw_buf);
        draw_buf = NULL;
    }
}

static const void * unpack_with_bpp(uint8_t bpp)
{
    lv_font_glyph_dsc_t g;
    lv_memzero(&g, sizeof(g));
    g.resolved_font = &font;
    g.gid.index = 1;
    g.box_w = GLYPH_W;
    g.box_h = GLYPH_H;
    g.format = LV_FONT_GLYPH_FORMAT_A8;
    g.stride = 0;

    font_dsc.bpp = bpp;

    return lv_font_get_bitmap_fmt_txt(&g, draw_buf);
}

/** A bpp that does not divide 8 must be rejected, not walked. */
void test_font_fmt_txt_plain_rejects_bpp_that_does_not_divide_a_byte(void)
{
    TEST_ASSERT_NOT_NULL(draw_buf);
    TEST_ASSERT_NULL(unpack_with_bpp(3));
}

/** The supported values must keep working. */
void test_font_fmt_txt_plain_accepts_the_supported_bpp_values(void)
{
    TEST_ASSERT_NOT_NULL(draw_buf);

    TEST_ASSERT_NOT_NULL(unpack_with_bpp(1));
    TEST_ASSERT_NOT_NULL(unpack_with_bpp(2));
    TEST_ASSERT_NOT_NULL(unpack_with_bpp(4));
    TEST_ASSERT_NOT_NULL(unpack_with_bpp(8));
}

/** 8 bpp is a plain copy, so it pins the unpack loop the guard sits in front of. */
void test_font_fmt_txt_plain_unpacks_8bpp_unchanged(void)
{
    TEST_ASSERT_NOT_NULL(draw_buf);
    TEST_ASSERT_NOT_NULL(unpack_with_bpp(8));

    const uint8_t * out = draw_buf->data;
    uint32_t stride = lv_draw_buf_width_to_stride(GLYPH_W, LV_COLOR_FORMAT_A8);
    int32_t x, y;
    for(y = 0; y < GLYPH_H; y++) {
        for(x = 0; x < GLYPH_W; x++) {
            TEST_ASSERT_EQUAL_UINT8(0xA5, out[y * stride + x]);
        }
    }
}

#endif /*LV_BUILD_TEST*/
