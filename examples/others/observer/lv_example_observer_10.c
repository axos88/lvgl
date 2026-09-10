#include "../../lv_examples.h"
#if LV_USE_OBSERVER && LV_USE_LABEL && LV_BUILD_EXAMPLES

typedef struct {
    int32_t x;
    int32_t y;
} touch_sample_t;

/*The buffer the driver writes into. Its address never changes.*/
static touch_sample_t sample;

/*What the mapper remembers, so it can tell whether the contents actually moved.
 *This is the mapper's captured state, handed to it as `user_data`.*/
typedef struct {
    touch_sample_t last_good;
    bool seeded;
} sample_watch_t;

static sample_watch_t watch;

/*What the driver publishes, and what the filtered subject is derived from.*/
static lv_subject_t * subject_raw;
static lv_subject_t * subject_sample;

static bool sample_changed_mapper(lv_subject_t * subject, void * user_data, const void ** value);
static void sample_observer_cb(lv_observer_t * observer, lv_subject_t * subject);

/**
 * @title Notifying only when the data behind a pointer really changed
 * @brief A driver rewrites the same pointer every poll; a mapper filters out the no-ops.
 *
 * A pointer subject notifies on every write, and it has to: the pointer may be
 * unchanged while the data behind it has been rewritten, and there is no way for LVGL
 * to know. For a driver that publishes the same buffer every few milliseconds that
 * means a notification per poll, whether anything moved or not.
 *
 * A derived subject fixes this. `subject_raw` is what the driver writes, and it does
 * notify on every poll. `subject_sample` is computed from it: it keeps a
 * last-known-good **copy** of the struct in its captured state, compares the contents
 * against it, and returns `false` when nothing moved. A mapper that reports no change
 * leaves the subject's version alone, so nothing downstream runs.
 *
 * Because the copy lives in the mapper's `user_data`, one mapper function can serve any
 * number of subjects, each with its own copy.
 *
 * Note what this cannot be done with: comparing pointers. The pointer is identical
 * every time. The comparison has to be on the contents, which means somebody has to
 * keep a copy, and the mapper is the right place for it.
 */
void lv_example_observer_10(void)
{
    static bool inited = false;

    if(!inited) {
        lv_memzero(&watch, sizeof(watch));

        subject_raw = lv_subject_create(LV_SUBJECT_TYPE_POINTER);

        subject_sample = lv_subject_create(LV_SUBJECT_TYPE_POINTER);
        lv_subject_set_pointer_mapper(subject_sample, sample_changed_mapper, &watch);

        /*Declared so `subject_raw` cannot be deleted while this mapper needs it.*/
        static lv_subject_t * sample_deps[1];
        sample_deps[0] = subject_raw;
        lv_subject_set_static_deps(subject_sample, sample_deps, 1);

        inited = true;
    }

    lv_obj_t * screen = lv_screen_active();
    lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(screen, 16, 0);

    lv_obj_t * label = lv_label_create(screen);
    lv_subject_add_observer_obj(subject_sample, sample_observer_cb, label, NULL);

    /* 💡 The same pointer is published four times, but only two are real changes. */
    sample.x = 10;
    sample.y = 20;
    lv_subject_set_pointer(subject_raw, &sample);      /*notifies: first value*/

    lv_subject_set_pointer(subject_raw, &sample);      /*silent: nothing moved*/

    sample.y = 21;                                     /*the driver mutates in place*/
    lv_subject_set_pointer(subject_raw, &sample);      /*notifies: y changed*/

    lv_subject_set_pointer(subject_raw, &sample);      /*silent again*/
}

static bool sample_changed_mapper(lv_subject_t * subject, void * user_data, const void ** value)
{
    LV_UNUSED(subject);
    sample_watch_t * w = user_data;
    /*Reading the raw subject is what registers it as a dependency, so this mapper re-runs
     *on every publish, and decides for itself whether that is worth telling anyone about.*/
    const touch_sample_t * now = lv_subject_get_pointer(subject_raw);
    if(now == NULL) return false;

    /*Compare the contents, not the pointer, against the copy kept from last time.*/
    if(w->seeded && lv_memcmp(&w->last_good, now, sizeof(*now)) == 0) {
        return false;   /*no change, so no notification*/
    }

    lv_memcpy(&w->last_good, now, sizeof(*now));
    w->seeded = true;
    *value = now;
    return true;
}

static void sample_observer_cb(lv_observer_t * observer, lv_subject_t * subject)
{
    lv_obj_t * label = lv_observer_get_target_obj(observer);
    const touch_sample_t * s = lv_subject_get_pointer(subject);

    lv_label_set_text_fmt(label, "x = %" LV_PRId32 ", y = %" LV_PRId32, s->x, s->y);
    LV_LOG_USER("sample moved to %" LV_PRId32 ", %" LV_PRId32, s->x, s->y);
}

#endif
