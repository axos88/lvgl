/**
 * @file lv_example_slider_bind_value.c
 */

#include "../../lv_examples.h"
#if LV_USE_SLIDER && LV_BUILD_EXAMPLES

/**
 * @title Slider bind value
 * @brief Two-way bind a slider to a shared int subject; a label mirrors the live value.
 *
 * `subject_value` lives in `examples/xml_project/globals.xml` (default 50).
 * The slider's `bind_value` reads and writes the subject: dragging it pushes the new
 * value out so anything else bound to `subject_value` (here, the label) updates
 * immediately. `bind_text-fmt` lets a label render a numeric subject through a
 * printf-style format.
 *
 * A subject has no built-in range, so `clamp_0_100` keeps the value in 0..100 no matter
 * who writes it. A mapper owns the subject's value: it is given the value that was just
 * written and returns whether it changed the stored one.
 */

/* Keeps `subject_value` in 0..100. `value` holds the previously stored value on entry,
 * so keeping a copy of it is all that is needed to report whether anything changed.
 * `input` arrives as a union because a subject's input type need not be the type of its
 * value; here both are integers, so read `.num`. */
static bool clamp_0_100(lv_subject_t * subject, void * user_data, lv_subject_value_t input, int32_t * value)
{
    LV_UNUSED(subject);
    LV_UNUSED(user_data);
    int32_t before = *value;
    *value = lv_subject_clamp_int(input.num, 0, 100);
    return *value != before;
}

void lv_example_slider_bind_value(void)
{
    static lv_subject_t * subject_value;

    static bool inited = false;

    if(!inited) {
        subject_value = lv_subject_create(LV_SUBJECT_TYPE_INT);
        /* Before the first value, so that one is clamped too. */
        lv_subject_set_int_mapper(subject_value, clamp_0_100, NULL);
        lv_subject_set_int(subject_value, 50);
        inited = true;
    }

    lv_obj_t * screen = lv_screen_active();
    lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_flex_main_place(screen, LV_FLEX_ALIGN_CENTER, 0);
    lv_obj_set_style_flex_cross_place(screen, LV_FLEX_ALIGN_CENTER, 0);
    lv_obj_set_style_flex_track_place(screen, LV_FLEX_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_row(screen, 16, 0);

    /* 💡 Drag the slider; the label re-renders because both widgets share `subject_value`. */
    lv_obj_t * slider = lv_slider_create(screen);
    lv_obj_set_width(slider, lv_pct(90));
    lv_slider_bind_value(slider, subject_value);

    lv_obj_t * label = lv_label_create(screen);
    lv_label_bind_text(label, subject_value, "Value: %d/100");
}
#endif
