#include "../../lv_examples.h"
#if LV_USE_OBSERVER && LV_USE_LABEL && LV_BUILD_EXAMPLES

/*
 *              subject_raw            (a plain source)
 *               /        \
 *      subject_doubled  subject_offset   (two branches, both derived from raw)
 *               \        /
 *              subject_total          (joins the branches back together)
 *                   |
 *              subject_report         (a string, derived from the total)
 */
static lv_subject_t * subject_raw;
static lv_subject_t * subject_doubled;
static lv_subject_t * subject_offset;
static lv_subject_t * subject_total;
static lv_subject_t * subject_report;

/*Counters, only so the example can show how often each mapper actually runs*/
static uint32_t doubled_runs;
static uint32_t offset_runs;
static uint32_t total_runs;

static bool doubled_mapper(lv_subject_t * subject, void * user_data, int32_t * value);
static bool offset_mapper(lv_subject_t * subject, void * user_data, int32_t * value);
static bool total_mapper(lv_subject_t * subject, void * user_data, int32_t * value);
static bool report_mapper(lv_subject_t * subject, void * user_data, char * buf, size_t size);

/**
 * @title A branching dependency graph
 * @brief One source feeds two branches that join again; each mapper still runs once.
 *
 * Nothing here declares a dependency. Each mapper simply reads the subjects it needs,
 * and reading them is what wires the graph. `subject_total` reads `subject_doubled` and
 * `subject_offset`, so it depends on both; they each read `subject_raw`, so the whole
 * graph hangs off one source.
 *
 * Writing `subject_raw` therefore updates five subjects, and the interesting part is
 * *how*. Setting a value first marks the whole downstream graph stale, and only then
 * evaluates it, pulling each dependency in turn. So `subject_total` runs exactly once
 * per change, and it never sees a fresh `subject_doubled` next to a stale
 * `subject_offset`. Watch the logged counters: they go up by one each time, not two.
 *
 * A naive "notify my subscribers immediately" scheme would run `subject_total` twice
 * for every change, once with mismatched inputs.
 */
void lv_example_observer_9(void)
{
    static bool inited = false;

    if(!inited) {
        subject_raw = lv_subject_create(LV_SUBJECT_TYPE_INT);

        subject_doubled = lv_subject_create(LV_SUBJECT_TYPE_INT);
        lv_subject_set_int_mapper(subject_doubled, doubled_mapper, NULL);

        subject_offset = lv_subject_create(LV_SUBJECT_TYPE_INT);
        lv_subject_set_int_mapper(subject_offset, offset_mapper, NULL);

        subject_total = lv_subject_create(LV_SUBJECT_TYPE_INT);
        lv_subject_set_int_mapper(subject_total, total_mapper, NULL);

        static char report_buf[64];
        subject_report = lv_subject_create(LV_SUBJECT_TYPE_STRING);
        lv_subject_set_string_buffer_static(subject_report, report_buf, sizeof(report_buf));
        lv_subject_set_string_mapper(subject_report, report_mapper, NULL);

        inited = true;
    }

    lv_obj_t * screen = lv_screen_active();
    lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(screen, 8, 0);
    lv_obj_set_style_pad_all(screen, 16, 0);

    /*Binding a label to a subject is what gives it an observer, which is what makes
     *the subject evaluate eagerly. Without any observer a lazy subject is only
     *computed when something reads it.*/
    lv_obj_t * l_raw = lv_label_create(screen);
    lv_label_bind_text(l_raw, subject_raw, "raw      = %d");

    lv_obj_t * l_doubled = lv_label_create(screen);
    lv_label_bind_text(l_doubled, subject_doubled, "doubled  = %d");

    lv_obj_t * l_offset = lv_label_create(screen);
    lv_label_bind_text(l_offset, subject_offset, "offset   = %d");

    lv_obj_t * l_total = lv_label_create(screen);
    lv_label_bind_text(l_total, subject_total, "total    = %d");

    lv_obj_t * l_report = lv_label_create(screen);
    lv_label_bind_text(l_report, subject_report, NULL);

    /* 💡 One write, and the whole graph settles before this call returns. */
    doubled_runs = 0;
    offset_runs = 0;
    total_runs = 0;
    lv_subject_set_int(subject_raw, 5);
    LV_LOG_USER("after raw=5:  doubled ran %" LV_PRIu32 "x, offset ran %" LV_PRIu32 "x, total ran %" LV_PRIu32 "x",
                doubled_runs, offset_runs, total_runs);

    lv_subject_set_int(subject_raw, 6);
    LV_LOG_USER("after raw=6:  doubled ran %" LV_PRIu32 "x, offset ran %" LV_PRIu32 "x, total ran %" LV_PRIu32 "x",
                doubled_runs, offset_runs, total_runs);
}

static bool doubled_mapper(lv_subject_t * subject, void * user_data, int32_t * value)
{
    LV_UNUSED(subject);
    LV_UNUSED(user_data);
    doubled_runs++;

    int32_t next = lv_subject_get_int(subject_raw) * 2;
    if(next == *value) return false;
    *value = next;
    return true;
}

static bool offset_mapper(lv_subject_t * subject, void * user_data, int32_t * value)
{
    LV_UNUSED(subject);
    LV_UNUSED(user_data);
    offset_runs++;

    int32_t next = lv_subject_get_int(subject_raw) + 10;
    if(next == *value) return false;
    *value = next;
    return true;
}

/*The join. Both branches are read here, so both are dependencies, and both are
 *guaranteed to be up to date and consistent by the time they are read.*/
static bool total_mapper(lv_subject_t * subject, void * user_data, int32_t * value)
{
    LV_UNUSED(subject);
    LV_UNUSED(user_data);
    total_runs++;

    int32_t doubled = lv_subject_get_int(subject_doubled);
    int32_t offset = lv_subject_get_int(subject_offset);

    /*Both branches derive from the same source, so this invariant always holds.
     *It would break if the graph were evaluated one edge at a time.*/
    int32_t raw = lv_subject_get_int(subject_raw);
    if(doubled != raw * 2 || offset != raw + 10) {
        LV_LOG_WARN("inconsistent inputs: this must never happen");
    }

    int32_t next = doubled + offset;
    if(next == *value) return false;
    *value = next;
    return true;
}

/*A derived string. The mapper gets the buffer, which still holds the previous text, so
 *comparing against it is all that is needed to decide whether anything changed.*/
static bool report_mapper(lv_subject_t * subject, void * user_data, char * buf, size_t size)
{
    LV_UNUSED(subject);
    LV_UNUSED(user_data);

    char next[64];
    lv_snprintf(next, sizeof(next), "2*%" LV_PRId32 " + (%" LV_PRId32 "+10) = %" LV_PRId32,
                lv_subject_get_int(subject_raw), lv_subject_get_int(subject_raw),
                lv_subject_get_int(subject_total));

    if(lv_strcmp(next, buf) == 0) return false;
    lv_strlcpy(buf, next, size);
    return true;
}

#endif
