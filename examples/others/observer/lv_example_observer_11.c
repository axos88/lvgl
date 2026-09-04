#include "../../lv_examples.h"
#if LV_USE_OBSERVER && LV_USE_ARC && LV_USE_LABEL && LV_BUILD_EXAMPLES

/*A reading that is free to go outside anything the arc can display*/
static lv_subject_t * subject_pressure;

static bool clamp_to_arc_range(lv_observer_t * observer, int32_t * out);

/**
 * @title Clamping in the observer, to the target widget's own range
 * @brief An unbounded subject drives an arc, bounded by the arc's min and max.
 *
 * `subject_pressure` carries whatever the sensor reports, including values the arc
 * cannot show. Clamping it in the subject would be wrong here: the real reading is
 * worth keeping, and a second widget or a log may want the out-of-range value.
 *
 * So the clamping belongs to the binding, not to the subject. The observer's mapper
 * reads the arc's own `lv_arc_get_min_value()` and `lv_arc_get_max_value()` and pulls
 * the reading inside them. Nothing hard-codes the range: change the arc's range and the
 * binding follows, because the mapper asks the widget every time.
 *
 * The mapper also returns `false` when its output has not changed, so a run of
 * out-of-range readings that all clamp to the same end leaves the arc untouched instead
 * of redrawing it.
 */
void lv_example_observer_11(void)
{
    static bool inited = false;

    if(!inited) {
        subject_pressure = lv_subject_create(LV_SUBJECT_TYPE_INT);
        inited = true;
    }

    lv_obj_t * screen = lv_screen_active();
    lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_flex_cross_place(screen, LV_FLEX_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_row(screen, 12, 0);
    lv_obj_set_style_pad_all(screen, 16, 0);

    lv_obj_t * arc = lv_arc_create(screen);
    lv_obj_set_size(arc, 150, 150);
    lv_arc_set_range(arc, 0, 100);

    /* 💡 The arc's range is read from the arc itself, so this binding needs no constants. */
    lv_obj_bind_int_mapped(arc, subject_pressure, lv_arc_set_value, clamp_to_arc_range, NULL);

    /*The label shows the true reading, unclamped, straight from the subject*/
    lv_obj_t * label = lv_label_create(screen);
    lv_label_bind_text(label, subject_pressure, "reading: %d");

    lv_subject_set_int(subject_pressure, 40);    /*inside the range: shown as 40*/
    lv_subject_set_int(subject_pressure, 250);   /*above it: arc shows 100, label 250*/
}

/*An *observer* mapper, which is shaped differently from a *subject* mapper. It has no
 *`input` parameter: `out` is this observer's own output slot, holding the value it last
 *pushed to the widget, not the subject's value. So the reading has to come from the
 *subject, and `*out` is only there so the mapper can answer "did my output change?" and
 *leave the widget alone when it did not.*/
static bool clamp_to_arc_range(lv_observer_t * observer, int32_t * out)
{
    lv_obj_t * arc = lv_observer_get_target_obj(observer);
    int32_t reading = lv_subject_get_int(lv_observer_get_subject(observer));

    int32_t bounded = lv_subject_clamp_int(reading, lv_arc_get_min_value(arc), lv_arc_get_max_value(arc));

    /*`*out` is the previous output: same clamped value means nothing to redraw*/
    if(bounded == *out) return false;

    *out = bounded;
    return true;
}

#endif
