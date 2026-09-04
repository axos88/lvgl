#include "../../lv_examples.h"
#if LV_USE_OBSERVER && LV_USE_ARC && LV_USE_LABEL && LV_BUILD_EXAMPLES

/*One subject: the measured temperature, in whole degrees Celsius*/
static lv_subject_t * subject_temperature;

static bool celsius_text(lv_observer_t * observer, const char ** out);
static bool to_arc_celsius(lv_observer_t * observer, int32_t * out);
static bool to_arc_fahrenheit(lv_observer_t * observer, int32_t * out);

/**
 * @title Why an observer has a mapper
 * @brief One temperature subject drives a label and two arcs in different units.
 *
 * Three subscribers, one subject, and every one of them needs the value in a different
 * shape:
 *
 * - a label wants a finished string, "Temperature is 21 Centigrade";
 * - one arc wants an integer in Celsius, bounded to its own range;
 * - the other arc wants an integer in Fahrenheit, bounded to *its* range, which is a
 *   different range because the unit is different.
 *
 * The subject holds the temperature once, in one unit, and each observer's mapper turns
 * it into what its own widget needs. Nothing downstream has to agree on units, and
 * nothing upstream has to know how many widgets are watching.
 *
 * Without observer mappers this needs three more subjects — a formatted string, a
 * bounded Celsius integer and a bounded Fahrenheit integer — each with its own mapper
 * and its own storage, existing only to feed one widget. The observer mapper is the
 * cheap version of that: no extra subject, no extra buffer, just a function on the way
 * out.
 *
 * Each mapper also returns whether its output changed, so a temperature that moves
 * without changing a rounded arc value redraws nothing.
 */
void lv_example_observer_12(void)
{
    static bool inited = false;

    if(!inited) {
        subject_temperature = lv_subject_create(LV_SUBJECT_TYPE_INT);
        inited = true;
    }

    lv_obj_t * screen = lv_screen_active();
    lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_flex_cross_place(screen, LV_FLEX_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_row(screen, 8, 0);
    lv_obj_set_style_pad_all(screen, 12, 0);

    /* 💡 A string on the way out, built by the observer's mapper. */
    lv_obj_t * label = lv_label_create(screen);
    lv_obj_bind_string_mapped(label, subject_temperature, lv_label_set_text, celsius_text, NULL);

    lv_obj_t * row = lv_obj_create(screen);
    lv_obj_set_size(row, lv_pct(100), 130);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_flex_main_place(row, LV_FLEX_ALIGN_SPACE_EVENLY, 0);

    /*The two arcs measure the same thing on different scales, so the same subject value
     *has to become a different arc value for each of them.*/
    lv_obj_t * arc_c = lv_arc_create(row);
    lv_obj_set_size(arc_c, 100, 100);
    lv_arc_set_range(arc_c, -20, 50);          /*Celsius*/
    lv_obj_bind_int_mapped(arc_c, subject_temperature, lv_arc_set_value, to_arc_celsius, NULL);

    lv_obj_t * arc_f = lv_arc_create(row);
    lv_obj_set_size(arc_f, 100, 100);
    lv_arc_set_range(arc_f, -4, 122);          /*the same span, in Fahrenheit*/
    lv_obj_bind_int_mapped(arc_f, subject_temperature, lv_arc_set_value, to_arc_fahrenheit, NULL);

    /*One write updates all three, each in its own unit*/
    lv_subject_set_int(subject_temperature, 21);
}

/*The label's mapper owns the text it hands out, so a static buffer serves.*/
static bool celsius_text(lv_observer_t * observer, const char ** out)
{
    static char buf[48];
    int32_t celsius = lv_subject_get_int(lv_observer_get_subject(observer));

    char next[48];
    lv_snprintf(next, sizeof(next), "Temperature is %" LV_PRId32 " Centigrade", celsius);
    if(lv_strcmp(next, buf) == 0) return false;   /*unchanged, leave the label alone*/

    lv_strlcpy(buf, next, sizeof(buf));
    *out = buf;
    return true;
}

/*Bounded to the arc's own range, read from the arc rather than hard-coded, so changing
 *the range in the code above needs no change here.*/
static bool to_arc_celsius(lv_observer_t * observer, int32_t * out)
{
    lv_obj_t * arc = lv_observer_get_target_obj(observer);
    int32_t celsius = lv_subject_get_int(lv_observer_get_subject(observer));

    int32_t bounded = lv_subject_clamp_int(celsius, lv_arc_get_min_value(arc), lv_arc_get_max_value(arc));
    if(bounded == *out) return false;   /*nothing to redraw*/

    *out = bounded;
    return true;
}

/*The same value, converted first. This is the mapper that justifies the whole feature:
 *the subject knows nothing about Fahrenheit, and neither does the arc.*/
static bool to_arc_fahrenheit(lv_observer_t * observer, int32_t * out)
{
    lv_obj_t * arc = lv_observer_get_target_obj(observer);
    int32_t celsius = lv_subject_get_int(lv_observer_get_subject(observer));
    int32_t fahrenheit = (celsius * 9) / 5 + 32;

    int32_t bounded = lv_subject_clamp_int(fahrenheit, lv_arc_get_min_value(arc), lv_arc_get_max_value(arc));
    if(bounded == *out) return false;

    *out = bounded;
    return true;
}

#endif
