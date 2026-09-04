#include "../../lv_examples.h"
#if LV_USE_OBSERVER && LV_USE_SLIDER && LV_USE_LABEL && LV_USE_ROLLER && LV_USE_DROPDOWN && LV_FONT_MONTSERRAT_30 && LV_BUILD_EXAMPLES

static lv_subject_t * hour_subject;
static lv_subject_t * minute_subject;
static lv_subject_t * format_subject;
static lv_subject_t * am_pm_subject;
const char * hour12_options = "01\n02\n03\n04\n05\n06\n07\n08\n09\n10\n11\n12";
const char * hour24_options =
    "00\n01\n02\n03\n04\n05\n06\n07\n08\n09\n10\n11\n12\n13\n14\n15\n16\n17\n18\n19\n20\n21\n22\n23";
const char * minute_options =
    "00\n01\n02\n03\n04\n05\n06\n07\n08\n09\n10\n11\n12\n13\n14\n15\n16\n17\n18\n19\n20\n21\n22\n23\n24\n25\n26\n27\n28\n29\n30\n31\n32\n33\n34\n35\n36\n37\n38\n39\n40\n41\n42\n43\n44\n45\n46\n47\n48\n49\n50\n51\n52\n53\n54\n55\n56\n57\n58\n59";

static void set_btn_clicked_event_cb(lv_event_t * e);
static void close_clicked_event_cb(lv_event_t * e);
static void hour_roller_options_update(lv_observer_t * observer, lv_subject_t * subject);
static bool time_mapper(lv_subject_t * subject, void * user_data, lv_subject_value_t input, const void ** value);
static void time_observer_cb(lv_observer_t * observer, lv_subject_t * subject);

typedef enum {
    TIME_FORMAT_12,
    TIME_FORMAT_24,
} time_format_t;

/*The value `time_subject` publishes. Every subscriber gets this, already assembled,
 *rather than a bare "something changed" and four subjects to go and read.*/
typedef struct {
    int32_t hour;
    int32_t minute;
    time_format_t format;
    bool pm;
} datetime_t;

typedef enum {
    TIME_AM,
    TIME_PM,
} time_am_pm_t;

/**
 * @title Time setting with an aggregate subject
 * @brief Aggregate hour, minute, format, and AM/PM subjects into one subject.
 *
 * Four int subjects hold hour, minute, 12/24 format, and AM/PM. `time_subject` joins
 * them: its mapper reads all four, which is what makes them its dependencies, and
 * assembles a `datetime_t`. Subscribers are handed that finished struct, so the
 * notification means "the time is now this" rather than "something changed, go and
 * work out what". No subscriber reads the four parts, so no two of them can disagree
 * about how to interpret them. A "Set" button creates
 * a bottom container with two rollers and two dropdowns bound through
 * `lv_roller_bind_value` and `lv_dropdown_bind_value`; the AM/PM dropdown adds an
 * observer with `lv_subject_add_observer_obj` to disable itself in
 * `TIME_FORMAT_24`. A second observer on the format subject swaps the hour roller
 * options between the 12 and 24 lists.
 */
void lv_example_observer_3(void)
{
    /*Initialize the subjects.
     *The UI will update these and read the current values from here,
     *however the application can update these values at any time and
     *the widgets will be updated automatically. */
    hour_subject = lv_subject_create(LV_SUBJECT_TYPE_INT);
    minute_subject = lv_subject_create(LV_SUBJECT_TYPE_INT);
    format_subject = lv_subject_create(LV_SUBJECT_TYPE_INT);
    am_pm_subject = lv_subject_create(LV_SUBJECT_TYPE_INT);

    lv_subject_set_int(hour_subject, 7);
    lv_subject_set_int(minute_subject, 45);
    lv_subject_set_int(format_subject, TIME_FORMAT_12);
    lv_subject_set_int(am_pm_subject, TIME_AM);

    /*A subject that joins the four above into one value. Its mapper reads them, which
     *is what registers them as its dependencies, and then assembles a `datetime_t`.
     *Subscribers receive that struct, so none of them has to know that the time is
     *stored as four separate subjects, and none of them can disagree about how to
     *read it.*/
    static datetime_t datetime;
    lv_subject_t * time_subject = lv_subject_create(LV_SUBJECT_TYPE_POINTER);
    lv_subject_set_pointer_mapper(time_subject, time_mapper, &datetime);

    /*Create the UI*/
    lv_obj_t * time_label = lv_label_create(lv_screen_active());
    lv_obj_set_style_text_font(time_label, &lv_font_montserrat_30, 0);
    lv_subject_add_observer_obj(time_subject, time_observer_cb, time_label, NULL);
    lv_obj_set_pos(time_label, 24, 24);

    lv_obj_t * set_btn = lv_button_create(lv_screen_active());
    lv_obj_set_pos(set_btn, 180, 24);
    lv_obj_add_event_cb(set_btn, set_btn_clicked_event_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t * set_label = lv_label_create(set_btn);
    lv_label_set_text(set_label, "Set");

    /*Update some subjects to see if the UI is updated as well*/
    lv_subject_set_int(hour_subject, 9);
    lv_subject_set_int(minute_subject, 30);
    lv_subject_set_int(am_pm_subject, TIME_PM);
}

/*Disable the AM/PM dropdown while the 24-hour format is selected*/
static void am_pm_disabled_observer_cb(lv_observer_t * observer, lv_subject_t * subject)
{
    lv_obj_t * obj = lv_observer_get_target_obj(observer);
    lv_obj_set_state(obj, LV_STATE_DISABLED, lv_subject_get_int(subject) == TIME_FORMAT_24);
}

static void set_btn_clicked_event_cb(lv_event_t * e)
{
    lv_obj_t * set_btn = lv_event_get_target_obj(e);
    lv_obj_add_state(set_btn, LV_STATE_DISABLED);

    lv_obj_t * cont = lv_obj_create(lv_screen_active());
    lv_obj_set_size(cont, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_align(cont, LV_ALIGN_BOTTOM_MID, 0, 0);

    lv_obj_t * hour_roller = lv_roller_create(cont);
    lv_obj_set_flex_in_new_track(hour_roller, true);
    lv_subject_add_observer_obj(format_subject, hour_roller_options_update, hour_roller, NULL);
    lv_roller_bind_value(hour_roller, hour_subject);
    lv_obj_set_pos(hour_roller, 0, 0);

    lv_obj_t * min_roller = lv_roller_create(cont);
    lv_roller_set_options(min_roller, minute_options, LV_ROLLER_MODE_NORMAL);
    lv_roller_bind_value(min_roller, minute_subject);
    lv_obj_set_pos(min_roller, 64, 0);

    lv_obj_t * format_dropdown = lv_dropdown_create(cont);
    lv_dropdown_set_options(format_dropdown, "12\n24");
    lv_dropdown_bind_value(format_dropdown, format_subject);
    lv_obj_set_pos(format_dropdown, 128, 0);
    lv_obj_set_width(format_dropdown, 80);

    lv_obj_t * am_pm_dropdown = lv_dropdown_create(cont);
    lv_dropdown_set_options(am_pm_dropdown, "am\npm");
    lv_dropdown_bind_value(am_pm_dropdown, am_pm_subject);
    lv_subject_add_observer_obj(format_subject, am_pm_disabled_observer_cb, am_pm_dropdown, NULL);
    lv_obj_set_pos(am_pm_dropdown, 128, 48);
    lv_obj_set_width(am_pm_dropdown, 80);

    lv_obj_t * close_btn = lv_button_create(cont);
    lv_obj_align(close_btn, LV_ALIGN_TOP_RIGHT, 0, 0);
    /*Pass the set_btn as user_data to make it non-disabled on close*/
    lv_obj_add_event_cb(close_btn, close_clicked_event_cb, LV_EVENT_CLICKED, set_btn);

    lv_obj_t * close_label = lv_label_create(close_btn);
    lv_label_set_text(close_label, LV_SYMBOL_CLOSE);
}

static void close_clicked_event_cb(lv_event_t * e)
{
    lv_obj_t * set_btn = (lv_obj_t *) lv_event_get_user_data(e);
    lv_obj_t * close_btn = lv_event_get_target_obj(e);
    lv_obj_t * cont = lv_obj_get_parent(close_btn);
    lv_obj_remove_state(set_btn, LV_STATE_DISABLED);
    lv_obj_delete(cont);
}

/*Read every subject the time is made of, and join them into one `datetime_t`.
 *
 *This is what the construct is for. It does more than register the four dependencies:
 *it turns them into a single value with a single meaning. The signal a subscriber gets
 *is not "something changed, go and work out what the time is now", it is "the time is
 *now this" with the value attached. Two subscribers cannot drift in how they read it,
 *because neither of them reads the parts at all.
 *
 *The struct lives in the mapper's captured state (`user_data`), so the mapper stays
 *reusable and the example needs no allocation.*/
static bool time_mapper(lv_subject_t * subject, void * user_data, lv_subject_value_t input, const void ** value)
{
    LV_UNUSED(subject);
    LV_UNUSED(input);

    datetime_t * datetime = user_data;

    datetime_t next;
    next.hour = lv_subject_get_int(hour_subject);
    next.minute = lv_subject_get_int(minute_subject);
    next.format = (time_format_t)lv_subject_get_int(format_subject);
    next.pm = lv_subject_get_int(am_pm_subject) == TIME_PM;

    /*Publishing the same pointer every time, so compare the contents to decide
     *whether this is really a change.*/
    if(*value != NULL && lv_memcmp(datetime, &next, sizeof(next)) == 0) return false;

    *datetime = next;
    *value = datetime;
    return true;
}

/*The observer just uses the value it was handed. It does not know, and does not need to
 *know, that the time is kept as four separate subjects.*/
static void time_observer_cb(lv_observer_t * observer, lv_subject_t * subject)
{
    const datetime_t * datetime = lv_subject_get_pointer(subject);
    lv_obj_t * label = lv_observer_get_target_obj(observer);

    if(datetime->format == TIME_FORMAT_24) {
        lv_label_set_text_fmt(label, "%" LV_PRId32 ":%02" LV_PRId32, datetime->hour, datetime->minute);
    }
    else {
        lv_label_set_text_fmt(label, "%" LV_PRId32 ":%02" LV_PRId32 " %s",
                              datetime->hour + 1, datetime->minute, datetime->pm ? "pm" : "am");
    }
}

/*Change the hour options on format change*/
static void hour_roller_options_update(lv_observer_t * observer, lv_subject_t * subject)
{
    lv_obj_t * roller = lv_observer_get_target_obj(observer);
    int32_t prev_selected = lv_roller_get_selected(roller);
    int32_t v = lv_subject_get_int(subject);
    if(v == TIME_FORMAT_12) {
        prev_selected--;
        if(prev_selected > 12) prev_selected -= 12;
        lv_roller_set_options(roller, hour12_options, LV_ROLLER_MODE_NORMAL);
    }
    else {
        prev_selected++;
        lv_roller_set_options(roller, hour24_options, LV_ROLLER_MODE_NORMAL);
    }

    lv_roller_set_selected(roller, prev_selected, LV_ANIM_OFF);
    lv_obj_send_event(roller, LV_EVENT_VALUE_CHANGED, NULL);
}

#endif
