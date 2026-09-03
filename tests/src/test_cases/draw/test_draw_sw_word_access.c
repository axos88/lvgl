#if LV_BUILD_TEST
#include "../lvgl.h"
#include "../../lvgl_private.h"

#include "unity/unity.h"

#if LV_USE_DRAW_SW

#include "src/draw/sw/blend/lv_draw_sw_blend_private.h"

/* The blend loops touch four bytes of a uint16_t or uint8_t buffer as one word. Built with
 * -O2 -fstrict-aliasing and no sanitizers (see tests/CMakeLists.txt), so a raw cast in the
 * macros fails these and the union passes. `noinline` is needed: the miscompile only appears
 * when the optimizer sees both the buffer's type and the word access. */

#if defined(__GNUC__) || defined(__clang__)
    #define TEST_NOINLINE __attribute__((noinline))
#elif defined(_MSC_VER)
    #define TEST_NOINLINE __declspec(noinline)
#else
    #define TEST_NOINLINE
#endif

void setUp(void)
{
}

void tearDown(void)
{
}

static void TEST_NOINLINE store_word_read_halfwords(uint16_t * px, uint32_t word, uint32_t * sum_out)
{
    ((lv_draw_sw_word_t *)px)->u32 = word;
    *sum_out = (uint32_t)px[0] + (uint32_t)px[1];
}

static void TEST_NOINLINE store_halfwords_read_word(uint16_t * px, uint16_t value, uint32_t * word_out)
{
    px[0] = value;
    px[1] = value;
    *word_out = ((const lv_draw_sw_word_t *)px)->u32;
}

static void TEST_NOINLINE store_word_read_bytes(uint8_t * mask, uint32_t word, uint32_t * sum_out)
{
    ((lv_draw_sw_word_t *)mask)->u32 = word;
    *sum_out = (uint32_t)mask[0] + (uint32_t)mask[1] + (uint32_t)mask[2] + (uint32_t)mask[3];
}

/** The RGB565 opacity fill stores a blended pair, then reads neighbouring pixels back. */
void test_draw_sw_store_u32_is_visible_to_halfword_reads(void)
{
    uint16_t px[2] = {0, 0};
    uint32_t sum = 0;

    store_word_read_halfwords(px, 0x00010001, &sum);

    TEST_ASSERT_EQUAL_UINT32(2, sum);
    TEST_ASSERT_EQUAL_UINT16(1, px[0]);
    TEST_ASSERT_EQUAL_UINT16(1, px[1]);
}

/** The other direction. */
void test_draw_sw_load_u32_sees_halfword_writes(void)
{
    uint16_t px[2] = {0, 0};
    uint32_t word = 0;

    store_halfwords_read_word(px, 0x1234, &word);

    TEST_ASSERT_EQUAL_UINT32(0x12341234, word);
}

/** A byte buffer, as the mask fast paths scan. */
void test_draw_sw_store_u32_is_visible_to_byte_reads(void)
{
    uint8_t mask[4] = {0, 0, 0, 0};
    uint32_t sum = 0;

    store_word_read_bytes(mask, 0x01010101, &sum);

    TEST_ASSERT_EQUAL_UINT32(4, sum);
}

/** The two macros agree, so the word is native endian. */
void test_draw_sw_word_access_round_trips(void)
{
    uint8_t buf[4] = {0, 0, 0, 0};

    ((lv_draw_sw_word_t *)buf)->u32 = 0xDEADBEEF;

    TEST_ASSERT_EQUAL_UINT32(0xDEADBEEF, ((const lv_draw_sw_word_t *)buf)->u32);
}

#else /*LV_USE_DRAW_SW*/

void setUp(void)
{
}

void tearDown(void)
{
}

void test_draw_sw_store_u32_is_visible_to_halfword_reads(void)
{
    TEST_PASS();
}

void test_draw_sw_load_u32_sees_halfword_writes(void)
{
    TEST_PASS();
}

void test_draw_sw_store_u32_is_visible_to_byte_reads(void)
{
    TEST_PASS();
}

void test_draw_sw_word_access_round_trips(void)
{
    TEST_PASS();
}

#endif /*LV_USE_DRAW_SW*/

#endif /*LV_BUILD_TEST*/
