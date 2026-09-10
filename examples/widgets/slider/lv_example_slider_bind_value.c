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
 * A subject has no built-in range, so the value is clamped by deriving it. The slider
 * writes `subject_raw`; `subject_value` computes the clamped version of it and is what
 * everything else reads. A subject with a mapper owns its value, so it is read-only:
 * writes go to the plain subject its mapper reads.
 */

/* Mirrors `subject_raw`, bounded to 0..100. Every `lv_subject_get_...()` call a mapper
 * makes registers a dependency, so this re-runs whenever `subject_raw` changes. `value`
 * holds the previously stored value on entry, which is all that is needed to report
 * whether anything changed. */
static lv_subject_t * subject_raw;

static bool clamp_0_100(lv_subject_t * subject, void * user_data, int32_t * value)
{
    LV_UNUSED(subject);
    LV_UNUSED(user_data);
    int32_t before = *value;
    *value = lv_subject_clamp_int(lv_subject_get_int(subject_raw), 0, 100);
    return *value != before;
}

void lv_example_slider_bind_value(void)
{
    static lv_subject_t * subject_value;

    static bool inited = false;

    if(!inited) {
        subject_raw = lv_subject_create(LV_SUBJECT_TYPE_INT);
        lv_subject_set_int(subject_raw, 50);

        subject_value = lv_subject_create(LV_SUBJECT_TYPE_INT);
        lv_subject_set_int_mapper(subject_value, clamp_0_100, NULL);

        /* Declared so `subject_raw` cannot be deleted while this mapper needs it. */
        static lv_subject_t * clamp_deps[1];
        clamp_deps[0] = subject_raw;
        lv_subject_set_static_deps(subject_value, clamp_deps, 1);
        inited = true;
    }

    lv_obj_t * screen = lv_screen_active();
    lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_flex_main_place(screen, LV_FLEX_ALIGN_CENTER, 0);
    lv_obj_set_style_flex_cross_place(screen, LV_FLEX_ALIGN_CENTER, 0);
    lv_obj_set_style_flex_track_place(screen, LV_FLEX_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_row(screen, 16, 0);

    /* 💡 Drag the slider; it writes `subject_raw` and the label re-renders because
     * `subject_value` is derived from it. */
    lv_obj_t * slider = lv_slider_create(screen);
    lv_obj_set_width(slider, lv_pct(90));
    lv_slider_bind_value(slider, subject_raw);

    lv_obj_t * label = lv_label_create(screen);
    lv_label_bind_text(label, subject_value, "Value: %d/100");
}
#endif
