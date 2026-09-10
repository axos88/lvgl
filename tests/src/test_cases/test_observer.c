#if LV_BUILD_TEST == 1
#include "../lvgl.h"
#include "../../lvgl_private.h"

#include "unity/unity.h"

static uint32_t observer_called = 0;

/* Subjects are owned by LVGL now, so every test registers the ones it creates
 * here and tearDown() deletes them. */
static lv_subject_t * subjects[16] = {NULL};

/* Create a Subject and register it for automatic deletion in tearDown(). */
static lv_subject_t * subject_create(lv_subject_type_t type)
{
    for(size_t i = 0; i < LV_ARRAYLEN(subjects); ++i) {
        if(subjects[i] == NULL) {
            subjects[i] = lv_subject_create(type);
            TEST_ASSERT_NOT_NULL(subjects[i]);
            return subjects[i];
        }
    }
    TEST_FAIL_MESSAGE("subject pool exhausted");
    return NULL;
}

/* Drop a Subject from the registry without deleting it, for cases where something else
 * deletes it (a cascade) or where the test deletes it itself. */
static void subject_forget(lv_subject_t * subject)
{
    for(size_t i = 0; i < LV_ARRAYLEN(subjects); ++i) {
        if(subjects[i] == subject) subjects[i] = NULL;
    }
}

/* Delete a registered Subject early, so tearDown() doesn't delete it twice. */
static void subject_delete(lv_subject_t * subject)
{
    for(size_t i = 0; i < LV_ARRAYLEN(subjects); ++i) {
        if(subjects[i] == subject) subjects[i] = NULL;
    }
    lv_subject_delete(subject);
}

void setUp(void)
{
    observer_called = 0;
    /* Function run before every test */
}

void tearDown(void)
{
    /* Function run after every test */
    lv_obj_clean(lv_screen_active());

    /* lv_subject_delete() refuses while something still depends on a Subject, so peel
     * the graph from the leaves inward: repeatedly delete whatever has no dependents.
     * The dependency graph is acyclic, so this drains it. */
    bool progress = true;
    while(progress) {
        progress = false;
        for(size_t i = 0; i < LV_ARRAYLEN(subjects); ++i) {
            if(subjects[i] == NULL) continue;
            if(lv_ll_get_len(&subjects[i]->dependents) > 0) continue;
            if(subjects[i]->static_ref_cnt > 0) continue;
            lv_subject_delete(subjects[i]);
            subjects[i] = NULL;
            progress = true;
        }
    }

    /* Anything left would mean a dependency cycle, which must not be possible. */
    for(size_t i = 0; i < LV_ARRAYLEN(subjects); ++i) {
        TEST_ASSERT_NULL(subjects[i]);
    }
}

static int32_t current_v;

static void observer_basic(lv_observer_t * observer, lv_subject_t * subject)
{
    LV_UNUSED(observer);
    LV_UNUSED(subject);
    observer_called++;
}

static void observer_int(lv_observer_t * observer, lv_subject_t * subject)
{
    LV_UNUSED(observer);
    current_v = lv_subject_get_int(subject);
}

void test_observer_add_remove(void)
{
    LV_DEPRECATIONS_IGNORE_BEGIN
    static lv_subject_t subject;
    lv_subject_init_int(&subject, 5);
    LV_DEPRECATIONS_IGNORE_END
    lv_subject_t * created = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int(created, 5);
    lv_subject_t * test_subjects[] = {&subject, created};

    for(size_t i = 0; i < LV_ARRAYLEN(test_subjects); ++i) {
        lv_subject_t * sub = test_subjects[i];

        lv_observer_t * observer =
            lv_subject_add_observer(sub, observer_int, NULL);

        current_v = 0;
        lv_subject_set_int(sub, 10);
        TEST_ASSERT_EQUAL(10, lv_subject_get_int(sub));
        TEST_ASSERT_EQUAL(10, current_v);

        lv_observer_delete(observer);

        lv_subject_set_int(sub, 15);
        TEST_ASSERT_EQUAL(15, lv_subject_get_int(sub));
        TEST_ASSERT_EQUAL(10, current_v); /*The observer cb is not called*/
    }

    LV_DEPRECATIONS_IGNORE_BEGIN
    lv_subject_deinit(&subject);
    LV_DEPRECATIONS_IGNORE_END

    static lv_subject_t uninitialized_subject;
    lv_observer_t * observer = lv_subject_add_observer(&uninitialized_subject, observer_int,
                                                       NULL);
    TEST_ASSERT_EQUAL_PTR(NULL, observer); /*The observer must be NULL*/
}

void test_object_observer_add_remove(void)
{
    LV_DEPRECATIONS_IGNORE_BEGIN
    lv_obj_t * obj = lv_obj_create(lv_screen_active());

    lv_subject_t * subject = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int(subject, 1);

    lv_observer_t * observer = lv_obj_bind_flag_if_eq(obj, subject, LV_OBJ_FLAG_HIDDEN, 5);

    TEST_ASSERT_EQUAL(false, lv_obj_is_hidden(obj));
    lv_subject_set_int(subject, 5);
    TEST_ASSERT_EQUAL(true, lv_obj_is_hidden(obj));
    lv_observer_delete(observer);
    lv_subject_set_int(subject, 1);

    /* This shouldn't get updated */
    TEST_ASSERT_EQUAL(true, lv_obj_is_hidden(obj));
    lv_obj_delete(obj);
    /* We shouldn't crash here */
    LV_DEPRECATIONS_IGNORE_END
}

static lv_event_dsc_t * get_event_delete_from_obj(lv_obj_t * obj)
{

    /* The remove event is a event callback using the observer as the user data */
    uint32_t event_cnt = lv_event_get_count(&obj->spec_attr->event_list);
    for(uint32_t i = 0; i < event_cnt; i++) {
        lv_event_dsc_t * event = lv_obj_get_event_dsc(obj, i);
        TEST_ASSERT_NOT_NULL(event);
        if(event->filter == LV_EVENT_DELETE) {
            return event;
        }
    }
    return NULL;
}
void test_obj_remove_from_subject_removes_delete_event(void)
{

    lv_obj_t * obj = lv_obj_create(lv_screen_active());

    lv_subject_t * subject = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int(subject, 1);
    (void)lv_subject_add_observer_obj(subject, observer_basic, obj, NULL);

    {
        /*
         * We expect the event delete to be added to the object allowing the observer
         * to be deleted when the object is deleted
         */
        TEST_ASSERT_NOT_NULL(obj->spec_attr);
        TEST_ASSERT_EQUAL(lv_event_get_count(&obj->spec_attr->event_list), 1);
        lv_event_dsc_t * delete_event = get_event_delete_from_obj(obj);
        TEST_ASSERT_NOT_NULL(delete_event);
    }
    {
        /* Removing the object from the subject should remove the delete event entry */
        lv_obj_remove_from_subject(obj, subject);
        lv_event_dsc_t * delete_event  = get_event_delete_from_obj(obj);
        TEST_ASSERT_NULL(delete_event);
    }

}

void test_observer_remove_removes_obj_callback(void)
{
    lv_obj_t * obj = lv_obj_create(lv_screen_active());

    lv_subject_t * subject = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int(subject, 1);
    lv_observer_t * observer = lv_subject_add_observer_obj(subject, observer_basic, obj, NULL);

    {
        /*
         * We expect the event delete to be added to the object allowing the observer
         * to be deleted when the object is deleted
         */
        TEST_ASSERT_NOT_NULL(obj->spec_attr);
        TEST_ASSERT_EQUAL(lv_event_get_count(&obj->spec_attr->event_list), 1);
        lv_event_dsc_t * delete_event = get_event_delete_from_obj(obj);
        TEST_ASSERT_NOT_NULL(delete_event);
    }
    {
        /* Removing the observer associated with the object should remove the delete event entry */
        lv_observer_delete(observer);
        lv_event_dsc_t * delete_event  = get_event_delete_from_obj(obj);
        TEST_ASSERT_NULL(delete_event);
    }
}

void test_observer_int(void)
{
    lv_subject_t * subject = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int(subject, 5);
    lv_observer_t * basic_observer =
        lv_subject_add_observer(subject, observer_basic, NULL);

    /*A new Subject starts at 0, so the first set leaves 0 as the previous value*/
    TEST_ASSERT_EQUAL(5, lv_subject_get_int(subject));
    TEST_ASSERT_EQUAL(1, observer_called);

    lv_subject_set_int(subject, 10);
    TEST_ASSERT_EQUAL(10, lv_subject_get_int(subject));
    TEST_ASSERT_EQUAL(2, observer_called);

    lv_subject_set_int(subject, 15);
    TEST_ASSERT_EQUAL(15, lv_subject_get_int(subject));
    TEST_ASSERT_EQUAL(3, observer_called);

    /* Observer shouldn't be called if value is the same */
    lv_subject_set_int(subject, 15);
    TEST_ASSERT_EQUAL(15, lv_subject_get_int(subject));
    TEST_ASSERT_EQUAL(3, observer_called);

    /*Ignore incorrect types*/
    lv_subject_set_pointer(subject, NULL);
    TEST_ASSERT_EQUAL(15, lv_subject_get_int(subject));
    TEST_ASSERT_EQUAL(3, observer_called);

    lv_subject_set_color(subject, lv_color_black());
    TEST_ASSERT_EQUAL(15, lv_subject_get_int(subject));
    TEST_ASSERT_EQUAL(3, observer_called);

    lv_subject_copy_string(subject, "hello");
    TEST_ASSERT_EQUAL(15, lv_subject_get_int(subject));
    TEST_ASSERT_EQUAL(3, observer_called);

    lv_observer_delete(basic_observer);
}

void test_observer_float(void)
{
    lv_subject_t * subject = subject_create(LV_SUBJECT_TYPE_FLOAT);
    lv_subject_set_float(subject, 5.25);
    lv_observer_t * basic_observer =
        lv_subject_add_observer(subject, observer_basic, NULL);

    /*A new Subject starts at 0, so the first set leaves 0 as the previous value*/
    TEST_ASSERT_EQUAL_FLOAT(5.25, lv_subject_get_float(subject));
    TEST_ASSERT_EQUAL(1, observer_called);

    lv_subject_set_float(subject, 10.5);
    TEST_ASSERT_EQUAL_FLOAT(10.5, lv_subject_get_float(subject));
    TEST_ASSERT_EQUAL(2, observer_called);

    lv_subject_set_float(subject, 15.75);
    TEST_ASSERT_EQUAL_FLOAT(15.75, lv_subject_get_float(subject));
    TEST_ASSERT_EQUAL(3, observer_called);

    /* Observer shouldn't be called if value is the same */
    lv_subject_set_float(subject, 15.75);
    TEST_ASSERT_EQUAL_FLOAT(15.75, lv_subject_get_float(subject));
    TEST_ASSERT_EQUAL(3, observer_called);

    /*Ignore incorrect types*/
    lv_subject_set_pointer(subject, NULL);
    TEST_ASSERT_EQUAL_FLOAT(15.75, lv_subject_get_float(subject));
    TEST_ASSERT_EQUAL(3, observer_called);

    lv_subject_set_color(subject, lv_color_black());
    TEST_ASSERT_EQUAL_FLOAT(15.75, lv_subject_get_float(subject));
    TEST_ASSERT_EQUAL(3, observer_called);

    lv_subject_copy_string(subject, "hello");
    TEST_ASSERT_EQUAL_FLOAT(15.75, lv_subject_get_float(subject));
    TEST_ASSERT_EQUAL(3, observer_called);

    lv_observer_delete(basic_observer);
}

void test_observer_string(void)
{
    char buf_current[32];
    lv_subject_t * subject = subject_create(LV_SUBJECT_TYPE_STRING);
    lv_subject_set_string_buffer_static(subject, buf_current, sizeof(buf_current));
    lv_subject_copy_string(subject, "hello");

    lv_observer_t * basic_observer =
        lv_subject_add_observer(subject, observer_basic, NULL);

    /*The buffers start empty, so the first copy leaves "" as the previous value*/
    TEST_ASSERT_EQUAL_STRING("hello", lv_subject_get_string(subject));
    TEST_ASSERT_EQUAL(1, observer_called);

    lv_subject_copy_string(subject, "my name is John");
    TEST_ASSERT_EQUAL_STRING("my name is John",
                             lv_subject_get_string(subject));
    TEST_ASSERT_EQUAL(2, observer_called);

    lv_subject_copy_string(subject, "how are you?");
    TEST_ASSERT_EQUAL_STRING("how are you?",
                             lv_subject_get_string(subject));
    TEST_ASSERT_EQUAL(3, observer_called);

    /* Observer shouldn't be called with same value */
    lv_subject_copy_string(subject, "how are you?");
    TEST_ASSERT_EQUAL_STRING("how are you?",
                             lv_subject_get_string(subject));
    TEST_ASSERT_EQUAL(3, observer_called);

    lv_subject_snprintf(subject, "I ate %d pizzas", 10);
    TEST_ASSERT_EQUAL_STRING("I ate 10 pizzas",
                             lv_subject_get_string(subject));
    TEST_ASSERT_EQUAL(4, observer_called);

    lv_subject_snprintf(subject, "%d: %s", 1, "Coding is fun !");
    TEST_ASSERT_EQUAL_STRING("1: Coding is fun !",
                             lv_subject_get_string(subject));
    TEST_ASSERT_EQUAL(5, observer_called);

    /* Observer shouldn't be called with same value */
    lv_subject_snprintf(subject, "%d: %s", 1, "Coding is fun !");
    TEST_ASSERT_EQUAL_STRING("1: Coding is fun !",
                             lv_subject_get_string(subject));
    TEST_ASSERT_EQUAL(5, observer_called);

    /* A string that does not fit a fixed buffer is refused rather than silently
     * truncated, so the Subject keeps the value it had and nobody is notified. */
    TEST_ASSERT_FALSE(lv_subject_copy_string(
                          subject,
                          "text to be clipped to 32 chars.this should be clipped"));
    TEST_ASSERT_EQUAL_STRING("1: Coding is fun !",
                             lv_subject_get_string(subject));
    TEST_ASSERT_EQUAL(5, observer_called);

    lv_subject_copy_string(subject, "a");
    TEST_ASSERT_EQUAL_STRING("a", lv_subject_get_string(subject));
    TEST_ASSERT_EQUAL(6, observer_called);

    /*Ignore incorrect types*/
    lv_subject_set_pointer(subject, NULL);
    TEST_ASSERT_EQUAL_STRING("a", lv_subject_get_string(subject));
    TEST_ASSERT_EQUAL(6, observer_called);

    lv_subject_set_color(subject, lv_color_black());
    TEST_ASSERT_EQUAL_STRING("a", lv_subject_get_string(subject));
    TEST_ASSERT_EQUAL(6, observer_called);

    lv_subject_set_int(subject, 10);
    TEST_ASSERT_EQUAL_STRING("a", lv_subject_get_string(subject));
    TEST_ASSERT_EQUAL(6, observer_called);
    lv_observer_delete(basic_observer);
}

void test_observer_pointer(void)
{
    static int32_t a[3] = { 0 };

    lv_subject_t * subject = subject_create(LV_SUBJECT_TYPE_POINTER);
    lv_subject_set_pointer(subject, &a[0]);

    lv_observer_t * basic_observer =
        lv_subject_add_observer(subject, observer_basic, NULL);

    TEST_ASSERT_EQUAL(1, observer_called);

    /*A new Subject starts at NULL, so the first set leaves NULL as the previous value*/
    TEST_ASSERT_EQUAL_PTR(&a[0], lv_subject_get_pointer(subject));

    lv_subject_set_pointer(subject, &a[1]);
    TEST_ASSERT_EQUAL_PTR(&a[1], lv_subject_get_pointer(subject));
    TEST_ASSERT_EQUAL(2, observer_called);

    lv_subject_set_pointer(subject, &a[2]);
    TEST_ASSERT_EQUAL_PTR(&a[2], lv_subject_get_pointer(subject));
    TEST_ASSERT_EQUAL(3, observer_called);

    /*
     * Even if pointer is the same, the observer should still get called as we shouldn't assume
     * what the pointer is indicating
     */
    lv_subject_set_pointer(subject, &a[2]);
    TEST_ASSERT_EQUAL_PTR(&a[2], lv_subject_get_pointer(subject));
    TEST_ASSERT_EQUAL(4, observer_called);

    /*Ignore incorrect types*/
    lv_subject_set_int(subject, 10);
    TEST_ASSERT_EQUAL_PTR(&a[2], lv_subject_get_pointer(subject));
    TEST_ASSERT_EQUAL(4, observer_called);

    lv_subject_set_color(subject, lv_color_black());
    TEST_ASSERT_EQUAL_PTR(&a[2], lv_subject_get_pointer(subject));
    TEST_ASSERT_EQUAL(4, observer_called);

    lv_subject_copy_string(subject, "hello");
    TEST_ASSERT_EQUAL_PTR(&a[2], lv_subject_get_pointer(subject));
    TEST_ASSERT_EQUAL(4, observer_called);
    lv_observer_delete(basic_observer);
}

void test_observer_color(void)
{
    lv_subject_t * subject = subject_create(LV_SUBJECT_TYPE_COLOR);
    lv_subject_set_color(subject, lv_color_hex3(0x123));
    lv_observer_t * basic_observer =
        lv_subject_add_observer(subject, observer_basic, NULL);

    /*A new Subject starts at black, so the first set leaves black as the previous value*/
    TEST_ASSERT_EQUAL_COLOR(lv_color_hex3(0x123),
                            lv_subject_get_color(subject));
    TEST_ASSERT_EQUAL(1, observer_called);

    lv_subject_set_color(subject, lv_color_hex3(0x456));
    TEST_ASSERT_EQUAL_COLOR(lv_color_hex3(0x456),
                            lv_subject_get_color(subject));
    TEST_ASSERT_EQUAL(2, observer_called);

    lv_subject_set_color(subject, lv_color_hex3(0xabc));
    TEST_ASSERT_EQUAL_COLOR(lv_color_hex3(0xabc),
                            lv_subject_get_color(subject));
    TEST_ASSERT_EQUAL(3, observer_called);

    /* Observer shouldn't be called if value is the same */
    lv_subject_set_color(subject, lv_color_hex3(0xabc));
    TEST_ASSERT_EQUAL_COLOR(lv_color_hex3(0xabc),
                            lv_subject_get_color(subject));
    TEST_ASSERT_EQUAL(3, observer_called);

    /*Ignore incorrect types*/
    lv_subject_set_pointer(subject, NULL);
    TEST_ASSERT_EQUAL_COLOR(lv_color_hex3(0xabc),
                            lv_subject_get_color(subject));

    lv_subject_set_int(subject, 10);
    TEST_ASSERT_EQUAL_COLOR(lv_color_hex3(0xabc),
                            lv_subject_get_color(subject));

    lv_subject_copy_string(subject, "hello");
    TEST_ASSERT_EQUAL_COLOR(lv_color_hex3(0xabc),
                            lv_subject_get_color(subject));
    lv_observer_delete(basic_observer);
}

static int32_t group_observer_called;
static int32_t group_value_sum;

/* What used to be a Group Subject: an LV_SUBJECT_TYPE_NONE Subject whose mapper reads
 * the Subjects it wants to react to. Reading them is what wires the dependencies. */
static lv_subject_t * agg_list[5];
static uint32_t agg_count;

static bool aggregate_mapper(lv_subject_t * subject, void * user_data)
{
    LV_UNUSED(user_data);
    LV_UNUSED(subject);
    for(uint32_t i = 0; i < agg_count; i++) {
        (void)lv_subject_get_int(agg_list[i]);
    }
    return true;  /* notify whenever any member changed */
}

static void group_observer_cb(lv_observer_t * observer, lv_subject_t * subject)
{
    LV_UNUSED(observer);
    LV_UNUSED(subject);
    group_observer_called++;

    group_value_sum = 0;
    for(uint32_t i = 0; i < agg_count; i++) {
        group_value_sum += lv_subject_get_int(agg_list[i]);
    }
}

void test_observer_aggregate_subject(void)
{
    for(uint32_t i = 0; i < LV_ARRAYLEN(agg_list); i++) {
        agg_list[i] = subject_create(LV_SUBJECT_TYPE_INT);
        lv_subject_set_int(agg_list[i], (int32_t)i + 1);
    }
    agg_count = 2;

    lv_subject_t * aggregate = subject_create(LV_SUBJECT_TYPE_NONE);
    /* An LV_SUBJECT_TYPE_NONE Subject is eager by default, so its Observers fire as
     * soon as any input changes, the way a Group's did. */
    TEST_ASSERT_EQUAL(LV_SUBJECT_MODE_EAGER, lv_subject_get_mode(aggregate));
    lv_subject_set_none_mapper(aggregate, aggregate_mapper, NULL);

    /* The members are ordinary dependencies now, discoverable through the graph. */
    TEST_ASSERT_EQUAL(2, lv_subject_get_dependency_count(aggregate));
    TEST_ASSERT_EQUAL_PTR(agg_list[0], lv_subject_get_dependency(aggregate, 0));
    TEST_ASSERT_EQUAL_PTR(agg_list[1], lv_subject_get_dependency(aggregate, 1));

    group_observer_called = 0;
    lv_subject_add_observer(aggregate, group_observer_cb, NULL);
    TEST_ASSERT_EQUAL(1, group_observer_called);
    TEST_ASSERT_EQUAL(1 + 2, group_value_sum);

    lv_subject_set_int(agg_list[0], 10);
    TEST_ASSERT_EQUAL(2, group_observer_called);
    TEST_ASSERT_EQUAL(10 + 2, group_value_sum);

    lv_subject_set_int(agg_list[1], 20);
    TEST_ASSERT_EQUAL(3, group_observer_called);
    TEST_ASSERT_EQUAL(10 + 20, group_value_sum);

    /* A member that is not read is not a dependency, so it changes nothing. */
    lv_subject_set_int(agg_list[4], 99);
    TEST_ASSERT_EQUAL(3, group_observer_called);
}

void test_observer_aggregate_delete_removes_edges(void)
{
    agg_list[0] = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int(agg_list[0], 1);
    agg_list[1] = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int(agg_list[1], 2);
    agg_count = 2;

    lv_subject_t * aggregate = subject_create(LV_SUBJECT_TYPE_NONE);
    lv_subject_set_none_mapper(aggregate, aggregate_mapper, NULL);

    /* Dependency edges, not hidden Observers: `subs_ll` stays empty. */
    TEST_ASSERT_EQUAL(0, lv_ll_get_len(&agg_list[0]->subs_ll));
    TEST_ASSERT_EQUAL(0, lv_ll_get_len(&agg_list[1]->subs_ll));
    TEST_ASSERT_EQUAL(1, lv_ll_get_len(&agg_list[0]->dependents));
    TEST_ASSERT_EQUAL(1, lv_ll_get_len(&agg_list[1]->dependents));

    group_observer_called = 0;
    lv_subject_add_observer(aggregate, group_observer_cb, NULL);
    TEST_ASSERT_EQUAL(1, group_observer_called);

    /* Deleting the aggregate must drop its edges, otherwise its members would keep
     * marking freed memory dirty. */
    subject_delete(aggregate);

    TEST_ASSERT_EQUAL(0, lv_ll_get_len(&agg_list[0]->dependents));
    TEST_ASSERT_EQUAL(0, lv_ll_get_len(&agg_list[1]->dependents));

    group_observer_called = 0;
    lv_subject_set_int(agg_list[0], 10);
    lv_subject_set_int(agg_list[1], 20);
    TEST_ASSERT_EQUAL(0, group_observer_called);
}

void test_observer_obj_flag_invalid_subject(void)
{
    LV_DEPRECATIONS_IGNORE_BEGIN
    typedef lv_observer_t * (*lv_obj_bind_flag_fn)(
        lv_obj_t *, lv_subject_t *, lv_obj_flag_t, int32_t);
    static const lv_obj_bind_flag_fn fns[] = {
        lv_obj_bind_flag_if_eq, lv_obj_bind_flag_if_not_eq,
        lv_obj_bind_flag_if_ge, lv_obj_bind_flag_if_gt,
        lv_obj_bind_flag_if_lt, lv_obj_bind_flag_if_le,
    };
    LV_DEPRECATIONS_IGNORE_END

    lv_obj_t * obj = lv_obj_create(lv_screen_active());

    /* Can only bind to int */
    lv_subject_t * invalid[4];
    invalid[0] = subject_create(LV_SUBJECT_TYPE_POINTER);
    invalid[1] = subject_create(LV_SUBJECT_TYPE_STRING);
    invalid[2] = subject_create(LV_SUBJECT_TYPE_COLOR);
    invalid[3] = subject_create(LV_SUBJECT_TYPE_NONE);

    for(size_t i = 0; i < LV_ARRAYLEN(fns); ++i) {
        for(size_t j = 0; j < LV_ARRAYLEN(invalid); ++j) {
            TEST_ASSERT_EQUAL_PTR(NULL,
                                  fns[i](obj, invalid[j],
                                         LV_OBJ_FLAG_HIDDEN, 5));
        }
    }
}
void test_observer_obj_flag_eq(void)
{
    LV_DEPRECATIONS_IGNORE_BEGIN
    lv_obj_t * obj = lv_obj_create(lv_screen_active());

    lv_subject_t * subject = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int(subject, 1);

    lv_obj_bind_flag_if_eq(obj, subject, LV_OBJ_FLAG_HIDDEN, 5);
    /*Should be applied immediately*/
    TEST_ASSERT_EQUAL(false, lv_obj_is_hidden(obj));

    lv_obj_bind_flag_if_not_eq(obj, subject, LV_OBJ_FLAG_CHECKABLE, 10);
    /*Should be applied immediately*/
    TEST_ASSERT_EQUAL(false, lv_obj_is_hidden(obj));
    TEST_ASSERT_EQUAL(true, lv_obj_is_checkable(obj));

    lv_subject_set_int(subject, 5);
    TEST_ASSERT_EQUAL(true, lv_obj_is_hidden(obj));
    TEST_ASSERT_EQUAL(true, lv_obj_is_checkable(obj));

    lv_subject_set_int(subject, 10);
    TEST_ASSERT_EQUAL(false, lv_obj_is_hidden(obj));
    TEST_ASSERT_EQUAL(false, lv_obj_is_checkable(obj));
    LV_DEPRECATIONS_IGNORE_END
}

void test_observer_obj_flag_ge(void)
{
    LV_DEPRECATIONS_IGNORE_BEGIN
    lv_obj_t * obj = lv_obj_create(lv_screen_active());

    lv_subject_t * subject = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int(subject, 1);

    lv_obj_bind_flag_if_ge(obj, subject, LV_OBJ_FLAG_HIDDEN, 5);
    /*Should be applied immediately*/
    TEST_ASSERT_EQUAL(false, lv_obj_is_hidden(obj));

    lv_subject_set_int(subject, 5);
    TEST_ASSERT_EQUAL(true, lv_obj_is_hidden(obj));

    lv_subject_set_int(subject, 4);
    TEST_ASSERT_EQUAL(false, lv_obj_is_hidden(obj));

    lv_subject_set_int(subject, 6);
    TEST_ASSERT_EQUAL(true, lv_obj_is_hidden(obj));
    LV_DEPRECATIONS_IGNORE_END
}

void test_observer_obj_flag_gt(void)
{
    LV_DEPRECATIONS_IGNORE_BEGIN
    lv_obj_t * obj = lv_obj_create(lv_screen_active());

    lv_subject_t * subject = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int(subject, 1);

    lv_obj_bind_flag_if_gt(obj, subject, LV_OBJ_FLAG_HIDDEN, 5);
    /*Should be applied immediately*/
    TEST_ASSERT_EQUAL(false, lv_obj_is_hidden(obj));

    lv_subject_set_int(subject, 5);
    TEST_ASSERT_EQUAL(false, lv_obj_is_hidden(obj));

    lv_subject_set_int(subject, 6);
    TEST_ASSERT_EQUAL(true, lv_obj_is_hidden(obj));

    lv_subject_set_int(subject, 4);
    TEST_ASSERT_EQUAL(false, lv_obj_is_hidden(obj));
    LV_DEPRECATIONS_IGNORE_END
}

void test_observer_obj_flag_le(void)
{
    LV_DEPRECATIONS_IGNORE_BEGIN
    lv_obj_t * obj = lv_obj_create(lv_screen_active());

    lv_subject_t * subject = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int(subject, 7);

    lv_obj_bind_flag_if_le(obj, subject, LV_OBJ_FLAG_HIDDEN, 5);
    /*Should be applied immediately*/
    TEST_ASSERT_EQUAL(false, lv_obj_is_hidden(obj));

    lv_subject_set_int(subject, 5);
    TEST_ASSERT_EQUAL(true, lv_obj_is_hidden(obj));

    lv_subject_set_int(subject, 6);
    TEST_ASSERT_EQUAL(false, lv_obj_is_hidden(obj));

    lv_subject_set_int(subject, 4);
    TEST_ASSERT_EQUAL(true, lv_obj_is_hidden(obj));
    LV_DEPRECATIONS_IGNORE_END
}

void test_observer_obj_flag_lt(void)
{
    LV_DEPRECATIONS_IGNORE_BEGIN
    lv_obj_t * obj = lv_obj_create(lv_screen_active());

    lv_subject_t * subject = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int(subject, 7);

    lv_obj_bind_flag_if_lt(obj, subject, LV_OBJ_FLAG_HIDDEN, 5);
    /*Should be applied immediately*/
    TEST_ASSERT_EQUAL(false, lv_obj_is_hidden(obj));

    lv_subject_set_int(subject, 4);
    TEST_ASSERT_EQUAL(true, lv_obj_is_hidden(obj));

    lv_subject_set_int(subject, 5);
    TEST_ASSERT_EQUAL(false, lv_obj_is_hidden(obj));

    lv_subject_set_int(subject, 3);
    TEST_ASSERT_EQUAL(true, lv_obj_is_hidden(obj));
    LV_DEPRECATIONS_IGNORE_END
}

void test_observer_obj_state_invalid_subject(void)
{
    LV_DEPRECATIONS_IGNORE_BEGIN
    typedef lv_observer_t * (*lv_obj_bind_state_fn)(
        lv_obj_t *, lv_subject_t *, lv_state_t, int32_t);

    static const lv_obj_bind_state_fn fns[] = {
        lv_obj_bind_state_if_eq, lv_obj_bind_state_if_not_eq,
        lv_obj_bind_state_if_ge, lv_obj_bind_state_if_gt,
        lv_obj_bind_state_if_lt, lv_obj_bind_state_if_le,
    };
    lv_obj_t * obj = lv_obj_create(lv_screen_active());

    /* Can only bind to int */
    lv_subject_t * invalid[4];
    invalid[0] = subject_create(LV_SUBJECT_TYPE_POINTER);
    invalid[1] = subject_create(LV_SUBJECT_TYPE_STRING);
    invalid[2] = subject_create(LV_SUBJECT_TYPE_COLOR);
    invalid[3] = subject_create(LV_SUBJECT_TYPE_NONE);

    for(size_t i = 0; i < LV_ARRAYLEN(fns); ++i) {
        for(size_t j = 0; j < LV_ARRAYLEN(invalid); ++j) {
            TEST_ASSERT_EQUAL_PTR(
                NULL, fns[i](obj, invalid[j], 0, 5));
        }
    }
    LV_DEPRECATIONS_IGNORE_END
}

void test_observer_obj_state_eq(void)
{
    LV_DEPRECATIONS_IGNORE_BEGIN
    lv_obj_t * obj = lv_obj_create(lv_screen_active());

    lv_subject_t * subject = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int(subject, 1);

    lv_obj_bind_state_if_eq(obj, subject, LV_STATE_CHECKED, 5);
    /*Should be applied immediately*/
    TEST_ASSERT_EQUAL(false, lv_obj_has_state(obj, LV_STATE_CHECKED));

    lv_obj_bind_state_if_not_eq(obj, subject, LV_STATE_DISABLED, 10);
    /*Should be applied immediately*/
    TEST_ASSERT_EQUAL(false, lv_obj_has_state(obj, LV_STATE_CHECKED));
    TEST_ASSERT_EQUAL(true, lv_obj_has_state(obj, LV_STATE_DISABLED));

    lv_subject_set_int(subject, 5);
    TEST_ASSERT_EQUAL(true, lv_obj_has_state(obj, LV_STATE_CHECKED));
    TEST_ASSERT_EQUAL(true, lv_obj_has_state(obj, LV_STATE_DISABLED));

    lv_subject_set_int(subject, 10);
    TEST_ASSERT_EQUAL(false, lv_obj_has_state(obj, LV_STATE_CHECKED));
    TEST_ASSERT_EQUAL(false, lv_obj_has_state(obj, LV_STATE_DISABLED));
    LV_DEPRECATIONS_IGNORE_END
}

void test_observer_obj_state_gt(void)
{
    LV_DEPRECATIONS_IGNORE_BEGIN
    lv_obj_t * obj = lv_obj_create(lv_screen_active());

    lv_subject_t * subject = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int(subject, 1);

    lv_obj_bind_state_if_gt(obj, subject, LV_STATE_CHECKED, 5);
    /*Should be applied immediately*/
    TEST_ASSERT_EQUAL(false, lv_obj_has_state(obj, LV_STATE_CHECKED));

    lv_subject_set_int(subject, 6);
    TEST_ASSERT_EQUAL(true, lv_obj_has_state(obj, LV_STATE_CHECKED));

    lv_subject_set_int(subject, 5);
    TEST_ASSERT_EQUAL(false, lv_obj_has_state(obj, LV_STATE_CHECKED));

    lv_subject_set_int(subject, 7);
    TEST_ASSERT_EQUAL(true, lv_obj_has_state(obj, LV_STATE_CHECKED));
    LV_DEPRECATIONS_IGNORE_END
}

void test_observer_obj_state_ge(void)
{
    LV_DEPRECATIONS_IGNORE_BEGIN
    lv_obj_t * obj = lv_obj_create(lv_screen_active());

    lv_subject_t * subject = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int(subject, 1);

    lv_obj_bind_state_if_ge(obj, subject, LV_STATE_CHECKED, 5);
    /*Should be applied immediately*/
    TEST_ASSERT_EQUAL(false, lv_obj_has_state(obj, LV_STATE_CHECKED));

    lv_subject_set_int(subject, 6);
    TEST_ASSERT_EQUAL(true, lv_obj_has_state(obj, LV_STATE_CHECKED));

    lv_subject_set_int(subject, 4);
    TEST_ASSERT_EQUAL(false, lv_obj_has_state(obj, LV_STATE_CHECKED));

    lv_subject_set_int(subject, 5);
    TEST_ASSERT_EQUAL(true, lv_obj_has_state(obj, LV_STATE_CHECKED));
    LV_DEPRECATIONS_IGNORE_END
}

void test_observer_obj_state_le(void)
{
    LV_DEPRECATIONS_IGNORE_BEGIN
    lv_obj_t * obj = lv_obj_create(lv_screen_active());

    lv_subject_t * subject = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int(subject, 1);

    lv_obj_bind_state_if_le(obj, subject, LV_STATE_CHECKED, 5);
    /*Should be applied immediately*/
    TEST_ASSERT_EQUAL(true, lv_obj_has_state(obj, LV_STATE_CHECKED));

    lv_subject_set_int(subject, 6);
    TEST_ASSERT_EQUAL(false, lv_obj_has_state(obj, LV_STATE_CHECKED));

    lv_subject_set_int(subject, 4);
    TEST_ASSERT_EQUAL(true, lv_obj_has_state(obj, LV_STATE_CHECKED));
    LV_DEPRECATIONS_IGNORE_END
}

void test_observer_obj_state_lt(void)
{
    LV_DEPRECATIONS_IGNORE_BEGIN
    lv_obj_t * obj = lv_obj_create(lv_screen_active());

    lv_subject_t * subject = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int(subject, 1);

    lv_obj_bind_state_if_lt(obj, subject, LV_STATE_CHECKED, 5);
    /*Should be applied immediately*/
    TEST_ASSERT_EQUAL(true, lv_obj_has_state(obj, LV_STATE_CHECKED));

    lv_subject_set_int(subject, 5);
    TEST_ASSERT_EQUAL(false, lv_obj_has_state(obj, LV_STATE_CHECKED));

    lv_subject_set_int(subject, 4);
    TEST_ASSERT_EQUAL(true, lv_obj_has_state(obj, LV_STATE_CHECKED));
    LV_DEPRECATIONS_IGNORE_END
}

/* Recommended replacement for lv_obj_bind_flag_if_*: bind a flag from a boolean
 * subject by passing a dedicated per-flag setter directly to lv_obj_bind_bool. */
void test_observer_obj_bind_bool_flag(void)
{
    lv_obj_t * obj = lv_obj_create(lv_screen_active());

    lv_subject_t * subject = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int(subject, 0);

    lv_observer_t * observer = lv_obj_bind_bool(obj, subject, lv_obj_set_hidden);
    TEST_ASSERT_NOT_NULL(observer);

    /*Applied immediately: 0 -> not hidden*/
    TEST_ASSERT_EQUAL(false, lv_obj_is_hidden(obj));

    /*Any non-zero value -> hidden*/
    lv_subject_set_int(subject, 1);
    TEST_ASSERT_EQUAL(true, lv_obj_is_hidden(obj));

    lv_subject_set_int(subject, 5);
    TEST_ASSERT_EQUAL(true, lv_obj_is_hidden(obj));

    lv_subject_set_int(subject, 0);
    TEST_ASSERT_EQUAL(false, lv_obj_is_hidden(obj));

    /*After removing the observer the widget is no longer updated*/
    lv_observer_delete(observer);
    lv_subject_set_int(subject, 1);
    TEST_ASSERT_EQUAL(false, lv_obj_is_hidden(obj));

    lv_obj_delete(obj);
}

void test_observer_obj_bind_bool_invalid(void)
{
    lv_obj_t * obj = lv_obj_create(lv_screen_active());

    lv_subject_t * subject = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int(subject, 0);

    TEST_ASSERT_EQUAL_PTR(NULL, lv_obj_bind_bool(NULL, subject, lv_obj_set_hidden));
    TEST_ASSERT_EQUAL_PTR(NULL, lv_obj_bind_bool(obj, NULL, lv_obj_set_hidden));
    TEST_ASSERT_EQUAL_PTR(NULL, lv_obj_bind_bool(obj, subject, NULL));

    lv_obj_delete(obj);
}

/* The remaining lv_obj_bind_<type>() functions just forward the subject's value
 * to the setter; these tests confirm the value arrives on subscribe and update. */
static int32_t captured_int;
static void capture_int_cb(lv_obj_t * obj, int32_t v)
{
    LV_UNUSED(obj);
    captured_int = v;
}

void test_observer_obj_bind_int(void)
{
    lv_obj_t * obj = lv_obj_create(lv_screen_active());

    lv_subject_t * subject = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int(subject, 10);

    TEST_ASSERT_NOT_NULL(lv_obj_bind_int(obj, subject, capture_int_cb));
    TEST_ASSERT_EQUAL_INT(10, captured_int);
    lv_subject_set_int(subject, 42);
    TEST_ASSERT_EQUAL_INT(42, captured_int);

    lv_obj_delete(obj);
}

#if LV_USE_FLOAT
static float captured_float;
static void capture_float_cb(lv_obj_t * obj, float v)
{
    LV_UNUSED(obj);
    captured_float = v;
}

void test_observer_obj_bind_float(void)
{
    lv_obj_t * obj = lv_obj_create(lv_screen_active());

    lv_subject_t * subject = subject_create(LV_SUBJECT_TYPE_FLOAT);
    lv_subject_set_float(subject, 1.5f);

    TEST_ASSERT_NOT_NULL(lv_obj_bind_float(obj, subject, capture_float_cb));
    TEST_ASSERT_EQUAL_FLOAT(1.5f, captured_float);
    lv_subject_set_float(subject, 2.5f);
    TEST_ASSERT_EQUAL_FLOAT(2.5f, captured_float);

    lv_obj_delete(obj);
}
#endif

static const char * captured_string;
static void capture_string_cb(lv_obj_t * obj, const char * v)
{
    LV_UNUSED(obj);
    captured_string = v;
}

void test_observer_obj_bind_string(void)
{
    lv_obj_t * obj = lv_obj_create(lv_screen_active());
    static char buf[32];

    lv_subject_t * subject = subject_create(LV_SUBJECT_TYPE_STRING);
    lv_subject_set_string_buffer_static(subject, buf, sizeof(buf));
    lv_subject_copy_string(subject, "hello");

    TEST_ASSERT_NOT_NULL(lv_obj_bind_string(obj, subject, capture_string_cb));
    TEST_ASSERT_EQUAL_STRING("hello", captured_string);
    lv_subject_copy_string(subject, "world");
    TEST_ASSERT_EQUAL_STRING("world", captured_string);

    lv_obj_delete(obj);
}

static lv_color_t captured_color;
static void capture_color_cb(lv_obj_t * obj, lv_color_t v)
{
    LV_UNUSED(obj);
    captured_color = v;
}

void test_observer_obj_bind_color(void)
{
    lv_obj_t * obj = lv_obj_create(lv_screen_active());

    lv_subject_t * subject = subject_create(LV_SUBJECT_TYPE_COLOR);
    lv_subject_set_color(subject, lv_color_hex(0x123456));

    TEST_ASSERT_NOT_NULL(lv_obj_bind_color(obj, subject, capture_color_cb));
    TEST_ASSERT_TRUE(lv_color_eq(lv_color_hex(0x123456), captured_color));
    lv_subject_set_color(subject, lv_color_hex(0xabcdef));
    TEST_ASSERT_TRUE(lv_color_eq(lv_color_hex(0xabcdef), captured_color));

    lv_obj_delete(obj);
}

static const void * captured_pointer;
static void capture_pointer_cb(lv_obj_t * obj, const void * v)
{
    LV_UNUSED(obj);
    captured_pointer = v;
}

void test_observer_obj_bind_pointer(void)
{
    lv_obj_t * obj = lv_obj_create(lv_screen_active());
    static const char * a = "a";
    static const char * b = "b";

    lv_subject_t * subject = subject_create(LV_SUBJECT_TYPE_POINTER);
    lv_subject_set_pointer(subject, (void *)a);

    TEST_ASSERT_NOT_NULL(lv_obj_bind_pointer(obj, subject, capture_pointer_cb));
    TEST_ASSERT_EQUAL_PTR(a, captured_pointer);
    lv_subject_set_pointer(subject, (void *)b);
    TEST_ASSERT_EQUAL_PTR(b, captured_pointer);

    lv_obj_delete(obj);
}

/* Recommended replacement for lv_obj_bind_state_if_*: toggle a state from a
 * custom observer added with lv_subject_add_observer_obj. */
static void set_disabled_observer_cb(lv_observer_t * observer, lv_subject_t * subject)
{
    lv_obj_set_state(lv_observer_get_target_obj(observer), LV_STATE_DISABLED, lv_subject_get_int(subject));
}

void test_observer_obj_state_via_observer(void)
{
    lv_obj_t * obj = lv_obj_create(lv_screen_active());

    lv_subject_t * subject = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int(subject, 0);

    lv_observer_t * observer =
        lv_subject_add_observer_obj(subject, set_disabled_observer_cb, obj, NULL);
    TEST_ASSERT_NOT_NULL(observer);

    /*Applied immediately*/
    TEST_ASSERT_EQUAL(false, lv_obj_has_state(obj, LV_STATE_DISABLED));

    lv_subject_set_int(subject, 1);
    TEST_ASSERT_EQUAL(true, lv_obj_has_state(obj, LV_STATE_DISABLED));

    lv_subject_set_int(subject, 0);
    TEST_ASSERT_EQUAL(false, lv_obj_has_state(obj, LV_STATE_DISABLED));

    lv_obj_delete(obj);
}

void test_observer_button_checked(void)
{
    lv_obj_t * obj = lv_button_create(lv_screen_active());
    lv_obj_set_size(obj, 100, 100);
    lv_obj_set_checkable(obj, true);
    lv_obj_update_layout(obj);

    /*Can bind only to int*/
    lv_subject_t * subject_wrong = subject_create(LV_SUBJECT_TYPE_POINTER);
    lv_observer_t * observer = lv_obj_bind_checked(obj, subject_wrong);
    TEST_ASSERT_EQUAL_PTR(NULL, observer);

    lv_subject_t * subject = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int(subject, 1);
    lv_obj_bind_checked(obj, subject);

    TEST_ASSERT_EQUAL(true, lv_obj_has_state(obj, LV_STATE_CHECKED));

    lv_subject_set_int(subject, 0);
    TEST_ASSERT_EQUAL(false, lv_obj_has_state(obj, LV_STATE_CHECKED));

    lv_test_mouse_click_at(10, 10);
    TEST_ASSERT_EQUAL(true, lv_obj_has_state(obj, LV_STATE_CHECKED));
    TEST_ASSERT_EQUAL(1, lv_subject_get_int(subject));
}

void test_observer_label_text_normal(void)
{
    lv_obj_t * obj = lv_label_create(lv_screen_active());

    lv_observer_t * observer;

    /*Cannot bind color*/
    lv_subject_t * subject_color = subject_create(LV_SUBJECT_TYPE_COLOR);
    observer = lv_label_bind_text(obj, subject_color, NULL);
    TEST_ASSERT_EQUAL_PTR(NULL, observer);

    /*Bind it with "%d" if NULL is passed*/
    lv_subject_t * subject_int = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int(subject_int, 10);
    observer = lv_label_bind_text(obj, subject_int, NULL);
    TEST_ASSERT_NOT_NULL(observer);
    TEST_ASSERT_EQUAL_STRING("10", lv_label_get_text(obj));

    /*Bind it with "%0.1f" if NULL is passed*/
    lv_subject_t * subject_float = subject_create(LV_SUBJECT_TYPE_FLOAT);
    lv_subject_set_float(subject_float, 10.5);
    observer = lv_label_bind_text(obj, subject_float, NULL);
    TEST_ASSERT_NOT_NULL(observer);
    TEST_ASSERT_EQUAL_STRING("10.5", lv_label_get_text(obj));

    /*An explicit format string is used as-is*/
    lv_subject_set_float(subject_float, 81.5);
    observer = lv_label_bind_text(obj, subject_float, "Value: %0.2f");
    TEST_ASSERT_NOT_NULL(observer);
    TEST_ASSERT_EQUAL_STRING("Value: 81.50", lv_label_get_text(obj));

    /*Bind to string*/
    static char buf[32];
    lv_subject_t * subject_string = subject_create(LV_SUBJECT_TYPE_STRING);
    lv_subject_set_string_buffer_static(subject_string, buf, sizeof(buf));
    lv_subject_copy_string(subject_string, "hello");
    lv_label_bind_text(obj, subject_string, NULL);
    TEST_ASSERT_EQUAL_STRING("hello", lv_label_get_text(obj));

    lv_subject_copy_string(subject_string, "world");
    TEST_ASSERT_EQUAL_STRING("world", lv_label_get_text(obj));

    /*Remove the label from the subject*/
    lv_obj_remove_from_subject(obj, subject_string);
    lv_subject_copy_string(subject_string, "nothing");
    TEST_ASSERT_EQUAL_STRING("world", lv_label_get_text(obj));

    /*Bind to pointer*/
    lv_subject_t * subject_pointer = subject_create(LV_SUBJECT_TYPE_POINTER);
    lv_subject_set_pointer(subject_pointer, "HELLO");
    lv_label_bind_text(obj, subject_pointer, NULL);
    TEST_ASSERT_EQUAL_STRING("HELLO", lv_label_get_text(obj));

    lv_subject_set_pointer(subject_pointer, "WORLD");
    TEST_ASSERT_EQUAL_STRING("WORLD", lv_label_get_text(obj));

    /*Remove the label from the subject*/
    lv_obj_remove_from_subject(obj, subject_pointer);
    lv_subject_copy_string(subject_pointer, "NOTHING");
    TEST_ASSERT_EQUAL_STRING("WORLD", lv_label_get_text(obj));
}

void test_observer_label_text_formatted(void)
{
    lv_obj_t * obj = lv_label_create(lv_screen_active());

    lv_observer_t * observer;

    /*Cannot bind color*/
    lv_subject_t * subject_color = subject_create(LV_SUBJECT_TYPE_COLOR);
    observer = lv_label_bind_text(obj, subject_color, NULL);
    TEST_ASSERT_EQUAL_PTR(NULL, observer);

    /*Bind to int*/
    lv_subject_t * subject_int = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int(subject_int, 10);
    lv_label_bind_text(obj, subject_int, "value: %d");
    TEST_ASSERT_EQUAL_STRING("value: 10", lv_label_get_text(obj));

    lv_subject_set_int(subject_int, -20);
    TEST_ASSERT_EQUAL_STRING("value: -20", lv_label_get_text(obj));

    /*Remove the label from the subject*/
    lv_obj_remove_from_subject(obj, subject_int);
    lv_subject_set_int(subject_int, 100);
    TEST_ASSERT_EQUAL_STRING("value: -20", lv_label_get_text(obj));

    /*Bind to string*/
    static char buf[32];
    lv_subject_t * subject_string = subject_create(LV_SUBJECT_TYPE_STRING);
    lv_subject_set_string_buffer_static(subject_string, buf, sizeof(buf));
    lv_subject_copy_string(subject_string, "hello");
    lv_label_bind_text(obj, subject_string, "text: %s");
    TEST_ASSERT_EQUAL_STRING("text: hello", lv_label_get_text(obj));

    lv_subject_copy_string(subject_string, "world");
    TEST_ASSERT_EQUAL_STRING("text: world", lv_label_get_text(obj));

    /*Remove the label from the subject*/
    lv_obj_remove_from_subject(obj, subject_string);
    lv_subject_copy_string(subject_string, "nothing");
    TEST_ASSERT_EQUAL_STRING("text: world", lv_label_get_text(obj));

    /*Bind to pointer*/
    lv_subject_t * subject_pointer = subject_create(LV_SUBJECT_TYPE_POINTER);
    lv_subject_set_pointer(subject_pointer, "HELLO");
    lv_label_bind_text(obj, subject_pointer, "pointer: %s");
    TEST_ASSERT_EQUAL_STRING("pointer: HELLO", lv_label_get_text(obj));

    lv_subject_set_pointer(subject_pointer, "WORLD");
    TEST_ASSERT_EQUAL_STRING("pointer: WORLD", lv_label_get_text(obj));

    /*Remove the label from the subject*/
    lv_obj_remove_from_subject(obj, subject_pointer);
    lv_subject_copy_string(subject_pointer, "NOTHING");
    TEST_ASSERT_EQUAL_STRING("pointer: WORLD", lv_label_get_text(obj));
}

void test_observer_arc_value(void)
{
    lv_obj_t * obj = lv_arc_create(lv_screen_active());

    lv_subject_t * subject = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int(subject, 30);
    lv_arc_bind_value(obj, subject);

    TEST_ASSERT_EQUAL(30, lv_arc_get_value(obj));

    lv_subject_set_int(subject, 40);
    TEST_ASSERT_EQUAL(40, lv_arc_get_value(obj));

    lv_obj_update_layout(obj);
    lv_test_mouse_release();
    lv_test_wait(100);

    lv_test_mouse_move_to(65, 10);
    lv_test_mouse_press();
    lv_test_wait(100);
    lv_test_mouse_release();

    TEST_ASSERT_EQUAL(50, lv_arc_get_value(obj));
    TEST_ASSERT_EQUAL(50, lv_subject_get_int(subject));
}

void test_observer_slider_value(void)
{
    lv_obj_t * obj = lv_slider_create(lv_screen_active());

    lv_subject_t * subject = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int(subject, 30);
    lv_slider_bind_value(obj, subject);

    TEST_ASSERT_EQUAL(30, lv_slider_get_value(obj));

    lv_subject_set_int(subject, 40);
    TEST_ASSERT_EQUAL(40, lv_slider_get_value(obj));

    lv_obj_update_layout(obj);
    lv_test_mouse_release();
    lv_test_wait(100);

    lv_test_mouse_move_to(65, 10);
    lv_test_mouse_press();
    lv_test_wait(100);

    lv_test_mouse_move_to(75, 10);
    lv_test_mouse_press();
    lv_test_wait(100);
    lv_test_mouse_release();

    TEST_ASSERT_EQUAL(29, lv_slider_get_value(obj));
    TEST_ASSERT_EQUAL(29, lv_subject_get_int(subject));
}

void test_observer_roller_value(void)
{
    lv_obj_t * obj = lv_roller_create(lv_screen_active());

    lv_subject_t * subject = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int(subject, 1);
    lv_roller_bind_value(obj, subject);

    TEST_ASSERT_EQUAL(1, lv_roller_get_selected(obj));

    lv_subject_set_int(subject, 2);
    TEST_ASSERT_EQUAL(2, lv_roller_get_selected(obj));

    lv_obj_update_layout(obj);
    lv_test_mouse_click_at(30, 10);

    TEST_ASSERT_EQUAL(1, lv_roller_get_selected(obj));
    TEST_ASSERT_EQUAL(1, lv_subject_get_int(subject));
}

void test_observer_dropdown_value(void)
{
    lv_obj_t * obj = lv_dropdown_create(lv_screen_active());

    lv_subject_t * subject = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int(subject, 1);
    lv_dropdown_bind_value(obj, subject);

    TEST_ASSERT_EQUAL(1, lv_dropdown_get_selected(obj));

    lv_subject_set_int(subject, 2);
    TEST_ASSERT_EQUAL(2, lv_dropdown_get_selected(obj));

    lv_obj_update_layout(obj);
    lv_test_mouse_click_at(30, 10);
    lv_test_mouse_click_at(30, 60);

    TEST_ASSERT_EQUAL(0, lv_dropdown_get_selected(obj));
    TEST_ASSERT_EQUAL(0, lv_subject_get_int(subject));
}

void test_observer_scale_line_needle_value(void)
{
    lv_obj_t * obj = lv_scale_create(lv_screen_active());
    lv_scale_set_mode(obj, LV_SCALE_MODE_ROUND_INNER);
    lv_scale_t * scale = (lv_scale_t *)obj;
    lv_obj_t * needle = lv_line_create(obj);

    lv_subject_t * subject = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int(subject, 30);
    lv_scale_bind_line_needle_value(obj, needle, 50, subject);

    lv_scale_needle_t * scale_needle = lv_array_at(&scale->needles, 0);

    TEST_ASSERT_EQUAL(30, scale_needle->value);
    TEST_ASSERT_EQUAL(50, scale_needle->length);

    lv_subject_set_int(subject, 40);
    TEST_ASSERT_EQUAL(40, scale_needle->value);
}

void test_observer_scale_image_needle_value(void)
{
    lv_obj_t * obj = lv_scale_create(lv_screen_active());
    lv_scale_set_mode(obj, LV_SCALE_MODE_ROUND_INNER);
    lv_scale_t * scale = (lv_scale_t *)obj;
    lv_obj_t * needle = lv_image_create(obj);

    lv_subject_t * subject = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int(subject, 30);
    lv_scale_bind_image_needle_value(obj, needle, subject);

    lv_scale_needle_t * scale_needle = lv_array_at(&scale->needles, 0);

    TEST_ASSERT_EQUAL(30, scale_needle->value);

    lv_subject_set_int(subject, 40);
    TEST_ASSERT_EQUAL(40, scale_needle->value);
}

void test_observer_set_user_data(void)
{

    lv_subject_t * subject = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int(subject, 5);

    lv_observer_t * observer =
        lv_subject_add_observer(subject, observer_basic, NULL);
    TEST_ASSERT_NOT_NULL(observer);

    /* Initially NULL */
    TEST_ASSERT_EQUAL_PTR(NULL, lv_observer_get_user_data(observer));

    static int32_t a;
    static int32_t b;
    lv_observer_set_user_data(observer, &a);
    TEST_ASSERT_EQUAL_PTR(&a, lv_observer_get_user_data(observer));

    lv_observer_set_user_data(observer, &b);
    TEST_ASSERT_EQUAL_PTR(&b, lv_observer_get_user_data(observer));

    lv_observer_delete(observer);
}

void test_observer_set_user_data_user_owned(void)
{

    lv_subject_t * subject = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int(subject, 5);

    /* Record the baseline before any allocation so removal returns to it */
    uint32_t mem = lv_test_get_free_mem();

    lv_observer_t * observer =
        lv_subject_add_observer(subject, observer_basic, NULL);
    TEST_ASSERT_NOT_NULL(observer);

    /* The observer must not free user-owned data on removal. */
    void * data = lv_malloc(64);
    TEST_ASSERT_NOT_NULL(data);
    lv_observer_set_user_data(observer, data);
    TEST_ASSERT_EQUAL_PTR(data, lv_observer_get_user_data(observer));

    lv_observer_delete(observer);
    lv_free(data);
    TEST_ASSERT_MEM_LEAK_LESS_THAN(mem, 32);
}

void test_observer_set_user_data_replaces_internal_data(void)
{

    lv_subject_t * subject = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int(subject, 5);

    /* Record the baseline before any allocation so removal returns to it */
    uint32_t mem = lv_test_get_free_mem();

    lv_observer_t * observer =
        lv_subject_add_observer(subject, observer_basic, NULL);
    TEST_ASSERT_NOT_NULL(observer);

    /* Simulate an observer whose data is owned by an internal binding. */
    void * old_data = lv_malloc(64);
    TEST_ASSERT_NOT_NULL(old_data);
    observer->user_data = old_data;
    observer->auto_free_user_data = 1;

    void * new_data = lv_malloc(64);
    TEST_ASSERT_NOT_NULL(new_data);
    uint32_t mem_with_both_data = lv_test_get_free_mem();
    lv_observer_set_user_data(observer, new_data);
    TEST_ASSERT_EQUAL_PTR(new_data, lv_observer_get_user_data(observer));
    TEST_ASSERT_FALSE(observer->auto_free_user_data);
    LV_UNUSED(mem_with_both_data);
    LV_HEAP_CHECK(TEST_ASSERT_GREATER_THAN_UINT32(mem_with_both_data, lv_test_get_free_mem()));

    lv_observer_delete(observer);
    lv_free(new_data);
    TEST_ASSERT_MEM_LEAK_LESS_THAN(mem, 32);
}

void test_observer_set_user_data_same_internal_data(void)
{

    lv_subject_t * subject = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int(subject, 5);

    uint32_t mem = lv_test_get_free_mem();

    lv_observer_t * observer =
        lv_subject_add_observer(subject, observer_basic, NULL);
    TEST_ASSERT_NOT_NULL(observer);

    void * data = lv_malloc(64);
    TEST_ASSERT_NOT_NULL(data);
    observer->user_data = data;
    observer->auto_free_user_data = 1;

    uint32_t mem_with_data = lv_test_get_free_mem();
    lv_observer_set_user_data(observer, data);
    TEST_ASSERT_EQUAL_PTR(data, lv_observer_get_user_data(observer));
    TEST_ASSERT_FALSE(observer->auto_free_user_data);
    TEST_ASSERT_EQUAL_UINT32(mem_with_data, lv_test_get_free_mem());

    lv_observer_delete(observer);
    lv_free(data);
    TEST_ASSERT_MEM_LEAK_LESS_THAN(mem, 32);
}

void test_observer_delete(void)
{
    uint32_t mem = lv_test_get_free_mem();
    uint32_t i;
    for(i = 0; i < 64; i++) {
        lv_obj_t * obj1 = lv_slider_create(lv_screen_active());
        lv_obj_t * obj2 = lv_slider_create(lv_screen_active());

        lv_subject_t * subject = lv_subject_create(LV_SUBJECT_TYPE_INT);
        lv_subject_set_int(subject, 30);
        lv_slider_bind_value(obj1, subject);
        lv_slider_bind_value(obj2, subject);
        lv_subject_add_observer(subject, observer_int, NULL);
        lv_obj_delete(obj1);
        lv_subject_delete(subject);
        lv_obj_delete(obj2);
    }

    TEST_ASSERT_MEM_LEAK_LESS_THAN(mem, 32);
}

/*=====================================================================
 * Mappers, autowired dependencies, and eager/lazy evaluation
 *====================================================================*/

/* Subjects the mappers below read. Set per test. */
static lv_subject_t * dep_a;
static lv_subject_t * dep_b;
static lv_subject_t * dep_switch;

static uint32_t mapper_runs;
static uint32_t mapper_saw_inconsistent;



/* The same mapper reused by two Subjects, each with its own limits in user_data.
 * This is why the mapper receives the Subject. */
typedef struct {
    int32_t min;
    int32_t max;
} range_t;





/*--------------------------------------------------------
 * A mapper that overwrites the slot: a derived value
 *-------------------------------------------------------*/

/* Reads a float Subject as an int, for the cross-type dependency test. */
static bool int_of_dep_a_mapper(lv_subject_t * subject, void * user_data, int32_t * value)
{
    LV_UNUSED(user_data);
    LV_UNUSED(subject);
    int32_t next = lv_subject_get_int(dep_a);
    if(next == *value) return false;
    *value = next;
    return true;
}

static bool sum_mapper(lv_subject_t * subject, void * user_data, int32_t * value)
{
    LV_UNUSED(user_data);
    LV_UNUSED(subject);
    mapper_runs++;
    int32_t next = lv_subject_get_int(dep_a) + lv_subject_get_int(dep_b);
    if(next == *value) return false;
    *value = next;
    return true;
}

void test_subject_mapper_autowires_dependencies(void)
{
    dep_a = subject_create(LV_SUBJECT_TYPE_INT);
    dep_b = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int(dep_a, 2);
    lv_subject_set_int(dep_b, 3);

    lv_subject_t * sum = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int_mapper(sum, sum_mapper, NULL);

    TEST_ASSERT_EQUAL(5, lv_subject_get_int(sum));
    TEST_ASSERT_EQUAL(2, lv_subject_get_dependency_count(sum));
    TEST_ASSERT_EQUAL(dep_a, lv_subject_get_dependency(sum, 0));
    TEST_ASSERT_EQUAL(dep_b, lv_subject_get_dependency(sum, 1));
    /* And the reverse edges exist, which is what dirty marking walks. */
    TEST_ASSERT_EQUAL(1, lv_ll_get_len(&dep_a->dependents));
    TEST_ASSERT_EQUAL(1, lv_ll_get_len(&dep_b->dependents));

    lv_subject_set_int(dep_a, 10);
    TEST_ASSERT_EQUAL(13, lv_subject_get_int(sum));
}

void test_subject_write_to_derived_is_discarded(void)
{
    dep_a = subject_create(LV_SUBJECT_TYPE_INT);
    dep_b = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int(dep_a, 4);
    lv_subject_set_int(dep_b, 6);

    lv_subject_t * sum = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int_mapper(sum, sum_mapper, NULL);
    TEST_ASSERT_EQUAL(10, lv_subject_get_int(sum));

    /* The mapper ignores the seeded slot, so the write silently has no effect. */
    lv_subject_set_int(sum, 999);
    TEST_ASSERT_EQUAL(10, lv_subject_get_int(sum));
}

/* Reads a different Subject depending on a switch, so the dependency set changes
 * between evaluations. */
static bool switched_mapper(lv_subject_t * subject, void * user_data, int32_t * value)
{
    LV_UNUSED(user_data);
    LV_UNUSED(subject);
    *value = lv_subject_get_int(dep_switch) ? lv_subject_get_int(dep_b) : lv_subject_get_int(dep_a);
    return true;
}

void test_subject_mapper_dependencies_rebuilt_each_run(void)
{
    dep_switch = subject_create(LV_SUBJECT_TYPE_INT);
    dep_a = subject_create(LV_SUBJECT_TYPE_INT);
    dep_b = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int(dep_a, 1);
    lv_subject_set_int(dep_b, 2);
    lv_subject_set_int(dep_switch, 0);

    lv_subject_t * picked = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int_mapper(picked, switched_mapper, NULL);

    TEST_ASSERT_EQUAL(1, lv_subject_get_int(picked));
    /* dep_switch and dep_a were read; dep_b was not. */
    TEST_ASSERT_EQUAL(2, lv_subject_get_dependency_count(picked));
    TEST_ASSERT_EQUAL(0, lv_ll_get_len(&dep_b->dependents));

    lv_subject_set_int(dep_switch, 1);
    TEST_ASSERT_EQUAL(2, lv_subject_get_int(picked));
    /* Now dep_b is a dependency and dep_a is not. */
    TEST_ASSERT_EQUAL(2, lv_subject_get_dependency_count(picked));
    TEST_ASSERT_EQUAL(1, lv_ll_get_len(&dep_b->dependents));
    TEST_ASSERT_EQUAL(0, lv_ll_get_len(&dep_a->dependents));

    /* Changing the Subject that is no longer read must not touch `picked`. */
    lv_subject_set_int(dep_a, 77);
    TEST_ASSERT_FALSE(lv_subject_is_dirty(picked));
    TEST_ASSERT_EQUAL(2, lv_subject_get_int(picked));
}

/*--------------------------------------------------------
 * Lazy versus eager
 *-------------------------------------------------------*/

void test_subject_lazy_is_only_marked_dirty(void)
{
    dep_a = subject_create(LV_SUBJECT_TYPE_INT);
    dep_b = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int(dep_a, 1);
    lv_subject_set_int(dep_b, 1);

    lv_subject_t * sum = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int_mapper(sum, sum_mapper, NULL);
    TEST_ASSERT_EQUAL(LV_SUBJECT_MODE_LAZY, lv_subject_get_mode(sum));
    TEST_ASSERT_FALSE(lv_subject_is_eager(sum));

    mapper_runs = 0;
    lv_subject_set_int(dep_a, 5);
    lv_subject_set_int(dep_a, 6);
    lv_subject_set_int(dep_a, 7);

    /* Three writes, no evaluation: the mapper has not run at all. */
    TEST_ASSERT_EQUAL(0, mapper_runs);
    TEST_ASSERT_TRUE(lv_subject_is_dirty(sum));

    /* Reading collapses them into one evaluation. */
    TEST_ASSERT_EQUAL(8, lv_subject_get_int(sum));
    TEST_ASSERT_EQUAL(1, mapper_runs);
    TEST_ASSERT_FALSE(lv_subject_is_dirty(sum));
}

void test_subject_eager_evaluates_inside_set(void)
{
    dep_a = subject_create(LV_SUBJECT_TYPE_INT);
    dep_b = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int(dep_a, 1);
    lv_subject_set_int(dep_b, 1);

    lv_subject_t * sum = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int_mapper(sum, sum_mapper, NULL);
    lv_subject_set_mode(sum, LV_SUBJECT_MODE_EAGER);
    TEST_ASSERT_TRUE(lv_subject_is_eager(sum));

    mapper_runs = 0;
    lv_subject_set_int(dep_a, 5);
    /* Already up to date without anyone reading it. */
    TEST_ASSERT_EQUAL(1, mapper_runs);
    TEST_ASSERT_FALSE(lv_subject_is_dirty(sum));
    TEST_ASSERT_EQUAL(6, sum->value.num);
}

void test_subject_lazy_promoted_by_eager_observer(void)
{
    dep_a = subject_create(LV_SUBJECT_TYPE_INT);
    dep_b = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int(dep_a, 1);
    lv_subject_set_int(dep_b, 1);

    lv_subject_t * sum = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int_mapper(sum, sum_mapper, NULL);
    TEST_ASSERT_FALSE(lv_subject_is_eager(sum));

    observer_called = 0;
    /* LV_OBSERVER_MODE_IMMEDIATE is the default, so this promotes `sum`. */
    lv_observer_t * observer = lv_subject_add_observer(sum, observer_basic, NULL);
    TEST_ASSERT_EQUAL(LV_OBSERVER_MODE_IMMEDIATE, lv_observer_get_mode(observer));
    TEST_ASSERT_TRUE(lv_subject_is_eager(sum));
    TEST_ASSERT_EQUAL(1, observer_called);  /* initial notification */

    mapper_runs = 0;
    lv_subject_set_int(dep_a, 9);
    TEST_ASSERT_EQUAL(1, mapper_runs);
    TEST_ASSERT_EQUAL(2, observer_called);

    /* Removing the last eager Observer lets it fall back to its declared mode. */
    lv_observer_delete(observer);
    TEST_ASSERT_FALSE(lv_subject_is_eager(sum));
}

void test_subject_lazy_observer_does_not_promote(void)
{
    dep_a = subject_create(LV_SUBJECT_TYPE_INT);
    dep_b = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int(dep_a, 1);
    lv_subject_set_int(dep_b, 1);

    lv_subject_t * sum = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int_mapper(sum, sum_mapper, NULL);

    lv_observer_t * observer = lv_subject_add_observer(sum, observer_basic, NULL);
    lv_observer_set_mode(observer, LV_OBSERVER_MODE_BATCHED);
    TEST_ASSERT_FALSE(lv_subject_is_eager(sum));

    observer_called = 0;
    mapper_runs = 0;
    lv_subject_set_int(dep_a, 4);

    /* Not evaluated and not notified inside set(). */
    TEST_ASSERT_EQUAL(0, mapper_runs);
    TEST_ASSERT_EQUAL(0, observer_called);
    TEST_ASSERT_TRUE(lv_subject_is_dirty(sum));

    /* The flush picks it up because it has an Observer waiting. */
    lv_subject_flush();
    TEST_ASSERT_EQUAL(1, mapper_runs);
    TEST_ASSERT_EQUAL(1, observer_called);
    TEST_ASSERT_FALSE(lv_subject_is_dirty(sum));
}

void test_subject_lazy_observer_flushed_by_frame(void)
{
    dep_a = subject_create(LV_SUBJECT_TYPE_INT);
    dep_b = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int(dep_a, 1);
    lv_subject_set_int(dep_b, 1);

    lv_subject_t * sum = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int_mapper(sum, sum_mapper, NULL);
    lv_observer_t * observer = lv_subject_add_observer(sum, observer_basic, NULL);
    lv_observer_set_mode(observer, LV_OBSERVER_MODE_BATCHED);

    observer_called = 0;
    mapper_runs = 0;
    lv_subject_set_int(dep_a, 4);
    lv_subject_set_int(dep_a, 5);
    TEST_ASSERT_EQUAL(0, observer_called);

    /* LVGL flushes once per lv_timer_handler() pass, so no explicit call is needed. */
    lv_test_wait(50);
    TEST_ASSERT_EQUAL(1, mapper_runs);  /* both writes coalesced */
    TEST_ASSERT_EQUAL(1, observer_called);
}

void test_subject_switching_observer_to_eager_flushes_immediately(void)
{
    dep_a = subject_create(LV_SUBJECT_TYPE_INT);
    dep_b = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int(dep_a, 1);
    lv_subject_set_int(dep_b, 1);

    lv_subject_t * sum = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int_mapper(sum, sum_mapper, NULL);
    lv_observer_t * observer = lv_subject_add_observer(sum, observer_basic, NULL);
    lv_observer_set_mode(observer, LV_OBSERVER_MODE_BATCHED);

    lv_subject_set_int(dep_a, 8);
    TEST_ASSERT_TRUE(lv_subject_is_dirty(sum));

    observer_called = 0;
    lv_observer_set_mode(observer, LV_OBSERVER_MODE_IMMEDIATE);
    /* The pending work was due the moment the Observer became eager. */
    TEST_ASSERT_FALSE(lv_subject_is_dirty(sum));
    TEST_ASSERT_EQUAL(1, observer_called);
    TEST_ASSERT_EQUAL(9, lv_subject_get_int(sum));
}

void test_subject_dirty_without_observers_stays_dirty_over_flush(void)
{
    dep_a = subject_create(LV_SUBJECT_TYPE_INT);
    dep_b = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int(dep_a, 1);
    lv_subject_set_int(dep_b, 1);

    lv_subject_t * sum = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int_mapper(sum, sum_mapper, NULL);
    TEST_ASSERT_EQUAL(2, lv_subject_get_int(sum));

    mapper_runs = 0;
    lv_subject_set_int(dep_a, 5);
    lv_subject_flush();

    /* Lazy, unobserved and unread: there is nothing that needs the value yet. */
    TEST_ASSERT_EQUAL(0, mapper_runs);
    TEST_ASSERT_TRUE(lv_subject_is_dirty(sum));
}

/*--------------------------------------------------------
 * Glitch-free evaluation
 *-------------------------------------------------------*/

/* A -> B, A -> C, B + C -> D. D must evaluate once and never see a fresh B next to
 * a stale C. */
static lv_subject_t * diamond_b;
static lv_subject_t * diamond_c;

static bool double_mapper(lv_subject_t * subject, void * user_data, int32_t * value)
{
    LV_UNUSED(user_data);
    LV_UNUSED(subject);
    *value = lv_subject_get_int(dep_a) * 2;
    return true;
}

static bool triple_mapper(lv_subject_t * subject, void * user_data, int32_t * value)
{
    LV_UNUSED(user_data);
    LV_UNUSED(subject);
    *value = lv_subject_get_int(dep_a) * 3;
    return true;
}

static bool diamond_sink_mapper(lv_subject_t * subject, void * user_data, int32_t * value)
{
    LV_UNUSED(user_data);
    LV_UNUSED(subject);
    mapper_runs++;
    int32_t b = lv_subject_get_int(diamond_b);
    int32_t c = lv_subject_get_int(diamond_c);
    /* b == 2A and c == 3A, so 3b must equal 2c for a consistent pair. */
    if(3 * b != 2 * c) mapper_saw_inconsistent++;
    *value = b + c;
    return true;
}

void test_subject_diamond_evaluates_sink_once(void)
{
    dep_a = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int(dep_a, 1);

    diamond_b = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int_mapper(diamond_b, double_mapper, NULL);
    diamond_c = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int_mapper(diamond_c, triple_mapper, NULL);

    lv_subject_t * sink = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int_mapper(sink, diamond_sink_mapper, NULL);
    lv_subject_set_mode(sink, LV_SUBJECT_MODE_EAGER);
    TEST_ASSERT_EQUAL(5, lv_subject_get_int(sink));

    mapper_runs = 0;
    mapper_saw_inconsistent = 0;
    lv_subject_set_int(dep_a, 10);

    TEST_ASSERT_EQUAL(1, mapper_runs);            /* not twice */
    TEST_ASSERT_EQUAL(0, mapper_saw_inconsistent); /* and never a mixed pair */
    TEST_ASSERT_EQUAL(50, lv_subject_get_int(sink));
}

/* a -> l1 -> l2 -> l3, so a change has to travel three levels. Each level counts its
 * own evaluations, so a re-evaluation storm would show up. */
static lv_subject_t * chain_l1;
static lv_subject_t * chain_l2;
static uint32_t l1_runs;
static uint32_t l2_runs;
static uint32_t l3_runs;

static bool chain_l1_mapper(lv_subject_t * subject, void * user_data, int32_t * value)
{
    LV_UNUSED(user_data);
    LV_UNUSED(subject);
    l1_runs++;
    *value = lv_subject_get_int(dep_a) * 2;
    return true;
}

static bool chain_l2_mapper(lv_subject_t * subject, void * user_data, int32_t * value)
{
    LV_UNUSED(user_data);
    LV_UNUSED(subject);
    l2_runs++;
    *value = lv_subject_get_int(chain_l1) + 1;
    return true;
}

static bool chain_l3_mapper(lv_subject_t * subject, void * user_data, int32_t * value)
{
    LV_UNUSED(user_data);
    LV_UNUSED(subject);
    l3_runs++;
    *value = lv_subject_get_int(chain_l2) * 10;
    return true;
}

void test_subject_deep_chain_propagates(void)
{
    dep_a = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int(dep_a, 1);

    chain_l1 = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int_mapper(chain_l1, chain_l1_mapper, NULL);
    chain_l2 = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int_mapper(chain_l2, chain_l2_mapper, NULL);

    lv_subject_t * l3 = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int_mapper(l3, chain_l3_mapper, NULL);
    lv_subject_set_mode(l3, LV_SUBJECT_MODE_EAGER);

    /* a=1 -> l1=2 -> l2=3 -> l3=30 */
    TEST_ASSERT_EQUAL(30, lv_subject_get_int(l3));

    l1_runs = 0;
    l2_runs = 0;
    l3_runs = 0;
    lv_subject_set_int(dep_a, 7);

    /* a=7 -> l1=14 -> l2=15 -> l3=150 */
    TEST_ASSERT_EQUAL(150, lv_subject_get_int(l3));
    /* Each level evaluated exactly once, pulled through by the eager tail. */
    TEST_ASSERT_EQUAL(1, l1_runs);
    TEST_ASSERT_EQUAL(1, l2_runs);
    TEST_ASSERT_EQUAL(1, l3_runs);
}

/*--------------------------------------------------------
 * Cycles
 *-------------------------------------------------------*/

static lv_subject_t * self_ref;

static bool self_reading_mapper(lv_subject_t * subject, void * user_data, int32_t * value)
{
    LV_UNUSED(user_data);
    LV_UNUSED(subject);
    mapper_runs++;
    /* Reading itself: a cycle. It has to be refused rather than recursing. */
    *value = lv_subject_get_int(self_ref) + 1;
    return true;
}

void test_subject_mapper_cycle_is_refused(void)
{
    self_ref = subject_create(LV_SUBJECT_TYPE_INT);
    mapper_runs = 0;
    lv_subject_set_int_mapper(self_ref, self_reading_mapper, NULL);

    /* The guard stops the recursion, so the mapper runs once, not forever. */
    TEST_ASSERT_EQUAL(1, mapper_runs);
    /* Reading itself registers no self-dependency. */
    TEST_ASSERT_EQUAL(0, lv_subject_get_dependency_count(self_ref));
}

/*--------------------------------------------------------
 * A mapper may not write a Subject
 *-------------------------------------------------------*/

static lv_subject_t * write_target;

/* Writing from inside a mapper has to be refused. Allowing it would let an evaluation
 * re-dirty what is being computed, so ordering and termination stop being predictable. */
static bool illegally_writing_mapper(lv_subject_t * subject, void * user_data, int32_t * value)
{
    LV_UNUSED(subject);
    LV_UNUSED(user_data);
    mapper_runs++;
    *value = lv_subject_get_int(dep_a);
    lv_subject_set_int(write_target, 999);   /* refused and warned about */
    return true;
}

void test_subject_mapper_may_not_write_a_subject(void)
{
    dep_a = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int(dep_a, 1);
    write_target = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int(write_target, 7);

    lv_subject_t * mirror = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int_mapper(mirror, illegally_writing_mapper, NULL);
    lv_subject_set_mode(mirror, LV_SUBJECT_MODE_EAGER);

    mapper_runs = 0;
    lv_subject_set_int(dep_a, 3);

    TEST_ASSERT_EQUAL(1, mapper_runs);
    TEST_ASSERT_EQUAL(3, lv_subject_get_int(mirror));
    /* The write was ignored, so the target still holds what it had. */
    TEST_ASSERT_EQUAL(7, lv_subject_get_int(write_target));
}

/* Observer callbacks, unlike mappers, MAY write Subjects. That is where a side effect
 * belongs. */
static void writing_observer_cb(lv_observer_t * observer, lv_subject_t * subject)
{
    LV_UNUSED(observer);
    lv_subject_set_int(write_target, lv_subject_get_int(subject) * 10);
}

void test_observer_callback_may_write_a_subject(void)
{
    dep_a = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int(dep_a, 1);
    write_target = subject_create(LV_SUBJECT_TYPE_INT);

    lv_subject_add_observer(dep_a, writing_observer_cb, NULL);
    lv_subject_set_int(dep_a, 4);
    TEST_ASSERT_EQUAL(40, lv_subject_get_int(write_target));
}

/*--------------------------------------------------------
 * Graph teardown
 *-------------------------------------------------------*/

void test_subject_delete_mid_graph_leaves_no_dangling_edge(void)
{
    dep_a = subject_create(LV_SUBJECT_TYPE_INT);
    dep_b = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int(dep_a, 1);
    lv_subject_set_int(dep_b, 2);

    lv_subject_t * sum = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int_mapper(sum, sum_mapper, NULL);
    TEST_ASSERT_EQUAL(3, lv_subject_get_int(sum));
    TEST_ASSERT_EQUAL(1, lv_ll_get_len(&dep_a->dependents));

    subject_delete(sum);

    /* The reverse edges are gone, so marking dirty cannot reach freed memory. */
    TEST_ASSERT_EQUAL(0, lv_ll_get_len(&dep_a->dependents));
    TEST_ASSERT_EQUAL(0, lv_ll_get_len(&dep_b->dependents));

    /* Which means this is safe. */
    lv_subject_set_int(dep_a, 100);
    TEST_ASSERT_EQUAL(100, lv_subject_get_int(dep_a));
}

void test_subject_removing_mapper_drops_dependencies(void)
{
    dep_a = subject_create(LV_SUBJECT_TYPE_INT);
    dep_b = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int(dep_a, 1);
    lv_subject_set_int(dep_b, 2);

    lv_subject_t * sum = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int_mapper(sum, sum_mapper, NULL);
    TEST_ASSERT_EQUAL(3, lv_subject_get_int(sum));

    lv_subject_set_int_mapper(sum, NULL, NULL);
    /* Without a mapper it is an ordinary writable Subject again. */
    lv_subject_set_int(sum, 42);
    TEST_ASSERT_EQUAL(42, lv_subject_get_int(sum));
    lv_subject_set_int(dep_a, 9);
    TEST_ASSERT_EQUAL(42, lv_subject_get_int(sum));
}

/*--------------------------------------------------------
 * Mappers of the other Subject types
 *-------------------------------------------------------*/

#if LV_USE_FLOAT
static bool float_half_mapper(lv_subject_t * subject, void * user_data, float * value)
{
    LV_UNUSED(user_data);
    LV_UNUSED(subject);
    float next = (float)lv_subject_get_int(dep_a) / 2.0f;
    if(next == *value) return false;
    *value = next;
    return true;
}

void test_subject_float_mapper(void)
{
    dep_a = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int(dep_a, 7);

    lv_subject_t * half = subject_create(LV_SUBJECT_TYPE_FLOAT);
    lv_subject_set_float_mapper(half, float_half_mapper, NULL);

    TEST_ASSERT_EQUAL_FLOAT(3.5f, lv_subject_get_float(half));
    TEST_ASSERT_EQUAL(1, lv_subject_get_dependency_count(half));

    lv_subject_set_int(dep_a, 10);
    TEST_ASSERT_EQUAL_FLOAT(5.0f, lv_subject_get_float(half));
}
#endif

/* A string mapper receives the Subject's buffer, not a value. */
static bool string_format_mapper(lv_subject_t * subject, void * user_data, char * buf,
                                 size_t size)
{
    LV_UNUSED(user_data);
    LV_UNUSED(subject);
    char next[32];
    lv_snprintf(next, sizeof(next), "%" LV_PRId32 " C", lv_subject_get_int(dep_a));
    if(lv_strcmp(next, buf) == 0) return false;
    lv_strlcpy(buf, next, size);
    return true;
}

void test_subject_string_mapper(void)
{
    dep_a = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int(dep_a, 21);

    static char buf[32];
    lv_subject_t * text = subject_create(LV_SUBJECT_TYPE_STRING);
    lv_subject_set_string_buffer_static(text, buf, sizeof(buf));
    lv_subject_set_string_mapper(text, string_format_mapper, NULL);

    TEST_ASSERT_EQUAL_STRING("21 C", lv_subject_get_string(text));

    lv_subject_set_int(dep_a, 22);
    TEST_ASSERT_EQUAL_STRING("22 C", lv_subject_get_string(text));
}

static bool color_mapper(lv_subject_t * subject, void * user_data, lv_color_t * value)
{
    LV_UNUSED(user_data);
    LV_UNUSED(subject);
    lv_color_t next = lv_subject_get_int(dep_a) > 50 ? lv_color_hex(0xff0000) : lv_color_hex(0x00ff00);
    if(lv_color_to_u32(next) == lv_color_to_u32(*value)) return false;
    *value = next;
    return true;
}

void test_subject_color_mapper(void)
{
    dep_a = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int(dep_a, 10);

    lv_subject_t * color = subject_create(LV_SUBJECT_TYPE_COLOR);
    lv_subject_set_color_mapper(color, color_mapper, NULL);

    TEST_ASSERT_EQUAL_UINT32(lv_color_to_u32(lv_color_hex(0x00ff00)),
                             lv_color_to_u32(lv_subject_get_color(color)));

    lv_subject_set_int(dep_a, 90);
    TEST_ASSERT_EQUAL_UINT32(lv_color_to_u32(lv_color_hex(0xff0000)),
                             lv_color_to_u32(lv_subject_get_color(color)));
}

static const char * const pointer_options[] = { "off", "on" };

static bool pointer_mapper(lv_subject_t * subject, void * user_data, const void ** value)
{
    LV_UNUSED(user_data);
    LV_UNUSED(subject);
    const void * next = pointer_options[lv_subject_get_int(dep_a) ? 1 : 0];
    if(next == *value) return false;
    *value = next;
    return true;
}

void test_subject_pointer_mapper(void)
{
    dep_a = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int(dep_a, 0);

    lv_subject_t * selected = subject_create(LV_SUBJECT_TYPE_POINTER);
    lv_subject_set_pointer_mapper(selected, pointer_mapper, NULL);

    TEST_ASSERT_EQUAL_STRING("off", (const char *)lv_subject_get_pointer(selected));

    lv_subject_set_int(dep_a, 1);
    TEST_ASSERT_EQUAL_STRING("on", (const char *)lv_subject_get_pointer(selected));
}

/* A Subject with no value of its own, used purely to aggregate dependencies. This is
 * what replaced the Group Subject type: an LV_SUBJECT_TYPE_NONE Subject plus a mapper
 * that reads whatever it wants to react to. */
static bool either_is_high_mapper(lv_subject_t * subject, void * user_data)
{
    LV_UNUSED(user_data);
    LV_UNUSED(subject);
    mapper_runs++;
    /* Notify only when at least one member crossed the threshold, rather than on
     * every member change the way the built-in Group mapper does. */
    return lv_subject_get_int(dep_a) > 50 || lv_subject_get_int(dep_b) > 50;
}

void test_subject_none_mapper_aggregates_with_a_custom_rule(void)
{
    dep_a = subject_create(LV_SUBJECT_TYPE_INT);
    dep_b = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int(dep_a, 1);
    lv_subject_set_int(dep_b, 1);

    lv_subject_t * any_high = subject_create(LV_SUBJECT_TYPE_NONE);
    lv_subject_set_none_mapper(any_high, either_is_high_mapper, NULL);
    lv_subject_add_observer(any_high, observer_basic, NULL);

    observer_called = 0;
    mapper_runs = 0;

    /* Below the threshold: the mapper runs but reports no change, so no notification. */
    lv_subject_set_int(dep_a, 10);
    TEST_ASSERT_EQUAL(1, mapper_runs);
    TEST_ASSERT_EQUAL(0, observer_called);

    /* Above it: notified. */
    lv_subject_set_int(dep_a, 80);
    TEST_ASSERT_EQUAL(2, mapper_runs);
    TEST_ASSERT_EQUAL(1, observer_called);
}

/*=====================================================================
 * Observer mappers
 *====================================================================*/

/* The motivating case: an integer Subject driving a Label's text directly. */
static bool int_to_text_mapper(lv_observer_t * observer, lv_subject_value_t input, const char ** out)
{
    LV_UNUSED(input);
    /* The mapper owns the storage; the Observer only keeps the pointer. */
    static char buf[32];
    lv_subject_t * subject = lv_observer_get_subject(observer);
    char next[32];
    lv_snprintf(next, sizeof(next), "Value: %" LV_PRId32, lv_subject_get_int(subject));
    if(lv_strcmp(next, buf) == 0) return false;
    lv_strlcpy(buf, next, sizeof(buf));
    *out = buf;
    return true;
}

void test_observer_mapper_binds_int_subject_to_label_text(void)
{
    lv_subject_t * subject = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int(subject, 7);

    lv_obj_t * label = lv_label_create(lv_screen_active());
    lv_observer_t * observer = lv_obj_bind_string_mapped(label, subject, lv_label_set_text, int_to_text_mapper, NULL);
    TEST_ASSERT_NOT_NULL(observer);

    /* An integer Subject bound straight to a string setter. */
    TEST_ASSERT_EQUAL_STRING("Value: 7", lv_label_get_text(label));

    lv_subject_set_int(subject, 42);
    TEST_ASSERT_EQUAL_STRING("Value: 42", lv_label_get_text(label));
}

static uint32_t observer_mapper_runs;
static uint32_t observer_target_writes;

static void counting_set_int(lv_obj_t * obj, int32_t value)
{
    LV_UNUSED(obj);
    LV_UNUSED(value);
    observer_target_writes++;
}

/* Maps any value to a constant, so the mapper reports "unchanged" from the second
 * run on and the target must stop being written. */
static bool constant_int_mapper(lv_observer_t * observer, lv_subject_value_t input, int32_t * out)
{
    LV_UNUSED(input);
    LV_UNUSED(observer);
    observer_mapper_runs++;
    if(*out == 5) return false;
    *out = 5;
    return true;
}

void test_observer_mapper_returning_false_skips_the_target(void)
{
    lv_subject_t * subject = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int(subject, 1);

    lv_obj_t * obj = lv_obj_create(lv_screen_active());
    observer_mapper_runs = 0;
    observer_target_writes = 0;
    lv_obj_bind_int_mapped(obj, subject, counting_set_int, constant_int_mapper, NULL);

    TEST_ASSERT_EQUAL(1, observer_mapper_runs);
    TEST_ASSERT_EQUAL(1, observer_target_writes);

    /* The Subject changes, the mapper runs, but its output does not change. */
    lv_subject_set_int(subject, 2);
    TEST_ASSERT_EQUAL(2, observer_mapper_runs);
    TEST_ASSERT_EQUAL(1, observer_target_writes);

    lv_subject_set_int(subject, 3);
    TEST_ASSERT_EQUAL(3, observer_mapper_runs);
    TEST_ASSERT_EQUAL(1, observer_target_writes);
}

/* A string Subject driving a boolean Widget property. */
static bool string_non_empty_mapper(lv_observer_t * observer, lv_subject_value_t input, bool * out)
{
    LV_UNUSED(input);
    lv_subject_t * subject = lv_observer_get_subject(observer);
    const char * text = lv_subject_get_string(subject);
    bool next = text != NULL && text[0] != '\0';
    if(next == *out) return false;
    *out = next;
    return true;
}

void test_observer_mapper_binds_string_subject_to_bool(void)
{
    static char buf[16];
    lv_subject_t * subject = subject_create(LV_SUBJECT_TYPE_STRING);
    lv_subject_set_string_buffer_static(subject, buf, sizeof(buf));
    lv_subject_copy_string(subject, "hello");

    lv_obj_t * obj = lv_obj_create(lv_screen_active());
    lv_obj_bind_bool_mapped(obj, subject, lv_obj_set_hidden, string_non_empty_mapper, NULL);
    TEST_ASSERT_TRUE(lv_obj_is_hidden(obj));

    lv_subject_copy_string(subject, "");
    TEST_ASSERT_FALSE(lv_obj_is_hidden(obj));
}

/* An Observer's mapper must not register dependencies: an Observer is not a node in
 * the dependency graph. */
static bool dependency_probing_mapper(lv_observer_t * observer, lv_subject_value_t input, int32_t * out)
{
    LV_UNUSED(input);
    LV_UNUSED(observer);
    *out = lv_subject_get_int(dep_a) + lv_subject_get_int(dep_b);
    return true;
}

void test_observer_mapper_does_not_autowire(void)
{
    dep_a = subject_create(LV_SUBJECT_TYPE_INT);
    dep_b = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int(dep_a, 1);
    lv_subject_set_int(dep_b, 2);

    lv_subject_t * trigger = subject_create(LV_SUBJECT_TYPE_INT);
    lv_obj_t * obj = lv_obj_create(lv_screen_active());
    observer_target_writes = 0;
    lv_obj_bind_int_mapped(obj, trigger, counting_set_int, dependency_probing_mapper, NULL);
    TEST_ASSERT_EQUAL(1, observer_target_writes);

    /* dep_a and dep_b were read by the mapper but are not dependencies of anything. */
    TEST_ASSERT_EQUAL(0, lv_ll_get_len(&dep_a->dependents));
    TEST_ASSERT_EQUAL(0, lv_ll_get_len(&dep_b->dependents));

    /* So changing them does not run the Observer; only its own Subject does. */
    lv_subject_set_int(dep_a, 100);
    TEST_ASSERT_EQUAL(1, observer_target_writes);

    lv_subject_set_int(trigger, 1);
    TEST_ASSERT_EQUAL(2, observer_target_writes);
}

/* Observer callbacks read Subjects all over LVGL. If the "currently evaluating"
 * pointer were not cleared around notification, those reads would be recorded as
 * dependencies of whatever mapper is running further up the stack. */
static bool notifying_mapper(lv_subject_t * subject, void * user_data, int32_t * value)
{
    LV_UNUSED(user_data);
    LV_UNUSED(subject);
    *value = lv_subject_get_int(dep_a);
    return true;
}

static void reading_observer_cb(lv_observer_t * observer, lv_subject_t * subject)
{
    LV_UNUSED(observer);
    LV_UNUSED(subject);
    (void)lv_subject_get_int(dep_b);  /* an unrelated read from inside a notification */
    observer_called++;
}

void test_observer_notification_does_not_pollute_dependencies(void)
{
    dep_a = subject_create(LV_SUBJECT_TYPE_INT);
    dep_b = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int(dep_a, 1);
    lv_subject_set_int(dep_b, 1);

    lv_subject_t * mirror = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int_mapper(mirror, notifying_mapper, NULL);
    lv_subject_add_observer(mirror, reading_observer_cb, NULL);

    observer_called = 0;
    lv_subject_set_int(dep_a, 5);
    TEST_ASSERT_EQUAL(1, observer_called);

    /* Only dep_a is a dependency. dep_b was read by the Observer, not by the mapper. */
    TEST_ASSERT_EQUAL(1, lv_subject_get_dependency_count(mirror));
    TEST_ASSERT_EQUAL(dep_a, lv_subject_get_dependency(mirror, 0));
    TEST_ASSERT_EQUAL(0, lv_ll_get_len(&dep_b->dependents));
}

/* Subscribing to a Subject that is dirty has to evaluate it first, and the brand-new
 * Observer must be notified exactly once. It would be notified twice if the evaluation
 * happened while the Observer was already linked into the Subject's list. */
void test_subject_subscribing_to_dirty_lazy_subject_notifies_once(void)
{
    dep_a = subject_create(LV_SUBJECT_TYPE_INT);
    dep_b = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int(dep_a, 1);
    lv_subject_set_int(dep_b, 1);

    lv_subject_t * sum = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int_mapper(sum, sum_mapper, NULL);

    /* Dirty it without evaluating it: lazy, no Observers yet. */
    lv_subject_set_int(dep_a, 5);
    TEST_ASSERT_TRUE(lv_subject_is_dirty(sum));

    observer_called = 0;
    lv_subject_add_observer(sum, observer_basic, NULL);

    TEST_ASSERT_EQUAL(1, observer_called);
    TEST_ASSERT_FALSE(lv_subject_is_dirty(sum));
    /* And the first notification saw the fresh value, not the stale one. */
    TEST_ASSERT_EQUAL(6, lv_subject_get_int(sum));
}

/* Same requirement for a Widget bound through an Observer mapper. */
void test_observer_mapped_bind_to_dirty_subject_pushes_once(void)
{
    dep_a = subject_create(LV_SUBJECT_TYPE_INT);
    dep_b = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int(dep_a, 1);
    lv_subject_set_int(dep_b, 1);

    lv_subject_t * sum = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int_mapper(sum, sum_mapper, NULL);
    lv_subject_set_int(dep_a, 40);
    TEST_ASSERT_TRUE(lv_subject_is_dirty(sum));

    lv_obj_t * label = lv_label_create(lv_screen_active());
    lv_obj_bind_string_mapped(label, sum, lv_label_set_text, int_to_text_mapper, NULL);

    TEST_ASSERT_EQUAL_STRING("Value: 41", lv_label_get_text(label));
}

/* The eager drain and the flush walk share the pull mechanism but use different
 * predicates, so the glitch-free guarantee has to be checked on the flush path too.
 * Here the two intermediates have no Observers and are not eager, so the flush walk
 * skips them and only the sink is due; its mapper has to pull them. */
void test_subject_diamond_behind_flush_evaluates_sink_once(void)
{
    dep_a = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int(dep_a, 1);

    diamond_b = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int_mapper(diamond_b, double_mapper, NULL);
    diamond_c = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int_mapper(diamond_c, triple_mapper, NULL);

    /* The sink stays lazy; only a lazy Observer waits on it. */
    lv_subject_t * sink = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int_mapper(sink, diamond_sink_mapper, NULL);
    lv_observer_t * observer = lv_subject_add_observer(sink, observer_basic, NULL);
    lv_observer_set_mode(observer, LV_OBSERVER_MODE_BATCHED);
    TEST_ASSERT_FALSE(lv_subject_is_eager(sink));
    TEST_ASSERT_EQUAL(5, lv_subject_get_int(sink));

    mapper_runs = 0;
    mapper_saw_inconsistent = 0;
    observer_called = 0;
    lv_subject_set_int(dep_a, 10);

    /* Nothing was due inside set(). */
    TEST_ASSERT_EQUAL(0, mapper_runs);
    TEST_ASSERT_TRUE(lv_subject_is_dirty(sink));

    lv_subject_flush();

    TEST_ASSERT_EQUAL(1, mapper_runs);             /* the sink evaluated once */
    TEST_ASSERT_EQUAL(0, mapper_saw_inconsistent); /* and saw a consistent pair */
    TEST_ASSERT_EQUAL(1, observer_called);
    TEST_ASSERT_EQUAL(50, lv_subject_get_int(sink));
}

/* The mirror of test_subject_switching_observer_to_eager_flushes_immediately, for the
 * Subject's own mode rather than an Observer's. */
void test_subject_switching_to_eager_while_dirty_evaluates_immediately(void)
{
    dep_a = subject_create(LV_SUBJECT_TYPE_INT);
    dep_b = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int(dep_a, 1);
    lv_subject_set_int(dep_b, 1);

    lv_subject_t * sum = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int_mapper(sum, sum_mapper, NULL);
    TEST_ASSERT_EQUAL(2, lv_subject_get_int(sum));

    mapper_runs = 0;
    lv_subject_set_int(dep_a, 6);
    TEST_ASSERT_TRUE(lv_subject_is_dirty(sum));
    TEST_ASSERT_EQUAL(0, mapper_runs);

    lv_subject_set_mode(sum, LV_SUBJECT_MODE_EAGER);
    /* The pending work was due the moment the Subject became eager. */
    TEST_ASSERT_EQUAL(1, mapper_runs);
    TEST_ASSERT_FALSE(lv_subject_is_dirty(sum));
    TEST_ASSERT_EQUAL(7, sum->value.num);
}

/*=====================================================================
 * Deferred notification
 *====================================================================*/

/* A plain source Subject with a batched Observer must not notify inside set(). Before
 * notification was deferred this fired once per write, which made
 * LV_OBSERVER_MODE_BATCHED meaningless for source Subjects. */
void test_observer_batched_on_source_subject_coalesces(void)
{
    lv_subject_t * src = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int(src, 1);

    lv_observer_t * observer = lv_subject_add_observer(src, observer_basic, NULL);
    lv_observer_set_mode(observer, LV_OBSERVER_MODE_BATCHED);

    observer_called = 0;
    lv_subject_set_int(src, 2);
    lv_subject_set_int(src, 3);
    lv_subject_set_int(src, 4);

    /* Nothing inside set(), however many writes. */
    TEST_ASSERT_EQUAL(0, observer_called);
    /* But the value itself is stored immediately. */
    TEST_ASSERT_EQUAL(4, lv_subject_get_int(src));

    lv_subject_flush();
    TEST_ASSERT_EQUAL(1, observer_called);
}

void test_observer_immediate_on_source_subject_notifies_per_write(void)
{
    lv_subject_t * src = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int(src, 1);

    /* LV_OBSERVER_MODE_IMMEDIATE is the default, and reproduces the pre-mapper behavior. */
    lv_subject_add_observer(src, observer_basic, NULL);

    observer_called = 0;
    lv_subject_set_int(src, 2);
    lv_subject_set_int(src, 3);
    lv_subject_set_int(src, 4);
    TEST_ASSERT_EQUAL(3, observer_called);
}

/* Reading must bring the value up to date without delivering notifications, otherwise
 * any reader would break the "once per frame" guarantee. */
void test_observer_batched_read_refreshes_value_but_does_not_notify(void)
{
    dep_a = subject_create(LV_SUBJECT_TYPE_INT);
    dep_b = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int(dep_a, 1);
    lv_subject_set_int(dep_b, 1);

    lv_subject_t * sum = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int_mapper(sum, sum_mapper, NULL);
    lv_observer_t * observer = lv_subject_add_observer(sum, observer_basic, NULL);
    lv_observer_set_mode(observer, LV_OBSERVER_MODE_BATCHED);

    observer_called = 0;
    lv_subject_set_int(dep_a, 9);

    /* The read returns the fresh value... */
    TEST_ASSERT_EQUAL(10, lv_subject_get_int(sum));
    /* ...but does not notify. */
    TEST_ASSERT_EQUAL(0, observer_called);

    lv_subject_flush();
    TEST_ASSERT_EQUAL(1, observer_called);
}




/* A lazy Subject nothing reads and nothing observes never runs its mapper at all. */
void test_subject_lazy_unread_never_evaluates(void)
{
    dep_a = subject_create(LV_SUBJECT_TYPE_INT);
    dep_b = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int(dep_a, 1);
    lv_subject_set_int(dep_b, 1);

    lv_subject_t * sum = subject_create(LV_SUBJECT_TYPE_INT);
    mapper_runs = 0;
    lv_subject_set_int_mapper(sum, sum_mapper, NULL);
    /* Attaching the mapper evaluates once so the Subject starts from a real value. */
    uint32_t after_attach = mapper_runs;

    lv_subject_set_int(dep_a, 2);
    lv_subject_set_int(dep_a, 3);
    lv_subject_set_int(dep_a, 4);
    lv_subject_flush();

    /* Lazy, unobserved and unread: no work was done for any of those writes. */
    TEST_ASSERT_EQUAL(after_attach, mapper_runs);
    TEST_ASSERT_TRUE(lv_subject_is_dirty(sum));
}

/* An eager dependent still drags a lazy Subject into evaluating inside set(), because
 * evaluating the dependent pulls it. */
void test_subject_lazy_is_pulled_by_an_eager_dependent(void)
{
    dep_a = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int(dep_a, 1);

    chain_l1 = subject_create(LV_SUBJECT_TYPE_INT);   /* lazy */
    lv_subject_set_int_mapper(chain_l1, chain_l1_mapper, NULL);
    chain_l2 = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int_mapper(chain_l2, chain_l2_mapper, NULL);
    lv_subject_set_mode(chain_l2, LV_SUBJECT_MODE_EAGER);

    l1_runs = 0;
    l2_runs = 0;
    lv_subject_set_int(dep_a, 5);

    /* Both ran inside the set(), the lazy one because the eager one needed it. */
    TEST_ASSERT_EQUAL(1, l1_runs);
    TEST_ASSERT_EQUAL(1, l2_runs);
    TEST_ASSERT_EQUAL(11, chain_l2->value.num);   /* 5*2 + 1 */
}

/*=====================================================================
 * Reading and writing across int and float
 *====================================================================*/

void test_subject_int_reads_as_float_exactly(void)
{
    lv_subject_t * count = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int(count, -7);

    TEST_ASSERT_EQUAL_FLOAT(-7.0f, lv_subject_get_float(count));
    TEST_ASSERT_EQUAL_FLOAT(-7.0f, lv_subject_peek_float(count));
}

void test_subject_float_reads_as_int_rounded_by_default(void)
{
    lv_subject_t * temp = subject_create(LV_SUBJECT_TYPE_FLOAT);

    /* Rounding is the default, so 4.9 shows as 5 and not as 4. */
    lv_subject_set_float(temp, 4.9f);
    TEST_ASSERT_EQUAL(5, lv_subject_get_int(temp));
    TEST_ASSERT_EQUAL(5, lv_subject_peek_int(temp));

    /* Away from zero on both sides. */
    lv_subject_set_float(temp, 2.5f);
    TEST_ASSERT_EQUAL(3, lv_subject_get_int(temp));
    lv_subject_set_float(temp, -2.5f);
    TEST_ASSERT_EQUAL(-3, lv_subject_get_int(temp));
}

void test_subject_float_reads_as_int_truncated_when_asked(void)
{
    lv_subject_t * temp = subject_create(LV_SUBJECT_TYPE_FLOAT);
    lv_subject_set_rounding(temp, LV_SUBJECT_ROUND_TOWARD_ZERO);

    lv_subject_set_float(temp, 4.9f);
    TEST_ASSERT_EQUAL(4, lv_subject_get_int(temp));
    lv_subject_set_float(temp, -4.9f);
    TEST_ASSERT_EQUAL(-4, lv_subject_get_int(temp));
}

void test_subject_exact_rounding_still_answers_with_the_nearest(void)
{
    lv_subject_t * steps = subject_create(LV_SUBJECT_TYPE_FLOAT);
    lv_subject_set_rounding(steps, LV_SUBJECT_ROUND_EXACT);

    /* A whole number is what this mode expects, and it passes through. */
    lv_subject_set_float(steps, 12.0f);
    TEST_ASSERT_EQUAL(12, lv_subject_get_int(steps));

    /* A fraction is reported, but the read still answers: a getter that returned
     * nothing would turn a warning into a fault. */
    lv_subject_set_float(steps, 12.4f);
    TEST_ASSERT_EQUAL(12, lv_subject_get_int(steps));

    /* Big enough that a float cannot hold a fraction at all, so nothing is wrong with it
     * and the tolerance has to scale with the magnitude to say so. */
    lv_subject_set_float(steps, 1000000.0f);
    TEST_ASSERT_EQUAL(1000000, lv_subject_get_int(steps));
}

void test_subject_writing_a_float_into_an_int_follows_the_rounding(void)
{
    lv_subject_t * count = subject_create(LV_SUBJECT_TYPE_INT);

    /* The rounding rule belongs to the Subject, so it governs this direction too. */
    lv_subject_set_float(count, 4.9f);
    TEST_ASSERT_EQUAL(5, lv_subject_get_int(count));

    lv_subject_set_rounding(count, LV_SUBJECT_ROUND_TOWARD_ZERO);
    lv_subject_set_float(count, 4.4f);
    TEST_ASSERT_EQUAL(4, lv_subject_get_int(count));
}

void test_subject_writing_an_int_into_a_float_is_exact(void)
{
    lv_subject_t * temp = subject_create(LV_SUBJECT_TYPE_FLOAT);

    lv_subject_set_int(temp, -12);
    TEST_ASSERT_EQUAL_FLOAT(-12.0f, lv_subject_get_float(temp));
}

void test_subject_a_cross_type_read_still_records_the_dependency(void)
{
    dep_a = subject_create(LV_SUBJECT_TYPE_FLOAT);
    lv_subject_set_float(dep_a, 2.4f);

    /* `rounded` reads a float Subject as an int. The edge has to be on the Subject, not
     * on the type it was read as, or the graph would lose it. */
    lv_subject_t * rounded = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int_mapper(rounded, int_of_dep_a_mapper, NULL);

    TEST_ASSERT_EQUAL(2, lv_subject_get_int(rounded));
    TEST_ASSERT_EQUAL(1, lv_subject_get_dependency_count(rounded));
    TEST_ASSERT_EQUAL_PTR(dep_a, lv_subject_get_dependency(rounded, 0));

    /* And it propagates, which is the point of the edge existing. */
    lv_subject_set_float(dep_a, 8.6f);
    TEST_ASSERT_EQUAL(9, lv_subject_get_int(rounded));
}

void test_subject_a_checked_read_reports_a_lost_fraction(void)
{
    lv_subject_t * temp = subject_create(LV_SUBJECT_TYPE_FLOAT);
    int32_t v = 0;

    lv_subject_set_float(temp, 12.0f);
    TEST_ASSERT_EQUAL(LV_RESULT_OK, lv_subject_get_int_checked(temp, &v));
    TEST_ASSERT_EQUAL(12, v);

    /* Reported, and the value is still there: a caller that ignores the result is no
     * worse off than one calling the plain getter. */
    lv_subject_set_float(temp, 12.4f);
    TEST_ASSERT_EQUAL(LV_RESULT_INVALID, lv_subject_get_int_checked(temp, &v));
    TEST_ASSERT_EQUAL(12, v);

    /* The answer does not depend on the mode: the mode picks the integer, this says
     * whether that integer is the whole story. */
    lv_subject_set_rounding(temp, LV_SUBJECT_ROUND_TOWARD_ZERO);
    TEST_ASSERT_EQUAL(LV_RESULT_INVALID, lv_subject_get_int_checked(temp, &v));
    TEST_ASSERT_EQUAL(12, v);
}

void test_subject_a_checked_read_reports_an_int_too_big_for_a_float(void)
{
    lv_subject_t * count = subject_create(LV_SUBJECT_TYPE_INT);
    float v = 0.0f;

    lv_subject_set_int(count, LV_SUBJECT_INT_EXACT_IN_FLOAT);
    TEST_ASSERT_EQUAL(LV_RESULT_OK, lv_subject_get_float_checked(count, &v));
    TEST_ASSERT_EQUAL_FLOAT(16777216.0f, v);

    /* One past the mantissa: the float cannot tell it from the value below. */
    lv_subject_set_int(count, LV_SUBJECT_INT_EXACT_IN_FLOAT + 1);
    TEST_ASSERT_EQUAL(LV_RESULT_INVALID, lv_subject_get_float_checked(count, &v));

    lv_subject_set_int(count, -LV_SUBJECT_INT_EXACT_IN_FLOAT - 1);
    TEST_ASSERT_EQUAL(LV_RESULT_INVALID, lv_subject_get_float_checked(count, &v));

    /* A float Subject read as a float never loses anything. */
    lv_subject_t * temp = subject_create(LV_SUBJECT_TYPE_FLOAT);
    lv_subject_set_float(temp, 0.25f);
    TEST_ASSERT_EQUAL(LV_RESULT_OK, lv_subject_get_float_checked(temp, &v));
    TEST_ASSERT_EQUAL_FLOAT(0.25f, v);
}

void test_subject_exact_refuses_a_write_that_would_lose_a_fraction(void)
{
    lv_subject_t * count = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int(count, 7);
    lv_subject_set_rounding(count, LV_SUBJECT_ROUND_EXACT);

    /* Refused, and the Subject keeps what it had. */
    lv_subject_set_float(count, 2.7f);
    TEST_ASSERT_EQUAL(7, lv_subject_get_int(count));

    /* A whole number goes in. */
    lv_subject_set_float(count, 3.0f);
    TEST_ASSERT_EQUAL(3, lv_subject_get_int(count));

    /* The other modes were asked to round, so they still do. */
    lv_subject_set_rounding(count, LV_SUBJECT_ROUND_NEAREST);
    lv_subject_set_float(count, 2.7f);
    TEST_ASSERT_EQUAL(3, lv_subject_get_int(count));
}

void test_subject_exact_refuses_an_int_a_float_cannot_hold(void)
{
    lv_subject_t * big = subject_create(LV_SUBJECT_TYPE_FLOAT);
    lv_subject_set_float(big, 1.0f);
    lv_subject_set_rounding(big, LV_SUBJECT_ROUND_EXACT);

    /* Refused: the float would read back as a different number. */
    lv_subject_set_int(big, LV_SUBJECT_INT_EXACT_IN_FLOAT + 1);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, lv_subject_get_float(big));

    /* At the limit it still fits. */
    lv_subject_set_int(big, LV_SUBJECT_INT_EXACT_IN_FLOAT);
    TEST_ASSERT_EQUAL_FLOAT(16777216.0f, lv_subject_get_float(big));

    /* Without LV_SUBJECT_ROUND_EXACT the precision loss is not an error. */
    lv_subject_set_rounding(big, LV_SUBJECT_ROUND_NEAREST);
    lv_subject_set_int(big, LV_SUBJECT_INT_EXACT_IN_FLOAT + 1);
    TEST_ASSERT_EQUAL_FLOAT(16777216.0f, lv_subject_get_float(big));
}

/*=====================================================================
 * Deleting a Subject that others depend on
 *====================================================================*/

void test_subject_delete_refused_while_it_is_a_dependency(void)
{
    dep_a = subject_create(LV_SUBJECT_TYPE_INT);
    dep_b = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int(dep_a, 2);
    lv_subject_set_int(dep_b, 3);

    lv_subject_t * sum = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int_mapper(sum, sum_mapper, NULL);
    TEST_ASSERT_EQUAL(5, lv_subject_get_int(sum));
    TEST_ASSERT_EQUAL(1, lv_ll_get_len(&dep_a->dependents));

    /* `sum`'s mapper reads dep_a, so deleting dep_a would leave it without an input. The
     * refusal is reported, so a caller can tell it still owns the Subject. */
    TEST_ASSERT_EQUAL(LV_RESULT_INVALID, lv_subject_delete(dep_a));

    /* Still there and still working. */
    lv_subject_set_int(dep_a, 10);
    TEST_ASSERT_EQUAL(13, lv_subject_get_int(sum));

    /* Deleting the dependent first makes the dependency deletable. */
    subject_delete(sum);
    TEST_ASSERT_EQUAL(0, lv_ll_get_len(&dep_a->dependents));
    TEST_ASSERT_EQUAL(LV_RESULT_OK, lv_subject_delete(dep_a));
    subject_forget(dep_a);
}

void test_subject_delete_cascade_removes_dependents(void)
{
    dep_a = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int(dep_a, 1);

    chain_l1 = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int_mapper(chain_l1, chain_l1_mapper, NULL);
    chain_l2 = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int_mapper(chain_l2, chain_l2_mapper, NULL);
    TEST_ASSERT_EQUAL(3, lv_subject_get_int(chain_l2));

    /* The cascade frees these transitively, so tearDown() must not see them. */
    subject_forget(dep_a);
    subject_forget(chain_l1);
    subject_forget(chain_l2);

    uint32_t before = lv_ll_get_len(&LV_GLOBAL_DEFAULT()->subject_ll);

    /* a -> l1 -> l2: deleting the root takes the whole chain with it. */
    lv_subject_delete_cascade(dep_a);

    /* All three are gone, not just the root. */
    TEST_ASSERT_EQUAL(before - 3, lv_ll_get_len(&LV_GLOBAL_DEFAULT()->subject_ll));

    /* And nothing is left to walk; a surviving edge would touch freed memory here. */
    lv_subject_flush();
}

void test_subject_delete_cascade_handles_a_diamond(void)
{
    dep_a = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int(dep_a, 1);

    diamond_b = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int_mapper(diamond_b, double_mapper, NULL);
    diamond_c = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int_mapper(diamond_c, triple_mapper, NULL);

    lv_subject_t * sink = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int_mapper(sink, diamond_sink_mapper, NULL);
    TEST_ASSERT_EQUAL(5, lv_subject_get_int(sink));

    /* The sink is reachable from the root by two paths and must still be deleted once. */
    TEST_ASSERT_EQUAL(2, lv_ll_get_len(&dep_a->dependents));

    subject_forget(dep_a);
    subject_forget(diamond_b);
    subject_forget(diamond_c);
    subject_forget(sink);

    uint32_t before = lv_ll_get_len(&LV_GLOBAL_DEFAULT()->subject_ll);

    lv_subject_delete_cascade(dep_a);

    /* Four gone, and the sink exactly once even though two paths reach it. */
    TEST_ASSERT_EQUAL(before - 4, lv_ll_get_len(&LV_GLOBAL_DEFAULT()->subject_ll));
    lv_subject_flush();
}

void test_subject_delete_cascade_on_a_leaf_is_a_plain_delete(void)
{
    dep_a = subject_create(LV_SUBJECT_TYPE_INT);
    dep_b = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int(dep_a, 1);
    lv_subject_set_int(dep_b, 1);

    lv_subject_t * sum = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int_mapper(sum, sum_mapper, NULL);
    TEST_ASSERT_EQUAL(2, lv_subject_get_int(sum));

    subject_forget(sum);
    uint32_t before = lv_ll_get_len(&LV_GLOBAL_DEFAULT()->subject_ll);
    lv_subject_delete_cascade(sum);  /* nothing depends on it */
    TEST_ASSERT_EQUAL(before - 1, lv_ll_get_len(&LV_GLOBAL_DEFAULT()->subject_ll));

    /* The dependencies survive, with their edges cleaned up. */
    TEST_ASSERT_EQUAL(0, lv_ll_get_len(&dep_a->dependents));
    lv_subject_set_int(dep_a, 5);
    TEST_ASSERT_EQUAL(5, lv_subject_get_int(dep_a));
}

/*=====================================================================
 * Derived-subject helpers
 *====================================================================*/



/* A pointer Subject has no natural ordering, which is the case the compare callback
 * exists for. */
typedef struct {
    const char * name;
    int32_t price;
} item_t;

static int cheaper_cb(lv_subject_t * subject, lv_subject_value_t a, lv_subject_value_t b)
{
    LV_UNUSED(subject);
    const item_t * ia = a.pointer;
    const item_t * ib = b.pointer;
    if(ia == NULL || ib == NULL) return 0;
    return (ia->price > ib->price) - (ia->price < ib->price);
}



void test_subject_create_clamped_int(void)
{
    lv_subject_t * raw = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int(raw, 50);

    lv_subject_t * bounded = lv_subject_create_clamped(raw, (lv_subject_value_t) {
        .num = 0
    }, (lv_subject_value_t) {
        .num = 100
    }, NULL);
    TEST_ASSERT_NOT_NULL(bounded);
    TEST_ASSERT_EQUAL(50, lv_subject_get_int(bounded));

    lv_subject_set_int(raw, 150);
    TEST_ASSERT_EQUAL(100, lv_subject_get_int(bounded));
    /* The source itself is untouched: this is a bounded mirror, not a clamp in place. */
    TEST_ASSERT_EQUAL(150, lv_subject_get_int(raw));

    lv_subject_set_int(raw, -20);
    TEST_ASSERT_EQUAL(0, lv_subject_get_int(bounded));

    lv_subject_delete(bounded);
}

#if LV_USE_FLOAT
void test_subject_create_clamped_float(void)
{
    lv_subject_t * raw = subject_create(LV_SUBJECT_TYPE_FLOAT);
    lv_subject_set_float(raw, 0.5f);

    lv_subject_t * bounded = lv_subject_create_clamped(raw, (lv_subject_value_t) {
        .float_v = 0.0f
    }, (lv_subject_value_t) {
        .float_v = 1.0f
    }, NULL);
    TEST_ASSERT_NOT_NULL(bounded);
    TEST_ASSERT_EQUAL_FLOAT(0.5f, lv_subject_get_float(bounded));

    lv_subject_set_float(raw, 2.5f);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, lv_subject_get_float(bounded));

    lv_subject_set_float(raw, -1.0f);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, lv_subject_get_float(bounded));

    lv_subject_delete(bounded);
}
#endif





/* The stored value handed to the mapper is the previous one, which is the whole reason
 * no previous-value bookkeeping is needed. */



/* A mapper reaches other Subjects through its captured state, so it does not need them
 * to be file-scope globals. */
typedef struct {
    lv_subject_t * left;
    lv_subject_t * right;
} pair_t;

static bool pair_sum_mapper(lv_subject_t * subject, void * user_data, int32_t * value)
{
    LV_UNUSED(subject);
    const pair_t * pair = user_data;
    int32_t next = lv_subject_get_int(pair->left) + lv_subject_get_int(pair->right);
    if(next == *value) return false;
    *value = next;
    return true;
}

void test_subject_mapper_captures_non_global_subjects(void)
{
    lv_subject_t * left = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_t * right = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int(left, 3);
    lv_subject_set_int(right, 4);

    static pair_t pair;
    pair.left = left;
    pair.right = right;

    lv_subject_t * sum = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int_mapper(sum, pair_sum_mapper, &pair);

    TEST_ASSERT_EQUAL(7, lv_subject_get_int(sum));
    TEST_ASSERT_EQUAL(2, lv_subject_get_dependency_count(sum));

    lv_subject_set_int(left, 10);
    TEST_ASSERT_EQUAL(14, lv_subject_get_int(sum));
}

/*=====================================================================
 * Ready-made comparisons and the generic clamp
 *====================================================================*/

void test_subject_compare_callbacks(void)
{
    lv_subject_t * subject = subject_create(LV_SUBJECT_TYPE_INT);

    lv_subject_value_t a;
    lv_subject_value_t b;
    lv_memzero(&a, sizeof(a));
    lv_memzero(&b, sizeof(b));

    a.num = 1;
    b.num = 2;
    TEST_ASSERT_TRUE(lv_subject_compare_int(subject, a, b) < 0);
    TEST_ASSERT_TRUE(lv_subject_compare_int(subject, b, a) > 0);
    TEST_ASSERT_EQUAL(0, lv_subject_compare_int(subject, a, a));

    a.color = lv_color_hex(0x000010);
    b.color = lv_color_hex(0xff0000);
    TEST_ASSERT_TRUE(lv_subject_compare_color(subject, a, b) < 0);

#if LV_USE_FLOAT
    a.float_v = 1.5f;
    b.float_v = 2.5f;
    TEST_ASSERT_TRUE(lv_subject_compare_float(subject, a, b) < 0);
#endif

    TEST_ASSERT_EQUAL(5, lv_subject_clamp_int(50, 0, 5));
    TEST_ASSERT_EQUAL(0, lv_subject_clamp_int(-50, 0, 5));
    TEST_ASSERT_EQUAL(3, lv_subject_clamp_int(3, 0, 5));
}

/* The clamp helper takes a compare callback, so it works on a type it could not
 * possibly order by itself. */
void test_subject_create_clamped_with_compare_callback(void)
{
    static const item_t cheap = { .name = "cheap", .price = 10 };
    static const item_t mid   = { .name = "mid",   .price = 50 };
    static const item_t dear  = { .name = "dear",  .price = 90 };

    lv_subject_t * offered = subject_create(LV_SUBJECT_TYPE_POINTER);
    lv_subject_set_pointer(offered, (void *)&mid);

    lv_subject_value_t lo;
    lv_subject_value_t hi;
    lv_memzero(&lo, sizeof(lo));
    lv_memzero(&hi, sizeof(hi));
    lo.pointer = &cheap;
    hi.pointer = &dear;

    lv_subject_t * bounded = lv_subject_create_clamped(offered, lo, hi, cheaper_cb);
    TEST_ASSERT_NOT_NULL(bounded);
    TEST_ASSERT_EQUAL_STRING("mid", ((const item_t *)lv_subject_get_pointer(bounded))->name);

    /* Something cheaper than the floor reports the floor. */
    static const item_t free_item = { .name = "free", .price = 0 };
    lv_subject_set_pointer(offered, (void *)&free_item);
    TEST_ASSERT_EQUAL_STRING("cheap", ((const item_t *)lv_subject_get_pointer(bounded))->name);

    /* Something dearer than the ceiling reports the ceiling. */
    static const item_t gold = { .name = "gold", .price = 999 };
    lv_subject_set_pointer(offered, (void *)&gold);
    TEST_ASSERT_EQUAL_STRING("dear", ((const item_t *)lv_subject_get_pointer(bounded))->name);

    lv_subject_delete(bounded);
}

void test_subject_create_clamped_needs_a_compare_for_pointers(void)
{
    lv_subject_t * ptr = subject_create(LV_SUBJECT_TYPE_POINTER);
    lv_subject_value_t zero;
    lv_memzero(&zero, sizeof(zero));
    TEST_ASSERT_NULL(lv_subject_create_clamped(ptr, zero, zero, NULL));
}





/*=====================================================================
 * Ownership is decided per write
 *====================================================================*/

static uint32_t freed_cnt;
static void * last_freed;

static void counting_free_cb(void * value)
{
    freed_cnt++;
    last_freed = value;
    lv_free(value);
}

void test_subject_owned_value_is_freed_when_replaced(void)
{
    lv_subject_t * frame = subject_create(LV_SUBJECT_TYPE_POINTER);

    freed_cnt = 0;
    void * first = lv_malloc(16);
    lv_subject_set_pointer_owned(frame, first, counting_free_cb);
    TEST_ASSERT_TRUE(lv_subject_is_value_owned(frame));
    TEST_ASSERT_EQUAL(0, freed_cnt);          /* nothing to replace yet */

    void * second = lv_malloc(16);
    lv_subject_set_pointer_owned(frame, second, counting_free_cb);
    TEST_ASSERT_EQUAL(1, freed_cnt);          /* the first one went */
    TEST_ASSERT_EQUAL_PTR(first, last_freed);
    TEST_ASSERT_EQUAL_PTR(second, lv_subject_get_pointer(frame));
}

void test_subject_owned_value_is_freed_with_the_subject(void)
{
    lv_subject_t * frame = subject_create(LV_SUBJECT_TYPE_POINTER);

    void * only = lv_malloc(16);
    lv_subject_set_pointer_owned(frame, only, counting_free_cb);

    freed_cnt = 0;
    subject_delete(frame);
    TEST_ASSERT_EQUAL(1, freed_cnt);
    TEST_ASSERT_EQUAL_PTR(only, last_freed);
}

/* A borrowed write is never freed, which is what makes it safe to publish a static. */
void test_subject_borrowed_value_is_never_freed(void)
{
    static int32_t a = 1;
    static int32_t b = 2;

    lv_subject_t * cfg = subject_create(LV_SUBJECT_TYPE_POINTER);

    freed_cnt = 0;
    lv_subject_set_pointer(cfg, &a);
    TEST_ASSERT_FALSE(lv_subject_is_value_owned(cfg));
    lv_subject_set_pointer(cfg, &b);
    lv_subject_set_pointer(cfg, &a);

    TEST_ASSERT_EQUAL(0, freed_cnt);
    TEST_ASSERT_EQUAL_PTR(&a, lv_subject_get_pointer(cfg));
}

/* Borrowed and owned writes can be mixed on the same Subject: each pointer is released,
 * or not, according to the write that introduced it. */
void test_subject_ownership_is_per_write(void)
{
    static int32_t borrowed = 1;
    lv_subject_t * mixed = subject_create(LV_SUBJECT_TYPE_POINTER);

    freed_cnt = 0;

    /* Borrowed, then replaced: not freed. */
    lv_subject_set_pointer(mixed, &borrowed);
    void * owned = lv_malloc(16);
    lv_subject_set_pointer_owned(mixed, owned, counting_free_cb);
    TEST_ASSERT_EQUAL(0, freed_cnt);
    TEST_ASSERT_TRUE(lv_subject_is_value_owned(mixed));

    /* Owned, then replaced by a borrowed one: the owned pointer is freed. */
    lv_subject_set_pointer(mixed, &borrowed);
    TEST_ASSERT_EQUAL(1, freed_cnt);
    TEST_ASSERT_EQUAL_PTR(owned, last_freed);
    TEST_ASSERT_FALSE(lv_subject_is_value_owned(mixed));

    /* And the borrowed one is still not freed at teardown. */
    freed_cnt = 0;
    subject_delete(mixed);
    TEST_ASSERT_EQUAL(0, freed_cnt);
}

/* Republishing the same pointer, which is what a driver does when it mutates its buffer
 * in place, must NOT release it: both the stored value and the retained input still
 * refer to it. */
void test_subject_owned_value_not_freed_when_republished(void)
{
    lv_subject_t * frame = subject_create(LV_SUBJECT_TYPE_POINTER);

    uint8_t * buffer = lv_malloc(16);
    buffer[0] = 1;
    lv_subject_set_pointer_owned(frame, buffer, counting_free_cb);

    freed_cnt = 0;
    buffer[0] = 2;
    lv_subject_set_pointer_owned(frame, buffer, counting_free_cb);
    buffer[0] = 3;
    lv_subject_set_pointer_owned(frame, buffer, counting_free_cb);
    lv_subject_set_pointer_owned(frame, buffer, counting_free_cb);

    /* Never released, so reading it is still valid. ASan would object otherwise. */
    TEST_ASSERT_EQUAL(0, freed_cnt);
    TEST_ASSERT_EQUAL_PTR(buffer, lv_subject_get_pointer(frame));
    TEST_ASSERT_EQUAL(3, buffer[0]);
}






/* Ownership follows what the write handed in. A mapper that publishes its own allocation
 * is responsible for it, and the Subject never releases it. */



/*=====================================================================
 * A string Subject can store a pointer instead of copying
 *====================================================================*/

void test_subject_string_borrowed_and_owned(void)
{
    lv_subject_t * text = subject_create(LV_SUBJECT_TYPE_STRING);
    /* No buffer, so this Subject stores the pointer rather than copying. */

    freed_cnt = 0;
    lv_subject_set_string(text, "literal");
    TEST_ASSERT_EQUAL_STRING("literal", lv_subject_get_string(text));
    TEST_ASSERT_FALSE(lv_subject_is_value_owned(text));

    char * heap = lv_malloc(8);
    lv_strlcpy(heap, "heap", 8);
    lv_subject_set_string_owned(text, heap, counting_free_cb);
    TEST_ASSERT_EQUAL_STRING("heap", lv_subject_get_string(text));
    TEST_ASSERT_TRUE(lv_subject_is_value_owned(text));
    /* The literal was borrowed, so nothing was released. */
    TEST_ASSERT_EQUAL(0, freed_cnt);

    /* Replacing the owned string releases it. */
    char * heap2 = lv_malloc(8);
    lv_strlcpy(heap2, "next", 8);
    lv_subject_set_string_owned(text, heap2, counting_free_cb);
    TEST_ASSERT_EQUAL(1, freed_cnt);
    TEST_ASSERT_EQUAL_PTR(heap, last_freed);
    TEST_ASSERT_EQUAL_STRING("next", lv_subject_get_string(text));
}

/* A string Subject that has a buffer copies, so the pointer setters are refused: one
 * Subject uses one form or the other. */
void test_subject_string_with_buffer_refuses_pointer_setters(void)
{
    static char buf[32];
    lv_subject_t * text = subject_create(LV_SUBJECT_TYPE_STRING);
    lv_subject_set_string_buffer_static(text, buf, sizeof(buf));
    lv_subject_copy_string(text, "copied");

    lv_subject_set_string(text, "ignored");
    TEST_ASSERT_EQUAL_STRING("copied", lv_subject_get_string(text));

    lv_subject_set_string_owned(text, NULL, NULL);
    TEST_ASSERT_EQUAL_STRING("copied", lv_subject_get_string(text));
}

/*=====================================================================
 * Detecting a mutation behind an unchanged pointer
 *====================================================================*/

/* A pointer Subject notifies on every set, because the data behind an unchanged pointer
 * may have changed. That is often too much. A mapper can do better by keeping a
 * last-known-good copy in its captured state and comparing against it, so the Subject
 * notifies only when the contents actually differ. */
typedef struct {
    int32_t x;
    int32_t y;
} sample_t;

typedef struct {
    sample_t last;      /**< last-known-good copy */
    bool seeded;
} sample_watch_t;






/* A Subject with no mapper has nothing to defer: the stored value is the input, so it
 * is written immediately even though notification still waits for the flush. */
void test_subject_without_mapper_stores_immediately(void)
{
    lv_subject_t * src = subject_create(LV_SUBJECT_TYPE_INT);

    lv_observer_t * observer = lv_subject_add_observer(src, observer_basic, NULL);
    lv_observer_set_mode(observer, LV_OBSERVER_MODE_BATCHED);

    observer_called = 0;
    lv_subject_set_int(src, 42);

    TEST_ASSERT_EQUAL(42, src->value.num);   /* stored */
    TEST_ASSERT_EQUAL(0, observer_called);   /* but not announced yet */

    lv_subject_flush();
    TEST_ASSERT_EQUAL(1, observer_called);
}


/*=====================================================================
 * Written as one type, observed as another
 *====================================================================*/

typedef enum {
    LEVEL_NORMAL = 0,
    LEVEL_HIGH = 1,
    LEVEL_CRITICAL = 2,
} level_t;

#if LV_USE_FLOAT



#endif /*LV_USE_FLOAT*/





/* The range is a retained pointer input, so it has to stay valid until a new range is
 * written. A static is the normal way to hold one. */
void test_subject_clamped_range_can_be_changed(void)
{
    lv_subject_t * reading = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int(reading, 500);

    lv_subject_value_t lo;
    lv_subject_value_t hi;
    lv_memzero(&lo, sizeof(lo));
    lv_memzero(&hi, sizeof(hi));
    lo.num = 0;
    hi.num = 10;
    lv_subject_t * bounded = lv_subject_create_clamped(reading, lo, hi, NULL);
    TEST_ASSERT_EQUAL(10, lv_subject_get_int(bounded));

    /* The bounds are the mapper's configuration now, not a value written to the
     * Subject, so they are set directly. */
    lv_subject_range_t range;
    lv_memzero(&range, sizeof(range));
    range.min_value.num = 0;
    range.max_value.num = 200;
    lv_subject_set_range(bounded, &range);
    TEST_ASSERT_EQUAL(200, lv_subject_get_int(bounded));

    /* Copied out, so the caller's struct may go away. */
    lv_subject_set_int(reading, 150);
    TEST_ASSERT_EQUAL(150, lv_subject_get_int(bounded));

    lv_subject_set_int(reading, 900);
    TEST_ASSERT_EQUAL(200, lv_subject_get_int(bounded));

    lv_subject_delete(bounded);
}


/*=====================================================================
 * Lifetime of a pointer value
 *====================================================================*/






/* A pointer input IS retained, so a dependency-driven re-run gets the same pointer the
 * last write carried. That is what lets a mapper re-derive from its input rather than
 * only from what it stored, and it is why the caller has to keep that pointer valid
 * until a new input is written. */





/*=====================================================================
 * Copying the value into a buffer
 *====================================================================*/

static uint32_t buf_freed_cnt;
static void * buf_last_freed;
static uint32_t realloc_calls;
static bool realloc_should_fail;

static void counting_buf_free(void * buf)
{
    buf_freed_cnt++;
    buf_last_freed = buf;
    lv_free(buf);
}

static void * counting_realloc(void * buf, size_t size)
{
    realloc_calls++;
    if(realloc_should_fail) return NULL;
    return lv_realloc(buf, size);
}

typedef struct {
    int32_t x;
    int32_t y;
} pt_t;

/* A fixed buffer copies, detects change on the bytes, and refuses what does not fit. */
void test_subject_copy_pointer_into_a_fixed_buffer(void)
{
    static uint8_t storage[sizeof(pt_t)];
    lv_subject_t * pts = subject_create(LV_SUBJECT_TYPE_POINTER);
    lv_subject_set_buffer(pts, storage, sizeof(storage), NULL, NULL);

    observer_called = 0;
    lv_subject_add_observer(pts, observer_basic, NULL);
    observer_called = 0;

    pt_t p = { .x = 1, .y = 2 };
    TEST_ASSERT_TRUE(lv_subject_copy_pointer(pts, &p, sizeof(p)));
    TEST_ASSERT_EQUAL(sizeof(p), lv_subject_get_pointer_size(pts));
    TEST_ASSERT_EQUAL(1, observer_called);

    /* The value is the buffer, holding a copy: mutating the source changes nothing. */
    const pt_t * stored = lv_subject_get_pointer(pts);
    TEST_ASSERT_EQUAL(1, stored->x);
    p.x = 99;
    TEST_ASSERT_EQUAL(1, stored->x);

    /* Same bytes: not a change, so nobody is notified. This is the mutation problem
     * solved for free, because the previous bytes were there to compare against. */
    p.x = 1;
    TEST_ASSERT_TRUE(lv_subject_copy_pointer(pts, &p, sizeof(p)));
    TEST_ASSERT_EQUAL(1, observer_called);

    /* Different bytes: a change. */
    p.y = 42;
    TEST_ASSERT_TRUE(lv_subject_copy_pointer(pts, &p, sizeof(p)));
    TEST_ASSERT_EQUAL(2, observer_called);
    TEST_ASSERT_EQUAL(42, ((const pt_t *)lv_subject_get_pointer(pts))->y);

    /* Too big for a fixed buffer: refused, previous value kept, nobody notified. */
    uint8_t big[sizeof(pt_t) + 8];
    lv_memzero(big, sizeof(big));
    TEST_ASSERT_FALSE(lv_subject_copy_pointer(pts, big, sizeof(big)));
    TEST_ASSERT_EQUAL(2, observer_called);
    TEST_ASSERT_EQUAL(sizeof(pt_t), lv_subject_get_pointer_size(pts));
    TEST_ASSERT_EQUAL(42, ((const pt_t *)lv_subject_get_pointer(pts))->y);
}

/* A growable buffer grows to fit. */
void test_subject_copy_pointer_grows_the_buffer(void)
{
    lv_subject_t * blob = subject_create(LV_SUBJECT_TYPE_POINTER);
    realloc_calls = 0;
    realloc_should_fail = false;
    buf_freed_cnt = 0;
    /* Start with nothing at all. */
    lv_subject_set_buffer(blob, NULL, 0, counting_realloc, counting_buf_free);

    uint8_t small[4] = { 1, 2, 3, 4 };
    TEST_ASSERT_TRUE(lv_subject_copy_pointer(blob, small, sizeof(small)));
    TEST_ASSERT_EQUAL(4, lv_subject_get_pointer_size(blob));
    TEST_ASSERT_EQUAL(1, realloc_calls);
    TEST_ASSERT_EQUAL(0, lv_memcmp(lv_subject_get_pointer(blob), small, sizeof(small)));

    uint8_t large[64];
    for(uint32_t i = 0; i < sizeof(large); i++) large[i] = (uint8_t)i;
    TEST_ASSERT_TRUE(lv_subject_copy_pointer(blob, large, sizeof(large)));
    TEST_ASSERT_EQUAL(64, lv_subject_get_pointer_size(blob));
    TEST_ASSERT_EQUAL(2, realloc_calls);
    TEST_ASSERT_EQUAL(0, lv_memcmp(lv_subject_get_pointer(blob), large, sizeof(large)));

    /* Shrinking back does not need to grow again. */
    TEST_ASSERT_TRUE(lv_subject_copy_pointer(blob, small, sizeof(small)));
    TEST_ASSERT_EQUAL(4, lv_subject_get_pointer_size(blob));
    TEST_ASSERT_EQUAL(2, realloc_calls);

    /* The buffer is released with the Subject. */
    buf_freed_cnt = 0;
    subject_delete(blob);
    TEST_ASSERT_EQUAL(1, buf_freed_cnt);
}

/* A failed grow refuses the write and keeps the previous value. */
void test_subject_copy_pointer_refuses_when_realloc_fails(void)
{
    lv_subject_t * blob = subject_create(LV_SUBJECT_TYPE_POINTER);
    realloc_should_fail = false;
    lv_subject_set_buffer(blob, NULL, 0, counting_realloc, counting_buf_free);

    uint8_t first[4] = { 9, 8, 7, 6 };
    TEST_ASSERT_TRUE(lv_subject_copy_pointer(blob, first, sizeof(first)));

    observer_called = 0;
    lv_subject_add_observer(blob, observer_basic, NULL);
    observer_called = 0;

    uint8_t big[128];
    lv_memzero(big, sizeof(big));
    realloc_should_fail = true;
    TEST_ASSERT_FALSE(lv_subject_copy_pointer(blob, big, sizeof(big)));
    realloc_should_fail = false;

    /* Nothing changed and nobody was notified. */
    TEST_ASSERT_EQUAL(0, observer_called);
    TEST_ASSERT_EQUAL(4, lv_subject_get_pointer_size(blob));
    TEST_ASSERT_EQUAL(0, lv_memcmp(lv_subject_get_pointer(blob), first, sizeof(first)));
}

/* Replacing the buffer releases the old one with its own free_cb. */
void test_subject_set_buffer_releases_the_previous_one(void)
{
    lv_subject_t * blob = subject_create(LV_SUBJECT_TYPE_POINTER);
    realloc_should_fail = false;
    lv_subject_set_buffer(blob, NULL, 0, counting_realloc, counting_buf_free);

    uint8_t data[4] = { 1, 1, 1, 1 };
    lv_subject_copy_pointer(blob, data, sizeof(data));

    buf_freed_cnt = 0;
    static uint8_t fixed[8];
    lv_subject_set_buffer(blob, fixed, sizeof(fixed), NULL, NULL);
    TEST_ASSERT_EQUAL(1, buf_freed_cnt);

    /* And a static buffer with no free_cb is not released at teardown. */
    buf_freed_cnt = 0;
    subject_delete(blob);
    TEST_ASSERT_EQUAL(0, buf_freed_cnt);
}

/* A growable string buffer grows instead of refusing. */
void test_subject_copy_string_grows_the_buffer(void)
{
    lv_subject_t * text = subject_create(LV_SUBJECT_TYPE_STRING);
    realloc_calls = 0;
    realloc_should_fail = false;
    lv_subject_set_buffer(text, NULL, 0, counting_realloc, counting_buf_free);

    TEST_ASSERT_TRUE(lv_subject_copy_string(text, "short"));
    TEST_ASSERT_EQUAL_STRING("short", lv_subject_get_string(text));

    TEST_ASSERT_TRUE(lv_subject_copy_string(text,
                                            "a considerably longer string than the first one"));
    TEST_ASSERT_EQUAL_STRING("a considerably longer string than the first one",
                             lv_subject_get_string(text));

    /* snprintf grows too, rather than truncating. */
    TEST_ASSERT_TRUE(lv_subject_snprintf(text, "%s/%s/%s",
                                         "one long component", "another long component",
                                         "a third long component"));
    TEST_ASSERT_EQUAL_STRING("one long component/another long component/a third long component",
                             lv_subject_get_string(text));

    buf_freed_cnt = 0;
    subject_delete(text);
    TEST_ASSERT_EQUAL(1, buf_freed_cnt);
}

/* Copying and referring are mutually exclusive on one Subject. */
void test_subject_copying_and_referring_are_exclusive(void)
{
    static uint8_t storage[8];
    lv_subject_t * pts = subject_create(LV_SUBJECT_TYPE_POINTER);

    /* No buffer yet, so copying is refused. */
    uint8_t data[4] = { 1, 2, 3, 4 };
    TEST_ASSERT_FALSE(lv_subject_copy_pointer(pts, data, sizeof(data)));

    lv_subject_set_buffer(pts, storage, sizeof(storage), NULL, NULL);
    TEST_ASSERT_TRUE(lv_subject_copy_pointer(pts, data, sizeof(data)));

    /* A string Subject with a buffer copies, so the pointer setters are refused. */
    static char text_buf[16];
    lv_subject_t * text = subject_create(LV_SUBJECT_TYPE_STRING);
    lv_subject_set_buffer(text, text_buf, sizeof(text_buf), NULL, NULL);
    lv_subject_copy_string(text, "copied");
    lv_subject_set_string(text, "ignored");
    TEST_ASSERT_EQUAL_STRING("copied", lv_subject_get_string(text));
}


/* The trimming variant stores as much as fits instead of refusing, which is what most
 * on-screen text wants. */
void test_subject_copy_string_trimmed_into_a_fixed_buffer(void)
{
    static char small[8];   /* 7 characters plus the terminator */
    lv_subject_t * text = subject_create(LV_SUBJECT_TYPE_STRING);
    lv_subject_set_buffer(text, small, sizeof(small), NULL, NULL);

    observer_called = 0;
    lv_subject_add_observer(text, observer_basic, NULL);
    observer_called = 0;

    /* Fits whole. */
    TEST_ASSERT_TRUE(lv_subject_copy_string_trimmed(text, "short"));
    TEST_ASSERT_EQUAL_STRING("short", lv_subject_get_string(text));
    TEST_ASSERT_EQUAL(1, observer_called);

    /* Does not fit: trimmed rather than refused, and it counts as a change. */
    TEST_ASSERT_TRUE(lv_subject_copy_string_trimmed(text, "a much longer label"));
    TEST_ASSERT_EQUAL_STRING("a much ", lv_subject_get_string(text));
    TEST_ASSERT_EQUAL(2, observer_called);

    /* A different string that trims to the same text is NOT a change: the comparison is
     * on what was stored, not on what was offered. */
    TEST_ASSERT_TRUE(lv_subject_copy_string_trimmed(text, "a much different label"));
    TEST_ASSERT_EQUAL_STRING("a much ", lv_subject_get_string(text));
    TEST_ASSERT_EQUAL(2, observer_called);

    /* Where the strict variant would refuse the same write outright. */
    TEST_ASSERT_FALSE(lv_subject_copy_string(text, "another long label"));
    TEST_ASSERT_EQUAL_STRING("a much ", lv_subject_get_string(text));
    TEST_ASSERT_EQUAL(2, observer_called);
}

/* With a growable buffer it grows first, so nothing is trimmed. */
void test_subject_copy_string_trimmed_grows_when_it_can(void)
{
    lv_subject_t * text = subject_create(LV_SUBJECT_TYPE_STRING);
    realloc_should_fail = false;
    realloc_calls = 0;
    lv_subject_set_buffer(text, NULL, 0, counting_realloc, counting_buf_free);

    TEST_ASSERT_TRUE(lv_subject_copy_string_trimmed(text, "a string of some length"));
    TEST_ASSERT_EQUAL_STRING("a string of some length", lv_subject_get_string(text));
    TEST_ASSERT_EQUAL(1, realloc_calls);

    /* If growing fails it falls back to trimming into what it already has: the buffer
     * holds 23 characters plus the terminator, so that is what is kept. */
    static const char * too_long = "a considerably longer string that will not fit at all";
    realloc_should_fail = true;
    TEST_ASSERT_TRUE(lv_subject_copy_string_trimmed(text, too_long));
    realloc_should_fail = false;

    const char * stored = lv_subject_get_string(text);
    size_t fits = lv_strlen("a string of some length");   /* the capacity, minus the terminator */
    TEST_ASSERT_EQUAL(fits, lv_strlen(stored));
    TEST_ASSERT_EQUAL(0, lv_strncmp(stored, too_long, fits));

    buf_freed_cnt = 0;
    subject_delete(text);
    TEST_ASSERT_EQUAL(1, buf_freed_cnt);
}

void test_subject_copy_string_trimmed_needs_a_buffer(void)
{
    lv_subject_t * text = subject_create(LV_SUBJECT_TYPE_STRING);
    TEST_ASSERT_FALSE(lv_subject_copy_string_trimmed(text, "no buffer"));
}


/*=====================================================================
 * Forwarding a Subject straight to a Widget setter
 *====================================================================*/

/* A setter that takes more than (obj, value) does not fit lv_obj_bind_int(). The macro
 * generates the bridging callback, type-checked and with no run-time cost. */
LV_SUBJECT_FORWARD_INT(forward_to_slider, lv_slider_set_value, LV_ANIM_OFF)
LV_SUBJECT_FORWARD_INT(forward_pad, lv_obj_set_style_pad_all, LV_PART_MAIN)
LV_SUBJECT_FORWARD_COLOR(forward_border_color, lv_obj_set_style_border_color, LV_PART_MAIN)

void test_subject_forward_macro_handles_extra_arguments(void)
{
    lv_subject_t * value = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int(value, 30);

    lv_obj_t * slider = lv_slider_create(lv_screen_active());
    lv_subject_add_observer_obj(value, forward_to_slider, slider, NULL);

    /* Applied on subscribing, through a setter that needs an animation flag. */
    TEST_ASSERT_EQUAL(30, lv_slider_get_value(slider));

    lv_subject_set_int(value, 70);
    TEST_ASSERT_EQUAL(70, lv_slider_get_value(slider));
}

void test_subject_forward_macro_to_a_style_setter(void)
{
    lv_subject_t * pad = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int(pad, 4);

    lv_obj_t * box = lv_obj_create(lv_screen_active());
    lv_subject_add_observer_obj(pad, forward_pad, box, NULL);
    TEST_ASSERT_EQUAL(4, lv_obj_get_style_pad_top(box, LV_PART_MAIN));

    lv_subject_set_int(pad, 12);
    TEST_ASSERT_EQUAL(12, lv_obj_get_style_pad_top(box, LV_PART_MAIN));

    lv_subject_t * col = subject_create(LV_SUBJECT_TYPE_COLOR);
    lv_subject_set_color(col, lv_color_hex(0x112233));
    lv_subject_add_observer_obj(col, forward_border_color, box, NULL);
    TEST_ASSERT_EQUAL_UINT32(lv_color_to_u32(lv_color_hex(0x112233)),
                             lv_color_to_u32(lv_obj_get_style_border_color(box, LV_PART_MAIN)));

    lv_subject_set_color(col, lv_color_hex(0x445566));
    TEST_ASSERT_EQUAL_UINT32(lv_color_to_u32(lv_color_hex(0x445566)),
                             lv_color_to_u32(lv_obj_get_style_border_color(box, LV_PART_MAIN)));
}

/* For the style families the dedicated binds are a single expression, with no forwarder
 * to declare. */
void test_subject_bind_style_int(void)
{
    lv_subject_t * pad = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int(pad, 6);

    lv_obj_t * box = lv_obj_create(lv_screen_active());
    TEST_ASSERT_NOT_NULL(lv_obj_bind_style_int(box, pad, lv_obj_set_style_pad_all, LV_PART_MAIN));
    TEST_ASSERT_EQUAL(6, lv_obj_get_style_pad_top(box, LV_PART_MAIN));

    lv_subject_set_int(pad, 20);
    TEST_ASSERT_EQUAL(20, lv_obj_get_style_pad_top(box, LV_PART_MAIN));

    /* The selector is honoured, so a second binding on another part is independent. */
    lv_subject_t * pad2 = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int(pad2, 3);
    lv_obj_bind_style_int(box, pad2, lv_obj_set_style_pad_all, LV_PART_SCROLLBAR);
    TEST_ASSERT_EQUAL(3, lv_obj_get_style_pad_top(box, LV_PART_SCROLLBAR));
    TEST_ASSERT_EQUAL(20, lv_obj_get_style_pad_top(box, LV_PART_MAIN));
}

void test_subject_bind_style_color_and_opa(void)
{
    lv_subject_t * col = subject_create(LV_SUBJECT_TYPE_COLOR);
    lv_subject_set_color(col, lv_color_hex(0xaabbcc));

    lv_obj_t * box = lv_obj_create(lv_screen_active());
    lv_obj_bind_style_color(box, col, lv_obj_set_style_bg_color, LV_PART_MAIN);
    TEST_ASSERT_EQUAL_UINT32(lv_color_to_u32(lv_color_hex(0xaabbcc)),
                             lv_color_to_u32(lv_obj_get_style_bg_color(box, LV_PART_MAIN)));

    lv_subject_t * opa = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int(opa, 128);
    lv_obj_bind_style_opa(box, opa, lv_obj_set_style_bg_opa, LV_PART_MAIN);
    TEST_ASSERT_EQUAL(128, lv_obj_get_style_bg_opa(box, LV_PART_MAIN));

    /* An opacity is a byte, so an out-of-range Subject value is bounded, not truncated. */
    lv_subject_set_int(opa, 400);
    TEST_ASSERT_EQUAL(255, lv_obj_get_style_bg_opa(box, LV_PART_MAIN));

    lv_subject_set_int(opa, -20);
    TEST_ASSERT_EQUAL(0, lv_obj_get_style_bg_opa(box, LV_PART_MAIN));
}

/* And the shape from the original request already worked: a setter that takes exactly
 * (obj, value) needs no bridging at all. */
void test_subject_bind_string_to_label_directly(void)
{
    static char buf[32];
    lv_subject_t * text = subject_create(LV_SUBJECT_TYPE_STRING);
    lv_subject_set_buffer(text, buf, sizeof(buf), NULL, NULL);
    lv_subject_copy_string(text, "hello");

    lv_obj_t * label = lv_label_create(lv_screen_active());
    lv_obj_bind_string(label, text, lv_label_set_text);
    TEST_ASSERT_EQUAL_STRING("hello", lv_label_get_text(label));

    lv_subject_copy_string(text, "world");
    TEST_ASSERT_EQUAL_STRING("world", lv_label_get_text(label));
}


/*=====================================================================
 * An accumulating mapper needs an eager Subject
 *====================================================================*/

static lv_subject_t * coin;

/* Accumulates, so it is not pure: the answer depends on how many times it ran. */
static bool accumulate_mapper(lv_subject_t * subject, void * user_data, int32_t * value)
{
    LV_UNUSED(subject);
    LV_UNUSED(user_data);
    mapper_runs++;
    *value += lv_subject_get_int(coin);
    return true;
}

void test_subject_accumulating_mapper_needs_eager(void)
{
    static const int32_t coins[] = { 10, 20, 50, 10, 5 };   /* 95 in total */

    coin = subject_create(LV_SUBJECT_TYPE_INT);

    lv_subject_t * total_lazy = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int_mapper(total_lazy, accumulate_mapper, NULL);

    lv_subject_t * total_eager = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int_mapper(total_eager, accumulate_mapper, NULL);
    lv_subject_set_mode(total_eager, LV_SUBJECT_MODE_EAGER);

    /* Attaching a mapper evaluates once, so start both from a known zero. */
    lv_subject_set_int(coin, 0);
    (void)lv_subject_get_int(total_lazy);
    total_lazy->value.num = 0;
    total_eager->value.num = 0;

    mapper_runs = 0;
    for(uint32_t i = 0; i < LV_ARRAYLEN(coins); i++) {
        lv_subject_set_int(coin, coins[i]);
    }

    /* The eager one ran once per coin and has the real total. */
    TEST_ASSERT_EQUAL(95, lv_subject_get_int(total_eager));

    /* The lazy one has not run at all yet: it is only marked stale. */
    TEST_ASSERT_TRUE(lv_subject_is_dirty(total_lazy));

    /* Reading it runs its mapper once, on the last coin only, so it under-counts. This
     * is the mapper breaking the purity contract, not a bug in the Subject. */
    TEST_ASSERT_EQUAL(5, lv_subject_get_int(total_lazy));
    TEST_ASSERT_EQUAL(5, coins[LV_ARRAYLEN(coins) - 1]);

    /* Reading again does not add anything, because nothing is stale any more — the same
     * mapper gives a different answer depending on when it runs, which is exactly what
     * "must be pure" rules out. */
    TEST_ASSERT_EQUAL(5, lv_subject_get_int(total_lazy));
}


/*=====================================================================
 * "I need the previous value" is a stateful mapper, not missing API
 *====================================================================*/

/* An Observer that needs to know what the value *was* — to animate away what was there
 * before, say — does not need the Subject to keep history. The mapper is handed the
 * previous stored value in `*value`, and its `user_data` is a place to publish it. */
typedef struct {
    int32_t previous;
    int32_t current;
    bool seeded;
} transition_t;





/*=====================================================================
 * A value that settles back costs one evaluation, not one per level
 *====================================================================*/

static lv_subject_t * sc_level1;
static uint32_t sc_level1_runs;
static uint32_t sc_level2_runs;

/* Deliberately lossy, so two different inputs give the same output. */
static bool sc_tenths_mapper(lv_subject_t * subject, void * user_data, int32_t * value)
{
    LV_UNUSED(subject);
    LV_UNUSED(user_data);
    sc_level1_runs++;

    int32_t next = lv_subject_get_int(dep_a) / 10;
    if(next == *value) return false;   /* no change, so no version bump */
    *value = next;
    return true;
}

static bool sc_double_mapper(lv_subject_t * subject, void * user_data, int32_t * value)
{
    LV_UNUSED(subject);
    LV_UNUSED(user_data);
    sc_level2_runs++;

    int32_t next = lv_subject_get_int(sc_level1) * 2;
    if(next == *value) return false;
    *value = next;
    return true;
}

void test_subject_no_change_stops_the_propagation(void)
{
    dep_a = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int(dep_a, 10);

    sc_level1 = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int_mapper(sc_level1, sc_tenths_mapper, NULL);

    lv_subject_t * level2 = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int_mapper(level2, sc_double_mapper, NULL);
    lv_subject_set_mode(level2, LV_SUBJECT_MODE_EAGER);

    TEST_ASSERT_EQUAL(1, lv_subject_get_int(sc_level1));
    TEST_ASSERT_EQUAL(2, lv_subject_get_int(level2));

    /* 11 / 10 is still 1, so level1 reports no change. */
    sc_level1_runs = 0;
    sc_level2_runs = 0;
    lv_subject_set_int(dep_a, 11);

    /* level1 had to run to find that out. */
    TEST_ASSERT_EQUAL(1, sc_level1_runs);
    /* level2 did NOT: nothing it reads changed version, so its mapper was skipped
     * rather than run to discover the same answer. */
    TEST_ASSERT_EQUAL(0, sc_level2_runs);
    TEST_ASSERT_EQUAL(2, lv_subject_get_int(level2));

    /* A real change still propagates all the way. */
    sc_level1_runs = 0;
    sc_level2_runs = 0;
    lv_subject_set_int(dep_a, 90);
    TEST_ASSERT_EQUAL(1, sc_level1_runs);
    TEST_ASSERT_EQUAL(1, sc_level2_runs);
    TEST_ASSERT_EQUAL(9, lv_subject_get_int(sc_level1));
    TEST_ASSERT_EQUAL(18, lv_subject_get_int(level2));
}



/*=====================================================================
 * The bind families that were not yet exercised
 *====================================================================*/

static int32_t captured_tag;
static const char * captured_str;
static const void * captured_ptr;
static lv_color_t captured_col;

/* Setters with an extra argument, which is what the FORWARD macros exist to bridge. */
static void capture_string_tagged(lv_obj_t * obj, const char * v, int32_t tag)
{
    LV_UNUSED(obj);
    captured_str = v;
    captured_tag = tag;
}

static void capture_pointer_tagged(lv_obj_t * obj, const void * v, int32_t tag)
{
    LV_UNUSED(obj);
    captured_ptr = v;
    captured_tag = tag;
}

LV_SUBJECT_FORWARD_STRING(forward_str_tagged, capture_string_tagged, 7)
LV_SUBJECT_FORWARD_POINTER(forward_ptr_tagged, capture_pointer_tagged, 9)

void test_subject_forward_string_and_pointer_macros(void)
{
    static char buf[16];
    lv_subject_t * text = subject_create(LV_SUBJECT_TYPE_STRING);
    lv_subject_set_buffer(text, buf, sizeof(buf), NULL, NULL);
    lv_subject_copy_string(text, "hi");

    lv_obj_t * obj = lv_obj_create(lv_screen_active());
    captured_str = NULL;
    captured_tag = 0;
    lv_subject_add_observer_obj(text, forward_str_tagged, obj, NULL);
    TEST_ASSERT_EQUAL_STRING("hi", captured_str);
    TEST_ASSERT_EQUAL(7, captured_tag);

    lv_subject_copy_string(text, "there");
    TEST_ASSERT_EQUAL_STRING("there", captured_str);

    static int32_t payload = 3;
    lv_subject_t * ptr = subject_create(LV_SUBJECT_TYPE_POINTER);
    captured_ptr = NULL;
    lv_subject_set_pointer(ptr, &payload);
    lv_subject_add_observer_obj(ptr, forward_ptr_tagged, obj, NULL);
    TEST_ASSERT_EQUAL_PTR(&payload, captured_ptr);
    TEST_ASSERT_EQUAL(9, captured_tag);
}

#if LV_USE_FLOAT
static float captured_flt;

static void capture_float_tagged(lv_obj_t * obj, float v, int32_t tag)
{
    LV_UNUSED(obj);
    captured_flt = v;
    captured_tag = tag;
}

LV_SUBJECT_FORWARD_FLOAT(forward_flt_tagged, capture_float_tagged, 11)

void test_subject_forward_float_macro(void)
{
    lv_subject_t * f = subject_create(LV_SUBJECT_TYPE_FLOAT);
    lv_subject_set_float(f, 1.5f);

    lv_obj_t * obj = lv_obj_create(lv_screen_active());
    captured_flt = 0.0f;
    lv_subject_add_observer_obj(f, forward_flt_tagged, obj, NULL);
    TEST_ASSERT_EQUAL_FLOAT(1.5f, captured_flt);
    TEST_ASSERT_EQUAL(11, captured_tag);

    lv_subject_set_float(f, -2.25f);
    TEST_ASSERT_EQUAL_FLOAT(-2.25f, captured_flt);
}

/* An int Subject driving a float setter, through a mapper. */
static bool int_to_float_mapper(lv_observer_t * observer, lv_subject_value_t input, float * out)
{
    LV_UNUSED(input);
    float next = (float)lv_subject_get_int(lv_observer_get_subject(observer)) / 4.0f;
    if(next == *out) return false;
    *out = next;
    return true;
}

static void capture_float(lv_obj_t * obj, float v)
{
    LV_UNUSED(obj);
    captured_flt = v;
}

void test_observer_bind_float_mapped(void)
{
    lv_subject_t * quarters = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int(quarters, 6);

    lv_obj_t * obj = lv_obj_create(lv_screen_active());
    captured_flt = 0.0f;
    TEST_ASSERT_NOT_NULL(lv_obj_bind_float_mapped(obj, quarters, capture_float, int_to_float_mapper, NULL));
    TEST_ASSERT_EQUAL_FLOAT(1.5f, captured_flt);

    lv_subject_set_int(quarters, 10);
    TEST_ASSERT_EQUAL_FLOAT(2.5f, captured_flt);
}

void test_subject_clamp_float_helper(void)
{
    TEST_ASSERT_EQUAL_FLOAT(1.0f, lv_subject_clamp_float(2.5f, 0.0f, 1.0f));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, lv_subject_clamp_float(-1.0f, 0.0f, 1.0f));
    TEST_ASSERT_EQUAL_FLOAT(0.5f, lv_subject_clamp_float(0.5f, 0.0f, 1.0f));
}

#endif /*LV_USE_FLOAT*/


/* An int Subject driving a colour setter through a mapper: a threshold turning red. */
static bool level_to_color(lv_observer_t * observer, lv_subject_value_t input, lv_color_t * out)
{
    LV_UNUSED(input);
    int32_t v = lv_subject_get_int(lv_observer_get_subject(observer));
    lv_color_t next = v > 50 ? lv_color_hex(0xff0000) : lv_color_hex(0x00ff00);
    if(lv_color_to_u32(next) == lv_color_to_u32(*out)) return false;
    *out = next;
    return true;
}

static void capture_color(lv_obj_t * obj, lv_color_t v)
{
    LV_UNUSED(obj);
    captured_col = v;
}

void test_observer_bind_color_mapped(void)
{
    lv_subject_t * level = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int(level, 10);

    lv_obj_t * obj = lv_obj_create(lv_screen_active());
    lv_obj_bind_color_mapped(obj, level, capture_color, level_to_color, NULL);
    TEST_ASSERT_EQUAL_UINT32(lv_color_to_u32(lv_color_hex(0x00ff00)), lv_color_to_u32(captured_col));

    lv_subject_set_int(level, 90);
    TEST_ASSERT_EQUAL_UINT32(lv_color_to_u32(lv_color_hex(0xff0000)), lv_color_to_u32(captured_col));
}

static const char * const choice_options[] = { "off", "on" };

static bool int_to_choice(lv_observer_t * observer, lv_subject_value_t input, const void ** out)
{
    LV_UNUSED(input);
    const void * next = choice_options[lv_subject_get_int(lv_observer_get_subject(observer)) ? 1 : 0];
    if(next == *out) return false;
    *out = next;
    return true;
}

static void capture_pointer(lv_obj_t * obj, const void * v)
{
    LV_UNUSED(obj);
    captured_ptr = v;
}

void test_observer_bind_pointer_mapped(void)
{
    lv_subject_t * flag = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int(flag, 0);

    lv_obj_t * obj = lv_obj_create(lv_screen_active());
    lv_obj_bind_pointer_mapped(obj, flag, capture_pointer, int_to_choice, NULL);
    TEST_ASSERT_EQUAL_STRING("off", (const char *)captured_ptr);

    lv_subject_set_int(flag, 1);
    TEST_ASSERT_EQUAL_STRING("on", (const char *)captured_ptr);
}

/*=====================================================================
 * Mapped style bindings
 *====================================================================*/

/* A float Subject driving an integer style property. */
static bool tenths_to_pad(lv_observer_t * observer, lv_subject_value_t input, int32_t * out)
{
    LV_UNUSED(input);
    int32_t next = lv_subject_get_int(lv_observer_get_subject(observer)) / 10;
    if(next == *out) return false;
    *out = next;
    return true;
}

void test_observer_bind_style_int_mapped(void)
{
    lv_subject_t * tenths = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int(tenths, 120);

    lv_obj_t * box = lv_obj_create(lv_screen_active());
    TEST_ASSERT_NOT_NULL(lv_obj_bind_style_int_mapped(box, tenths, lv_obj_set_style_pad_all,
                                                      LV_PART_MAIN, tenths_to_pad, NULL));
    TEST_ASSERT_EQUAL(12, lv_obj_get_style_pad_top(box, LV_PART_MAIN));

    lv_subject_set_int(tenths, 300);
    TEST_ASSERT_EQUAL(30, lv_obj_get_style_pad_top(box, LV_PART_MAIN));

    /* The mapper's output does not change, so the style is not written again. */
    lv_subject_set_int(tenths, 305);
    TEST_ASSERT_EQUAL(30, lv_obj_get_style_pad_top(box, LV_PART_MAIN));
}

void test_observer_bind_style_color_mapped(void)
{
    lv_subject_t * level = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int(level, 10);

    lv_obj_t * box = lv_obj_create(lv_screen_active());
    lv_obj_bind_style_color_mapped(box, level, lv_obj_set_style_bg_color, LV_PART_MAIN,
                                   level_to_color, NULL);
    TEST_ASSERT_EQUAL_UINT32(lv_color_to_u32(lv_color_hex(0x00ff00)),
                             lv_color_to_u32(lv_obj_get_style_bg_color(box, LV_PART_MAIN)));

    lv_subject_set_int(level, 80);
    TEST_ASSERT_EQUAL_UINT32(lv_color_to_u32(lv_color_hex(0xff0000)),
                             lv_color_to_u32(lv_obj_get_style_bg_color(box, LV_PART_MAIN)));
}

/* Maps a 0..100 percentage onto a 0..255 opacity, and the result is still bounded. */
static bool percent_to_opa(lv_observer_t * observer, lv_subject_value_t input, int32_t * out)
{
    LV_UNUSED(input);
    int32_t next = lv_subject_get_int(lv_observer_get_subject(observer)) * 255 / 100;
    if(next == *out) return false;
    *out = next;
    return true;
}

void test_observer_bind_style_opa_mapped(void)
{
    lv_subject_t * percent = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int(percent, 50);

    lv_obj_t * box = lv_obj_create(lv_screen_active());
    lv_obj_bind_style_opa_mapped(box, percent, lv_obj_set_style_bg_opa, LV_PART_MAIN,
                                 percent_to_opa, NULL);
    TEST_ASSERT_EQUAL(127, lv_obj_get_style_bg_opa(box, LV_PART_MAIN));

    lv_subject_set_int(percent, 100);
    TEST_ASSERT_EQUAL(255, lv_obj_get_style_bg_opa(box, LV_PART_MAIN));

    /* Beyond 100% the mapper overshoots, and the bind bounds it rather than truncating. */
    lv_subject_set_int(percent, 200);
    TEST_ASSERT_EQUAL(255, lv_obj_get_style_bg_opa(box, LV_PART_MAIN));
}

/*=====================================================================
 * Remaining public API
 *====================================================================*/

/* Writes the value into whatever the Observer's user data points at. An Observer has one
 * pointer for application data; a Widget, when there is one, is a separate lifetime link
 * rather than a second data slot. */
static void target_writing_cb(lv_observer_t * observer, lv_subject_t * subject)
{
    int32_t * sink = lv_observer_get_user_data(observer);
    if(sink) *sink = lv_subject_get_int(subject);
}

void test_subject_observer_user_data_carries_a_non_widget_target(void)
{
    static int32_t sink;
    sink = 0;

    lv_subject_t * v = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int(v, 3);

    lv_observer_t * observer = lv_subject_add_observer(v, target_writing_cb, &sink);
    TEST_ASSERT_NOT_NULL(observer);
    TEST_ASSERT_EQUAL_PTR(&sink, lv_observer_get_user_data(observer));
    /* Not bound to a Widget, so there is no Widget to report. */
    TEST_ASSERT_NULL(lv_observer_get_target_obj(observer));

    /* Applied on subscribing, and on every change after. */
    TEST_ASSERT_EQUAL(3, sink);
    lv_subject_set_int(v, 8);
    TEST_ASSERT_EQUAL(8, sink);

    lv_observer_delete(observer);
    lv_subject_set_int(v, 99);
    TEST_ASSERT_EQUAL(8, sink);   /* no longer subscribed */
}

/* A Widget-bound Observer keeps its own user data, so both are available at once —
 * which a single merged slot could not do. */
void test_subject_observer_has_both_a_widget_and_user_data(void)
{
    static int32_t sink;
    sink = 0;

    lv_subject_t * v = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int(v, 5);

    lv_obj_t * obj = lv_obj_create(lv_screen_active());
    lv_observer_t * observer = lv_subject_add_observer_obj(v, target_writing_cb, obj, &sink);

    TEST_ASSERT_EQUAL_PTR(obj, lv_observer_get_target_obj(observer));
    TEST_ASSERT_EQUAL_PTR(&sink, lv_observer_get_user_data(observer));
    TEST_ASSERT_EQUAL(5, sink);

    /* And the Widget is still the lifetime link: deleting it unsubscribes. */
    lv_obj_delete(obj);
    lv_subject_set_int(v, 42);
    TEST_ASSERT_EQUAL(5, sink);
}

/* lv_subject_notify() forces a notification without changing the value, which is what a
 * Subject whose pointed-to data changed behind its back needs. */
void test_subject_notify_forces_a_notification(void)
{
    lv_subject_t * v = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int(v, 5);

    observer_called = 0;
    lv_subject_add_observer(v, observer_basic, NULL);
    observer_called = 0;

    /* Writing the same value changes nothing, so nobody is told. */
    lv_subject_set_int(v, 5);
    TEST_ASSERT_EQUAL(0, observer_called);

    /* An explicit notify tells them anyway. */
    lv_subject_notify(v);
    TEST_ASSERT_EQUAL(1, observer_called);
    TEST_ASSERT_EQUAL(5, lv_subject_get_int(v));
}


/*=====================================================================
 * The subject-changing event helpers
 *
 * These were never covered, and this rearchitecture changed their behaviour: the
 * increment event used to intersect its own range with the Subject's min_value/max_value,
 * and those fields are gone, so the event's range is now the only one.
 *====================================================================*/

void test_subject_increment_event(void)
{
    lv_subject_t * v = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int(v, 5);

    lv_obj_t * btn = lv_obj_create(lv_screen_active());
    lv_obj_set_pos(btn, 0, 0);
    lv_obj_set_size(btn, 50, 50);

    lv_subject_increment_dsc_t * dsc = lv_obj_add_subject_increment_event(btn, v, LV_EVENT_CLICKED, 3);
    TEST_ASSERT_NOT_NULL(dsc);

    lv_obj_send_event(btn, LV_EVENT_CLICKED, NULL);
    TEST_ASSERT_EQUAL(8, lv_subject_get_int(v));

    lv_obj_send_event(btn, LV_EVENT_CLICKED, NULL);
    TEST_ASSERT_EQUAL(11, lv_subject_get_int(v));
}

void test_subject_increment_event_respects_its_own_range(void)
{
    lv_subject_t * v = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int(v, 8);

    lv_obj_t * btn = lv_obj_create(lv_screen_active());
    lv_subject_increment_dsc_t * dsc = lv_obj_add_subject_increment_event(btn, v, LV_EVENT_CLICKED, 5);
    lv_obj_set_subject_increment_event_min_value(btn, dsc, 0);
    lv_obj_set_subject_increment_event_max_value(btn, dsc, 10);

    /* Setting a maximum below the current value pulls it in straight away. */
    TEST_ASSERT_EQUAL(8, lv_subject_get_int(v));

    lv_obj_send_event(btn, LV_EVENT_CLICKED, NULL);
    TEST_ASSERT_EQUAL(10, lv_subject_get_int(v));   /* stops at the maximum */

    lv_obj_send_event(btn, LV_EVENT_CLICKED, NULL);
    TEST_ASSERT_EQUAL(10, lv_subject_get_int(v));   /* and stays there */
}

void test_subject_increment_event_rollover(void)
{
    lv_subject_t * v = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int(v, 2);

    lv_obj_t * btn = lv_obj_create(lv_screen_active());
    lv_subject_increment_dsc_t * dsc = lv_obj_add_subject_increment_event(btn, v, LV_EVENT_CLICKED, 1);
    lv_obj_set_subject_increment_event_min_value(btn, dsc, 0);
    lv_obj_set_subject_increment_event_max_value(btn, dsc, 3);
    lv_obj_set_subject_increment_event_rollover(btn, dsc, true);

    lv_obj_send_event(btn, LV_EVENT_CLICKED, NULL);
    TEST_ASSERT_EQUAL(3, lv_subject_get_int(v));

    /* Past the top it starts again from the bottom. */
    lv_obj_send_event(btn, LV_EVENT_CLICKED, NULL);
    TEST_ASSERT_EQUAL(0, lv_subject_get_int(v));
}



void test_subject_toggle_event(void)
{
    lv_subject_t * flag = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int(flag, 0);

    lv_obj_t * btn = lv_obj_create(lv_screen_active());
    lv_obj_add_subject_toggle_event(btn, flag, LV_EVENT_CLICKED);

    lv_obj_send_event(btn, LV_EVENT_CLICKED, NULL);
    TEST_ASSERT_EQUAL(1, lv_subject_get_int(flag));

    lv_obj_send_event(btn, LV_EVENT_CLICKED, NULL);
    TEST_ASSERT_EQUAL(0, lv_subject_get_int(flag));

    /* Any non-zero counts as set, so toggling from it gives zero. */
    lv_subject_set_int(flag, 7);
    lv_obj_send_event(btn, LV_EVENT_CLICKED, NULL);
    TEST_ASSERT_EQUAL(0, lv_subject_get_int(flag));
}

void test_subject_set_int_event(void)
{
    lv_subject_t * v = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int(v, 0);

    lv_obj_t * btn = lv_obj_create(lv_screen_active());
    lv_obj_add_subject_set_int_event(btn, v, LV_EVENT_CLICKED, 42);

    lv_obj_send_event(btn, LV_EVENT_CLICKED, NULL);
    TEST_ASSERT_EQUAL(42, lv_subject_get_int(v));

    /* Idempotent: the same value again is still 42 and notifies nobody. */
    observer_called = 0;
    lv_subject_add_observer(v, observer_basic, NULL);
    observer_called = 0;
    lv_obj_send_event(btn, LV_EVENT_CLICKED, NULL);
    TEST_ASSERT_EQUAL(42, lv_subject_get_int(v));
    TEST_ASSERT_EQUAL(0, observer_called);
}

#if LV_USE_FLOAT
void test_subject_set_float_event(void)
{
    lv_subject_t * v = subject_create(LV_SUBJECT_TYPE_FLOAT);
    lv_subject_set_float(v, 0.0f);

    lv_obj_t * btn = lv_obj_create(lv_screen_active());
    lv_obj_add_subject_set_float_event(btn, v, LV_EVENT_CLICKED, 2.5f);

    lv_obj_send_event(btn, LV_EVENT_CLICKED, NULL);
    TEST_ASSERT_EQUAL_FLOAT(2.5f, lv_subject_get_float(v));
}
#endif

void test_subject_set_string_event(void)
{
    static char buf[32];
    lv_subject_t * text = subject_create(LV_SUBJECT_TYPE_STRING);
    lv_subject_set_buffer(text, buf, sizeof(buf), NULL, NULL);
    lv_subject_copy_string(text, "before");

    lv_obj_t * btn = lv_obj_create(lv_screen_active());
    lv_obj_add_subject_set_string_event(btn, text, LV_EVENT_CLICKED, "after");

    lv_obj_send_event(btn, LV_EVENT_CLICKED, NULL);
    TEST_ASSERT_EQUAL_STRING("after", lv_subject_get_string(text));
}


/* Setting a minimum above the current value pulls it up straight away, which is the
 * mirror of the maximum case. */
void test_subject_increment_event_min_pulls_the_value_up(void)
{
    lv_subject_t * v = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int(v, 2);

    lv_obj_t * btn = lv_obj_create(lv_screen_active());
    lv_subject_increment_dsc_t * dsc = lv_obj_add_subject_increment_event(btn, v, LV_EVENT_CLICKED, 1);

    lv_obj_set_subject_increment_event_min_value(btn, dsc, 10);
    TEST_ASSERT_EQUAL(10, lv_subject_get_int(v));

    lv_obj_send_event(btn, LV_EVENT_CLICKED, NULL);
    TEST_ASSERT_EQUAL(11, lv_subject_get_int(v));
}

#if LV_USE_FLOAT
/* The increment event works on a float Subject too, with its own bounds and rollover. */
void test_subject_increment_event_on_a_float(void)
{
    lv_subject_t * v = subject_create(LV_SUBJECT_TYPE_FLOAT);
    lv_subject_set_float(v, 1.0f);

    lv_obj_t * btn = lv_obj_create(lv_screen_active());
    lv_subject_increment_dsc_t * dsc = lv_obj_add_subject_increment_event(btn, v, LV_EVENT_CLICKED, 2);
    TEST_ASSERT_NOT_NULL(dsc);

    lv_obj_send_event(btn, LV_EVENT_CLICKED, NULL);
    TEST_ASSERT_EQUAL_FLOAT(3.0f, lv_subject_get_float(v));

    lv_obj_set_subject_increment_event_max_value(btn, dsc, 4);
    lv_obj_send_event(btn, LV_EVENT_CLICKED, NULL);
    TEST_ASSERT_EQUAL_FLOAT(4.0f, lv_subject_get_float(v));   /* stops at the maximum */

    /* And a minimum above the value pulls it up, as for an integer. */
    lv_obj_set_subject_increment_event_min_value(btn, dsc, 6);
    TEST_ASSERT_EQUAL_FLOAT(6.0f, lv_subject_get_float(v));

    /* A max of 9 leaves room for one step, so the rollover is distinguishable from
     * simply not moving. */
    lv_obj_set_subject_increment_event_rollover(btn, dsc, true);
    lv_obj_set_subject_increment_event_max_value(btn, dsc, 9);

    lv_obj_send_event(btn, LV_EVENT_CLICKED, NULL);
    TEST_ASSERT_EQUAL_FLOAT(8.0f, lv_subject_get_float(v));   /* 6 + 2, still inside */

    lv_obj_send_event(btn, LV_EVENT_CLICKED, NULL);
    TEST_ASSERT_EQUAL_FLOAT(6.0f, lv_subject_get_float(v));   /* 10 > 9, so back to the minimum */
}
#endif


/*=====================================================================
 * Releasing what the application attached to a Subject
 *====================================================================*/

static uint32_t delete_cb_calls;
static lv_subject_t * delete_cb_saw_subject;
static void * delete_cb_saw_user_data;

static void recording_delete_cb(lv_subject_t * subject, void * user_data)
{
    delete_cb_calls++;
    delete_cb_saw_subject = subject;
    delete_cb_saw_user_data = user_data;
}

void test_subject_delete_cb_runs_on_delete(void)
{
    static int32_t token;
    lv_subject_t * v = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int(v, 4);
    lv_subject_set_delete_cb(v, recording_delete_cb, &token);

    delete_cb_calls = 0;
    delete_cb_saw_subject = NULL;
    delete_cb_saw_user_data = NULL;

    subject_delete(v);

    TEST_ASSERT_EQUAL(1, delete_cb_calls);
    TEST_ASSERT_EQUAL_PTR(v, delete_cb_saw_subject);
    TEST_ASSERT_EQUAL_PTR(&token, delete_cb_saw_user_data);
}

/* It runs before the Subject is taken apart, so the callback can still read it. */
static int32_t value_seen_at_delete;

static void reading_delete_cb(lv_subject_t * subject, void * user_data)
{
    LV_UNUSED(user_data);
    value_seen_at_delete = lv_subject_get_int(subject);
}

void test_subject_delete_cb_sees_an_intact_subject(void)
{
    lv_subject_t * v = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int(v, 77);
    lv_subject_set_delete_cb(v, reading_delete_cb, NULL);

    value_seen_at_delete = 0;
    subject_delete(v);
    TEST_ASSERT_EQUAL(77, value_seen_at_delete);
}

/* Cascade destroys several Subjects, and each one's callback has to run. */
void test_subject_delete_cb_runs_for_every_cascaded_subject(void)
{
    dep_a = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int(dep_a, 1);

    chain_l1 = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int_mapper(chain_l1, chain_l1_mapper, NULL);
    chain_l2 = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int_mapper(chain_l2, chain_l2_mapper, NULL);
    (void)lv_subject_get_int(chain_l2);

    lv_subject_set_delete_cb(dep_a, recording_delete_cb, NULL);
    lv_subject_set_delete_cb(chain_l1, recording_delete_cb, NULL);
    lv_subject_set_delete_cb(chain_l2, recording_delete_cb, NULL);

    subject_forget(dep_a);
    subject_forget(chain_l1);
    subject_forget(chain_l2);

    delete_cb_calls = 0;
    lv_subject_delete_cascade(dep_a);
    TEST_ASSERT_EQUAL(3, delete_cb_calls);
}

/* A refused delete must not run it: the Subject is still alive. */
void test_subject_delete_cb_not_run_when_delete_is_refused(void)
{
    dep_a = subject_create(LV_SUBJECT_TYPE_INT);
    dep_b = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int(dep_a, 1);
    lv_subject_set_int(dep_b, 1);

    lv_subject_t * sum = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int_mapper(sum, sum_mapper, NULL);
    (void)lv_subject_get_int(sum);

    lv_subject_set_delete_cb(dep_a, recording_delete_cb, NULL);

    delete_cb_calls = 0;
    lv_subject_delete(dep_a);          /* refused: `sum` reads it */
    TEST_ASSERT_EQUAL(0, delete_cb_calls);
    TEST_ASSERT_EQUAL(1, lv_subject_get_int(dep_a));
}

/* The one-call form for the common case: a mapper whose captured state is allocated. */
void test_subject_owned_mapper_user_data_is_freed(void)
{
    uint32_t mem_before = lv_test_get_free_mem();

    for(uint32_t i = 0; i < 32; i++) {
        lv_subject_t * left = lv_subject_create(LV_SUBJECT_TYPE_INT);
        lv_subject_t * right = lv_subject_create(LV_SUBJECT_TYPE_INT);
        lv_subject_set_int(left, (int32_t)i);
        lv_subject_set_int(right, 1);

        pair_t * cfg = lv_malloc(sizeof(pair_t));
        cfg->left = left;
        cfg->right = right;

        lv_subject_t * sum = lv_subject_create(LV_SUBJECT_TYPE_INT);
        lv_subject_set_int_mapper(sum, pair_sum_mapper, cfg);
        lv_subject_set_mapper_user_data_owned(sum);
        TEST_ASSERT_EQUAL_PTR(cfg, lv_subject_get_mapper_user_data(sum));
        TEST_ASSERT_EQUAL((int32_t)i + 1, lv_subject_get_int(sum));

        lv_subject_delete(sum);     /* releases cfg */
        lv_subject_delete(left);
        lv_subject_delete(right);
    }

    /* Nothing accumulates, so the allocation really is released each time. */
    TEST_ASSERT_MEM_LEAK_LESS_THAN(mem_before, 32);
}

void test_subject_mapper_user_data_owned_needs_a_mapper(void)
{
    lv_subject_t * v = subject_create(LV_SUBJECT_TYPE_INT);

    /* No mapper yet, so there is nothing to own: refused rather than silently armed. */
    lv_subject_set_mapper_user_data_owned(v);
    TEST_ASSERT_NULL(lv_subject_get_mapper_user_data(v));
}


/*=====================================================================
 * A mapper must write only its own output
 *
 * Several consumers are handed the same value. If one of them writes through it, the
 * others see the change, and what they see depends on the order they were added in.
 * These two tests pin that, because it is the reason for the rule rather than a
 * behaviour worth relying on.
 *====================================================================*/

typedef struct {
    int32_t n;
} box_t;

static int32_t second_observer_saw;

/* Badly behaved on purpose: it writes through `input`. */
static bool mutating_observer_mapper(lv_observer_t * observer, lv_subject_value_t input, int32_t * out)
{
    LV_UNUSED(observer);
    box_t * b = (box_t *)input.pointer;
    if(b == NULL) return false;
    b->n = 999;                  /* the mutation the rule forbids */
    *out = b->n;
    return true;
}

static bool reading_observer_mapper(lv_observer_t * observer, lv_subject_value_t input, int32_t * out)
{
    LV_UNUSED(observer);
    const box_t * b = input.pointer;
    if(b == NULL) return false;
    second_observer_saw = b->n;
    *out = b->n;
    return true;
}

static void ignore_int(lv_obj_t * obj, int32_t v)
{
    LV_UNUSED(obj);
    LV_UNUSED(v);
}

void test_observer_mapper_mutation_is_seen_by_later_observers(void)
{
    static box_t box;
    box.n = 1;

    lv_subject_t * subject = subject_create(LV_SUBJECT_TYPE_POINTER);
    lv_obj_t * a = lv_obj_create(lv_screen_active());
    lv_obj_t * b = lv_obj_create(lv_screen_active());

    /* Added first, so notified first. */
    lv_obj_bind_int_mapped(a, subject, ignore_int, mutating_observer_mapper, NULL);
    lv_obj_bind_int_mapped(b, subject, ignore_int, reading_observer_mapper, NULL);

    second_observer_saw = 0;
    lv_subject_set_pointer(subject, &box);

    /* The second Observer saw the first Observer's mutation, not the value that was
     * published. That is why a mapper may write only `*out`. */
    TEST_ASSERT_EQUAL(999, second_observer_saw);
    TEST_ASSERT_EQUAL(999, box.n);
}

/* The same hazard exists on the Subject side, so it is not an Observer-only rule: two
 * derived Subjects reading one pointer dependency are in exactly the same position.
 *
 * This asserts the *inconsistency* rather than a particular order. Observers are
 * notified in the order they were added, while dirty Subjects are evaluated from the
 * head of the global list and marking moves them there, so derived Subjects run in
 * roughly the reverse order. Pinning either direction would be over-fitting; what
 * matters is that two consumers of one value disagree about what it was. */
static lv_subject_t * shared_source;
static int32_t first_dependent_saw;
static int32_t second_dependent_saw;

static bool mutating_subject_mapper(lv_subject_t * subject, void * user_data, int32_t * value)
{
    LV_UNUSED(subject);
    LV_UNUSED(user_data);
    box_t * b = (box_t *)lv_subject_get_pointer(shared_source);
    if(b == NULL) return false;

    first_dependent_saw = b->n;
    b->n = 555;                  /* writing through a dependency: also forbidden */
    *value = b->n;
    return true;
}

static bool reading_subject_mapper(lv_subject_t * subject, void * user_data, int32_t * value)
{
    LV_UNUSED(subject);
    LV_UNUSED(user_data);
    const box_t * b = lv_subject_get_pointer(shared_source);
    if(b == NULL) return false;

    second_dependent_saw = b->n;
    *value = b->n;
    return true;
}

void test_subject_mapper_mutation_leaks_between_dependents(void)
{
    static box_t box;
    box.n = 1;

    shared_source = subject_create(LV_SUBJECT_TYPE_POINTER);

    lv_subject_t * first = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int_mapper(first, mutating_subject_mapper, NULL);
    lv_subject_set_mode(first, LV_SUBJECT_MODE_EAGER);

    lv_subject_t * second = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int_mapper(second, reading_subject_mapper, NULL);
    lv_subject_set_mode(second, LV_SUBJECT_MODE_EAGER);

    first_dependent_saw = 0;
    second_dependent_saw = 0;
    box.n = 1;
    lv_subject_set_pointer(shared_source, &box);

    /* A consumer changed the value its producer published. That much is unambiguous. */
    TEST_ASSERT_EQUAL(555, box.n);
    TEST_ASSERT_EQUAL(555, ((const box_t *)lv_subject_get_pointer(shared_source))->n);

    /* Whether a *sibling in the same round* sees it depends on evaluation order, which is
     * precisely why this must not be done. Here the reader happened to run first and saw
     * the published 1; anything evaluated afterwards sees 555 instead. */
    TEST_ASSERT_TRUE(first_dependent_saw == 1 || first_dependent_saw == 555);

    /* A lazy dependent added now, and read now, gets the corrupted value rather than
     * what was published — the leak outlives the round it happened in. */
    second_dependent_saw = 0;
    lv_subject_t * late = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int_mapper(late, reading_subject_mapper, NULL);
    TEST_ASSERT_EQUAL(555, lv_subject_get_int(late));
    TEST_ASSERT_EQUAL(555, second_dependent_saw);
}



/*---------------------------------------------------------------
 * Transactions and version stamps
 *--------------------------------------------------------------*/

/* w * h, so it is a diamond when both are written together. */
static lv_subject_t * txn_w;
static lv_subject_t * txn_h;

static bool txn_area_mapper(lv_subject_t * subject, void * user_data, int32_t * value)
{
    LV_UNUSED(subject);
    LV_UNUSED(user_data);
    int32_t before = *value;
    *value = lv_subject_get_int(txn_w) * lv_subject_get_int(txn_h);
    return *value != before;
}

static lv_subject_t * txn_build_area(void)
{
    txn_w = subject_create(LV_SUBJECT_TYPE_INT);
    txn_h = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int(txn_w, 4);
    lv_subject_set_int(txn_h, 5);

    lv_subject_t * area = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int_mapper(area, txn_area_mapper, NULL);
    return area;
}

/* Two writes that feed one dependent notify it once, not once per write. */
void test_subject_transaction_notifies_once(void)
{
    lv_subject_t * area = txn_build_area();
    lv_subject_add_observer(area, observer_basic, NULL);
    observer_called = 0;

    /* Written on their own, each write is its own transaction. */
    lv_subject_set_int(txn_w, 6);
    lv_subject_set_int(txn_h, 7);
    TEST_ASSERT_EQUAL(42, lv_subject_get_int(area));
    TEST_ASSERT_EQUAL(2, observer_called);

    /* Written together, the graph settles before anyone hears about it. */
    observer_called = 0;
    lv_subject_transaction_begin();
    lv_subject_set_int(txn_w, 8);
    lv_subject_set_int(txn_h, 9);
    lv_subject_transaction_commit();
    TEST_ASSERT_EQUAL(72, lv_subject_get_int(area));
    TEST_ASSERT_EQUAL(1, observer_called);
}

/* A read inside a transaction sees what was just written. This is what lets a chain of
 * setters work: each one has to see what the one before it wrote. */
void test_subject_transaction_reads_are_fresh(void)
{
    lv_subject_t * area = txn_build_area();
    TEST_ASSERT_EQUAL(20, lv_subject_get_int(area));

    lv_subject_transaction_begin();
    lv_subject_set_int(txn_w, 10);
    TEST_ASSERT_EQUAL(50, lv_subject_get_int(area));   /* not the old 20 */
    lv_subject_set_int(txn_h, 10);
    TEST_ASSERT_EQUAL(100, lv_subject_get_int(area));
    lv_subject_transaction_commit();
}

/* Nesting is a depth counter: only the outermost commit notifies. */
void test_subject_transaction_nests(void)
{
    lv_subject_t * area = txn_build_area();
    lv_subject_add_observer(area, observer_basic, NULL);
    observer_called = 0;

    lv_subject_transaction_begin();
    lv_subject_set_int(txn_w, 2);
    lv_subject_transaction_begin();
    lv_subject_set_int(txn_h, 3);
    lv_subject_transaction_commit();          /* inner: notifies nothing */
    TEST_ASSERT_EQUAL(0, observer_called);
    lv_subject_set_int(txn_w, 3);
    lv_subject_transaction_commit();          /* outer: one notification */

    TEST_ASSERT_EQUAL(9, lv_subject_get_int(area));
    TEST_ASSERT_EQUAL(1, observer_called);
}

/* Subjects written in one transaction share a stamp, and a value that does not change
 * does not move its stamp. */
void test_subject_changed_at(void)
{
    lv_subject_t * area = txn_build_area();
    TEST_ASSERT_EQUAL(20, lv_subject_get_int(area));

    /* Written separately, so the stamps differ. */
    TEST_ASSERT_NOT_EQUAL(lv_subject_get_changed_at(txn_w), lv_subject_get_changed_at(txn_h));

    /* A change downstream carries the stamp of the transaction that caused it. */
    lv_subject_set_int(txn_w, 6);
    TEST_ASSERT_EQUAL(30, lv_subject_get_int(area));
    TEST_ASSERT_EQUAL(lv_subject_get_changed_at(txn_w), lv_subject_get_changed_at(area));
    uint32_t area_stamp = lv_subject_get_changed_at(area);

    /* Both written together: same stamp, because they changed together. */
    lv_subject_transaction_begin();
    lv_subject_set_int(txn_w, 5);
    lv_subject_set_int(txn_h, 6);
    lv_subject_transaction_commit();
    TEST_ASSERT_EQUAL(lv_subject_get_changed_at(txn_w), lv_subject_get_changed_at(txn_h));

    /* 5 * 6 is still 30, so `area` did not change and its stamp did not move. */
    TEST_ASSERT_EQUAL(30, lv_subject_get_int(area));
    TEST_ASSERT_EQUAL(area_stamp, lv_subject_get_changed_at(area));

    /* And the dimensions are now newer than the area they feed. */
    TEST_ASSERT_TRUE((int32_t)(lv_subject_get_changed_at(txn_w) - area_stamp) > 0);
}

/* A write that changes nothing does not move the stamp either. */
void test_subject_changed_at_ignores_a_write_that_changes_nothing(void)
{
    lv_subject_t * n = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int(n, 7);
    uint32_t stamp = lv_subject_get_changed_at(n);

    lv_subject_set_int(n, 7);
    TEST_ASSERT_EQUAL(stamp, lv_subject_get_changed_at(n));

    lv_subject_set_int(n, 8);
    TEST_ASSERT_NOT_EQUAL(stamp, lv_subject_get_changed_at(n));
}

/*---------------------------------------------------------------
 * Setters: writing a computed Subject
 *--------------------------------------------------------------*/

static bool area_snap_setter(lv_subject_t * subject, void * user_data, int32_t value);

static lv_subject_t * chain_adc;
static lv_subject_t * chain_k;
static lv_subject_t * chain_c;
static uint32_t setter_fail_at_k;

/* k = adc + 100 */
static bool chain_k_mapper(lv_subject_t * subject, void * user_data, int32_t * value)
{
    LV_UNUSED(subject);
    LV_UNUSED(user_data);
    int32_t before = *value;
    *value = lv_subject_get_int(chain_adc) + 100;
    return *value != before;
}

static bool chain_k_setter(lv_subject_t * subject, void * user_data, int32_t value)
{
    LV_UNUSED(subject);
    LV_UNUSED(user_data);
    if(setter_fail_at_k) return false;
    lv_subject_set_int(chain_adc, value - 100);
    return true;
}

/* c = k - 273 */
static bool chain_c_mapper(lv_subject_t * subject, void * user_data, int32_t * value)
{
    LV_UNUSED(subject);
    LV_UNUSED(user_data);
    int32_t before = *value;
    *value = lv_subject_get_int(chain_k) - 273;
    return *value != before;
}

static bool chain_c_setter(lv_subject_t * subject, void * user_data, int32_t value)
{
    LV_UNUSED(subject);
    LV_UNUSED(user_data);
    /* Only inverts its own step. k's setter carries on from here. */
    lv_subject_set_int(chain_k, value + 273);
    return true;
}

static void chain_build(void)
{
    setter_fail_at_k = 0;

    chain_adc = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int(chain_adc, 200);

    chain_k = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int_mapper(chain_k, chain_k_mapper, NULL);
    lv_subject_set_int_setter(chain_k, chain_k_setter, NULL);

    chain_c = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int_mapper(chain_c, chain_c_mapper, NULL);
    lv_subject_set_int_setter(chain_c, chain_c_setter, NULL);
}

/* A computed Subject with no setter is read-only: its value belongs to its mapper. */
void test_subject_computed_without_setter_is_not_writable(void)
{
    lv_subject_t * area = txn_build_area();
    TEST_ASSERT_FALSE(lv_subject_is_writable(area));
    TEST_ASSERT_TRUE(lv_subject_is_writable(txn_w));

    /* Refused, with a warning, rather than silently ignored. */
    TEST_ASSERT_EQUAL(20, lv_subject_get_int(area));
    lv_subject_set_int(area, 999);
    TEST_ASSERT_EQUAL(20, lv_subject_get_int(area));
    TEST_ASSERT_EQUAL(4, lv_subject_get_int(txn_w));   /* nothing upstream moved either */

    /* A setter is what makes it writable. */
    lv_subject_set_int_setter(area, area_snap_setter, NULL);
    TEST_ASSERT_TRUE(lv_subject_is_writable(area));
    lv_subject_set_int(area, 30);
    TEST_ASSERT_EQUAL(6, lv_subject_get_int(txn_w));
}

/* Writing a computed Subject writes the Subjects its mapper reads. */
void test_subject_setter_writes_upstream(void)
{
    chain_build();
    TEST_ASSERT_EQUAL(300, lv_subject_get_int(chain_k));
    TEST_ASSERT_TRUE(lv_subject_is_writable(chain_k));

    lv_subject_set_int(chain_k, 350);
    TEST_ASSERT_EQUAL(250, lv_subject_get_int(chain_adc));
    TEST_ASSERT_EQUAL(350, lv_subject_get_int(chain_k));
}

/* Setters chain: each one inverts only its own step. */
void test_subject_setters_chain(void)
{
    chain_build();
    TEST_ASSERT_EQUAL(27, lv_subject_get_int(chain_c));

    /* c -> k -> adc, all the way to the root. */
    lv_subject_set_int(chain_c, 50);
    TEST_ASSERT_EQUAL(323, lv_subject_get_int(chain_k));
    TEST_ASSERT_EQUAL(223, lv_subject_get_int(chain_adc));
    TEST_ASSERT_EQUAL(50, lv_subject_get_int(chain_c));
}

/* A failing setter puts everything back, including what an earlier link already wrote. */
void test_subject_failing_setter_rolls_back(void)
{
    chain_build();
    TEST_ASSERT_EQUAL(27, lv_subject_get_int(chain_c));
    uint32_t adc_stamp = lv_subject_get_changed_at(chain_adc);

    setter_fail_at_k = 1;
    lv_subject_set_int(chain_c, 50);

    /* Nothing moved, and no stamp claims a change that was undone. */
    TEST_ASSERT_EQUAL(200, lv_subject_get_int(chain_adc));
    TEST_ASSERT_EQUAL(300, lv_subject_get_int(chain_k));
    TEST_ASSERT_EQUAL(27, lv_subject_get_int(chain_c));
    TEST_ASSERT_EQUAL(adc_stamp, lv_subject_get_changed_at(chain_adc));

    /* And the graph still works afterwards. */
    setter_fail_at_k = 0;
    lv_subject_set_int(chain_c, 50);
    TEST_ASSERT_EQUAL(223, lv_subject_get_int(chain_adc));
}

/* A rolled back transaction must notify nobody. Its writes queued notifications before
 * the refusal undid them, and the values are back as they were, so delivering those
 * would report a change that never happened. */
void test_subject_rollback_notifies_nobody(void)
{
    chain_build();

    /* Settle first, so nothing is left owed from the setup. */
    lv_subject_flush();
    lv_subject_add_observer(chain_adc, observer_basic, NULL);
    observer_called = 0;          /* adding one calls it once */

    setter_fail_at_k = 1;
    lv_subject_transaction_begin();
    lv_subject_set_int(chain_adc, 999);
    lv_subject_set_int(chain_c, 70);
    TEST_ASSERT_EQUAL(LV_RESULT_INVALID, lv_subject_transaction_commit());

    /* The value went back, so there is nothing to report — not now, and not at the
     * next flush either, which is where it used to surface as `200 -> 200`. */
    TEST_ASSERT_EQUAL(200, lv_subject_get_int(chain_adc));
    TEST_ASSERT_EQUAL(0, observer_called);
    lv_subject_flush();
    TEST_ASSERT_EQUAL(0, observer_called);

    /* A real change afterwards still gets through. */
    setter_fail_at_k = 0;
    lv_subject_set_int(chain_adc, 300);
    lv_subject_flush();
    TEST_ASSERT_EQUAL(1, observer_called);
}

/* The rollback clears only what it queued itself. A notification that was already owed
 * when the transaction opened is still owed after it is undone.
 *
 * A batched Observer is what makes the case reachable: it does not make its Subject
 * eager, so the write's notification waits for the next flush instead of being
 * delivered as the write returns. */
void test_subject_rollback_keeps_a_notification_already_owed(void)
{
    lv_subject_t * batched = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int(batched, 1);

    lv_observer_t * o = lv_subject_add_observer(batched, observer_basic, NULL);
    lv_observer_set_mode(o, LV_OBSERVER_MODE_BATCHED);
    lv_subject_flush();
    observer_called = 0;

    /* Owed but undelivered: the Subject is not eager, so nothing was notified yet. */
    lv_subject_set_int(batched, 2);
    TEST_ASSERT_EQUAL(0, observer_called);

    chain_build();
    setter_fail_at_k = 1;
    lv_subject_transaction_begin();
    lv_subject_set_int(batched, 3);        /* the transaction writes it too */
    lv_subject_set_int(chain_c, 70);       /* and then this is refused */
    TEST_ASSERT_EQUAL(LV_RESULT_INVALID, lv_subject_transaction_commit());
    setter_fail_at_k = 0;

    /* Back to 2, the value the outstanding notification belongs to — not to 1, and the
     * notification is still due. */
    TEST_ASSERT_EQUAL(2, lv_subject_get_int(batched));
    lv_subject_flush();
    TEST_ASSERT_EQUAL(1, observer_called);
}

/* An application cannot see a rollback in its Subjects — the values are as they were —
 * so the commit reports it. Without this there is no way to tell a transaction that
 * committed from one that was undone. */
void test_subject_transaction_commit_reports_a_rollback(void)
{
    chain_build();

    /* The ordinary case: it committed. */
    lv_subject_transaction_begin();
    lv_subject_set_int(chain_c, 50);
    TEST_ASSERT_EQUAL(LV_RESULT_OK, lv_subject_transaction_commit());
    TEST_ASSERT_EQUAL(223, lv_subject_get_int(chain_adc));

    /* Now with a setter that refuses. The refusal closes the transaction itself, so by
     * the time the caller commits there is nothing left to commit. */
    uint32_t adc_stamp = lv_subject_get_changed_at(chain_adc);
    setter_fail_at_k = 1;
    lv_subject_transaction_begin();
    lv_subject_set_int(chain_adc, 999);
    lv_subject_set_int(chain_c, 70);
    TEST_ASSERT_EQUAL(LV_RESULT_INVALID, lv_subject_transaction_commit());

    /* Both writes went back, the one before the refusal included. */
    TEST_ASSERT_EQUAL(223, lv_subject_get_int(chain_adc));
    TEST_ASSERT_EQUAL(adc_stamp, lv_subject_get_changed_at(chain_adc));

    /* The report describes the last transaction, not every one after it. */
    setter_fail_at_k = 0;
    lv_subject_transaction_begin();
    lv_subject_set_int(chain_c, 90);
    TEST_ASSERT_EQUAL(LV_RESULT_OK, lv_subject_transaction_commit());
}

/* Committing nothing is a misuse, and says so rather than reporting a rollback. */
void test_subject_transaction_commit_without_a_begin_is_invalid(void)
{
    TEST_ASSERT_EQUAL(LV_RESULT_INVALID, lv_subject_transaction_commit());
}

/* A nested commit reports its own level. Only the outermost one can see a rollback,
 * because a refusal anywhere closes the whole nest at once. */
void test_subject_transaction_nested_commit_reports_ok(void)
{
    chain_build();

    lv_subject_transaction_begin();
    lv_subject_transaction_begin();
    lv_subject_set_int(chain_c, 50);
    TEST_ASSERT_EQUAL(LV_RESULT_OK, lv_subject_transaction_commit());   /* inner */
    TEST_ASSERT_EQUAL(LV_RESULT_OK, lv_subject_transaction_commit());   /* outer */
    TEST_ASSERT_EQUAL(223, lv_subject_get_int(chain_adc));

    /* A refusal inside the nest takes the outer level with it, so the outer commit is
     * the one that finds nothing to do. */
    setter_fail_at_k = 1;
    lv_subject_transaction_begin();
    lv_subject_transaction_begin();
    lv_subject_set_int(chain_c, 70);
    TEST_ASSERT_EQUAL(LV_RESULT_INVALID, lv_subject_transaction_commit());
    TEST_ASSERT_EQUAL(LV_RESULT_INVALID, lv_subject_transaction_commit());
    setter_fail_at_k = 0;
}

/* The Subject is recomputed from what the setter managed to write, not set to the
 * written value. Integer factors mean the answer can differ from what was asked for. */
static bool area_snap_setter(lv_subject_t * subject, void * user_data, int32_t value)
{
    LV_UNUSED(subject);
    LV_UNUSED(user_data);
    lv_subject_set_int(txn_w, value / lv_subject_get_int(txn_h));
    return true;
}

void test_subject_setter_result_can_snap_back(void)
{
    lv_subject_t * area = txn_build_area();
    lv_subject_set_int_setter(area, area_snap_setter, NULL);
    TEST_ASSERT_EQUAL(20, lv_subject_get_int(area));   /* 4 * 5 */

    /* 30 / 5 is 6, and 6 * 5 is 30: exact. */
    lv_subject_set_int(area, 30);
    TEST_ASSERT_EQUAL(6, lv_subject_get_int(txn_w));
    TEST_ASSERT_EQUAL(30, lv_subject_get_int(area));

    /* 28 / 5 is 5, and 5 * 5 is 25: the area snaps back to what is reachable. */
    lv_subject_set_int(area, 28);
    TEST_ASSERT_EQUAL(5, lv_subject_get_int(txn_w));
    TEST_ASSERT_EQUAL(25, lv_subject_get_int(area));
}

/* Adjust whichever dimension was touched longest ago. This is what the version stamps
 * are for. */
static bool area_oldest_setter(lv_subject_t * subject, void * user_data, int32_t value)
{
    LV_UNUSED(subject);
    LV_UNUSED(user_data);
    uint32_t w_at = lv_subject_get_changed_at(txn_w);
    uint32_t h_at = lv_subject_get_changed_at(txn_h);

    /* Signed difference, because the stamps wrap. */
    if((int32_t)(w_at - h_at) < 0) lv_subject_set_int(txn_w, value / lv_subject_get_int(txn_h));
    else lv_subject_set_int(txn_h, value / lv_subject_get_int(txn_w));
    return true;
}

void test_subject_setter_picks_the_oldest_dimension(void)
{
    lv_subject_t * area = txn_build_area();      /* w = 4 then h = 5, so w is older */
    lv_subject_set_int_setter(area, area_oldest_setter, NULL);

    lv_subject_set_int(area, 40);                /* w is older, so w moves: 40 / 5 */
    TEST_ASSERT_EQUAL(8, lv_subject_get_int(txn_w));
    TEST_ASSERT_EQUAL(5, lv_subject_get_int(txn_h));

    lv_subject_set_int(area, 24);                /* now h is the older one: 24 / 8 */
    TEST_ASSERT_EQUAL(8, lv_subject_get_int(txn_w));
    TEST_ASSERT_EQUAL(3, lv_subject_get_int(txn_h));
}

/* A setter on a Subject with no mapper is refused: a plain Subject is already writable. */
void test_subject_setter_needs_a_mapper(void)
{
    lv_subject_t * plain = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int_setter(plain, area_snap_setter, NULL);
    TEST_ASSERT_TRUE(lv_subject_is_writable(plain));

    lv_subject_set_int(plain, 3);
    TEST_ASSERT_EQUAL(3, lv_subject_get_int(plain));
}

/*---------------------------------------------------------------
 * Static dependencies: what may be deleted
 *--------------------------------------------------------------*/

static lv_subject_t * branch_use_metric;
static lv_subject_t * branch_celsius;
static lv_subject_t * branch_fahrenheit;

/* Reads one of two Subjects, so the other leaves no dependency edge behind. */
static bool branch_shown_mapper(lv_subject_t * subject, void * user_data, int32_t * value)
{
    LV_UNUSED(subject);
    LV_UNUSED(user_data);
    int32_t before = *value;
    *value = lv_subject_get_int(branch_use_metric) ? lv_subject_get_int(branch_celsius)
             : lv_subject_get_int(branch_fahrenheit);
    return *value != before;
}

static lv_subject_t * branch_build(void)
{
    branch_use_metric = subject_create(LV_SUBJECT_TYPE_INT);
    branch_celsius = subject_create(LV_SUBJECT_TYPE_INT);
    branch_fahrenheit = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int(branch_use_metric, 1);
    lv_subject_set_int(branch_celsius, 20);
    lv_subject_set_int(branch_fahrenheit, 68);

    lv_subject_t * shown = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int_mapper(shown, branch_shown_mapper, NULL);
    return shown;
}

/* The branch that was not taken leaves no dependency edge, which is exactly what makes
 * the dynamic graph the wrong thing to base lifetime on. */
void test_subject_unread_branch_has_no_dynamic_edge(void)
{
    lv_subject_t * shown = branch_build();
    TEST_ASSERT_EQUAL(20, lv_subject_get_int(shown));

    /* celsius was read, fahrenheit was not. */
    TEST_ASSERT_EQUAL(1, lv_ll_get_len(&branch_celsius->dependents));
    TEST_ASSERT_EQUAL(0, lv_ll_get_len(&branch_fahrenheit->dependents));
}

/* Declaring the branches keeps the unread one alive. */
void test_subject_static_deps_refuse_deleting_an_unread_branch(void)
{
    lv_subject_t * shown = branch_build();
    /* Filled here because the Subjects are created per test. Real code emits a
     * `static lv_subject_t * const [] = {...}` initialiser instead. */
    static lv_subject_t * shown_deps[3];
    shown_deps[0] = branch_use_metric;
    shown_deps[1] = branch_celsius;
    shown_deps[2] = branch_fahrenheit;
    lv_subject_set_static_deps(shown, shown_deps, 3);

    TEST_ASSERT_EQUAL(20, lv_subject_get_int(shown));
    TEST_ASSERT_EQUAL(0, lv_ll_get_len(&branch_fahrenheit->dependents));

    /* Refused, even though nothing currently reads it. */
    lv_subject_delete(branch_fahrenheit);

    /* So taking the other branch still finds it there. */
    lv_subject_set_int(branch_use_metric, 0);
    TEST_ASSERT_EQUAL(68, lv_subject_get_int(shown));
}

/* Clearing the declaration gives the references back. */
void test_subject_static_deps_can_be_cleared(void)
{
    lv_subject_t * shown = branch_build();
    static lv_subject_t * deps[1];
    deps[0] = branch_fahrenheit;

    lv_subject_set_static_deps(shown, deps, 1);
    TEST_ASSERT_EQUAL(1, branch_fahrenheit->static_ref_cnt);

    lv_subject_set_static_deps(shown, NULL, 0);
    TEST_ASSERT_EQUAL(0, branch_fahrenheit->static_ref_cnt);
}

/* A setter's write targets are declared the same way: they have to outlive the Subject
 * that writes them just as a mapper's sources do. */
void test_subject_static_deps_cover_setter_targets(void)
{
    chain_build();
    static lv_subject_t * k_deps[1];
    k_deps[0] = chain_adc;
    lv_subject_set_static_deps(chain_k, k_deps, 1);

    lv_subject_delete(chain_adc);           /* refused: k's setter writes it */
    lv_subject_set_int(chain_k, 350);
    TEST_ASSERT_EQUAL(250, lv_subject_get_int(chain_adc));
}

/*---------------------------------------------------------------
 * Accumulating mappers, which is what eager mode is for
 *--------------------------------------------------------------*/

static lv_subject_t * acc_reading;
static uint32_t acc_runs;

/* A peak hold: it reads its own previous value, so it is only well defined if it runs
 * once per change rather than once per read. That is eager mode. */
static bool peak_mapper(lv_subject_t * subject, void * user_data, int32_t * value)
{
    LV_UNUSED(subject);
    LV_UNUSED(user_data);
    acc_runs++;
    int32_t now = lv_subject_get_int(acc_reading);
    if(now <= *value) return false;
    *value = now;
    return true;
}

static lv_subject_t * peak_build(lv_subject_mode_t mode)
{
    acc_runs = 0;
    acc_reading = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int(acc_reading, 0);

    lv_subject_t * peak = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int_mapper(peak, peak_mapper, NULL);
    lv_subject_set_mode(peak, mode);
    return peak;
}

/* An eager accumulator sees every value, even though nothing reads it in between. */
void test_subject_eager_mapper_can_accumulate(void)
{
    lv_subject_t * peak = peak_build(LV_SUBJECT_MODE_EAGER);

    lv_subject_set_int(acc_reading, 10);
    lv_subject_set_int(acc_reading, 40);
    lv_subject_set_int(acc_reading, 25);
    lv_subject_set_int(acc_reading, 5);

    /* 40 was the highest, and it was never read while it was current. */
    TEST_ASSERT_EQUAL(40, lv_subject_get_int(peak));
}

/* The same mapper declared lazy misses the peak, because nothing pulled it while the
 * high value was current. This is why an accumulator has to be eager. */
void test_subject_lazy_mapper_cannot_accumulate(void)
{
    lv_subject_t * peak = peak_build(LV_SUBJECT_MODE_LAZY);

    lv_subject_set_int(acc_reading, 10);
    lv_subject_set_int(acc_reading, 40);
    lv_subject_set_int(acc_reading, 25);
    lv_subject_set_int(acc_reading, 5);

    /* One evaluation, at the read, and by then the reading is 5. */
    TEST_ASSERT_EQUAL(5, lv_subject_get_int(peak));
}

/* An eager Subject runs once per transaction, not once per write inside one. A
 * transaction is atomic, so its intermediate states are not observable. */
void test_subject_eager_mapper_sees_transactions_not_writes(void)
{
    lv_subject_t * peak = peak_build(LV_SUBJECT_MODE_EAGER);

    lv_subject_transaction_begin();
    lv_subject_set_int(acc_reading, 40);
    lv_subject_set_int(acc_reading, 5);
    lv_subject_transaction_commit();

    /* 40 existed only inside the transaction, so the peak never saw it. */
    TEST_ASSERT_EQUAL(5, lv_subject_get_int(peak));

    /* Written on its own it is a transaction of its own, so it is seen. */
    lv_subject_set_int(acc_reading, 40);
    lv_subject_set_int(acc_reading, 5);
    TEST_ASSERT_EQUAL(40, lv_subject_get_int(peak));
}

/* A clamped Subject is a pure mirror, so it stays lazy. This also covers colour, whose
 * coverage went with the min/max helpers. */
void test_subject_create_clamped_color(void)
{
    lv_subject_t * col = subject_create(LV_SUBJECT_TYPE_COLOR);
    lv_subject_set_color(col, lv_color_hex(0x808080));

    lv_subject_value_t lo;
    lv_subject_value_t hi;
    lv_memzero(&lo, sizeof(lo));
    lv_memzero(&hi, sizeof(hi));
    lo.color = lv_color_hex(0x000000);
    hi.color = lv_color_hex(0x404040);

    lv_subject_t * bounded = lv_subject_create_clamped(col, lo, hi, NULL);
    TEST_ASSERT_NOT_NULL(bounded);
    TEST_ASSERT_FALSE(lv_subject_is_eager(bounded));
    TEST_ASSERT_EQUAL(lv_color_to_u32(lv_color_hex(0x404040)),
                      lv_color_to_u32(lv_subject_get_color(bounded)));

    lv_subject_set_color(col, lv_color_hex(0x101010));
    TEST_ASSERT_EQUAL(lv_color_to_u32(lv_color_hex(0x101010)),
                      lv_color_to_u32(lv_subject_get_color(bounded)));

    lv_subject_delete(bounded);
}

/*---------------------------------------------------------------
 * Setters on the pointer-shaped types, and what a rollback owes them
 *--------------------------------------------------------------*/

static lv_subject_t * own_raw;          /* plain string, the root */
static lv_subject_t * own_shout;        /* computed from it, with a setter */
static uint32_t own_setter_fail;
static char own_raw_buf[32];
static char own_shout_buf[32];

/* Upper-cases whatever `own_raw` holds. */
static bool shout_mapper(lv_subject_t * subject, void * user_data, char * buf, size_t size)
{
    LV_UNUSED(subject);
    LV_UNUSED(user_data);
    const char * src = lv_subject_get_string(own_raw);
    char next[32];
    size_t i = 0;
    for(; src != NULL && src[i] != '\0' && i < sizeof(next) - 1; i++) {
        next[i] = (src[i] >= 'a' && src[i] <= 'z') ? (char)(src[i] - 32) : src[i];
    }
    next[i] = '\0';
    if(lv_streq(next, buf)) return false;
    lv_strlcpy(buf, next, size);
    return true;
}

/* Writing the shouted version writes the root, lower-cased. */
static bool shout_setter(lv_subject_t * subject, void * user_data, const char * value)
{
    LV_UNUSED(subject);
    LV_UNUSED(user_data);
    if(own_setter_fail) return false;

    char next[32];
    size_t i = 0;
    for(; value != NULL && value[i] != '\0' && i < sizeof(next) - 1; i++) {
        next[i] = (value[i] >= 'A' && value[i] <= 'Z') ? (char)(value[i] + 32) : value[i];
    }
    next[i] = '\0';
    lv_subject_copy_string(own_raw, next);
    return true;
}

static void shout_build(void)
{
    own_setter_fail = 0;

    own_raw = subject_create(LV_SUBJECT_TYPE_STRING);
    lv_subject_set_string_buffer_static(own_raw, own_raw_buf, sizeof(own_raw_buf));
    lv_subject_copy_string(own_raw, "hello");

    own_shout = subject_create(LV_SUBJECT_TYPE_STRING);
    lv_subject_set_string_buffer_static(own_shout, own_shout_buf, sizeof(own_shout_buf));
    lv_subject_set_string_mapper(own_shout, shout_mapper, NULL);
    lv_subject_set_string_setter(own_shout, shout_setter, NULL);
}

/* A string Subject can be two-way bound like any other. */
void test_subject_string_setter_writes_upstream(void)
{
    shout_build();
    TEST_ASSERT_EQUAL_STRING("HELLO", lv_subject_get_string(own_shout));
    TEST_ASSERT_TRUE(lv_subject_is_writable(own_shout));

    TEST_ASSERT_TRUE(lv_subject_copy_string(own_shout, "WORLD"));
    TEST_ASSERT_EQUAL_STRING("world", lv_subject_get_string(own_raw));
    TEST_ASSERT_EQUAL_STRING("WORLD", lv_subject_get_string(own_shout));
}

/* A failing string setter puts the copied bytes back. That needs a snapshot, because a
 * copying Subject writes over its own buffer. */
void test_subject_string_setter_rolls_back_the_bytes(void)
{
    shout_build();
    TEST_ASSERT_EQUAL_STRING("HELLO", lv_subject_get_string(own_shout));

    own_setter_fail = 1;
    TEST_ASSERT_FALSE(lv_subject_copy_string(own_shout, "WORLD"));

    TEST_ASSERT_EQUAL_STRING("hello", lv_subject_get_string(own_raw));
    TEST_ASSERT_EQUAL_STRING("HELLO", lv_subject_get_string(own_shout));

    /* Still usable afterwards. */
    own_setter_fail = 0;
    TEST_ASSERT_TRUE(lv_subject_copy_string(own_shout, "AGAIN"));
    TEST_ASSERT_EQUAL_STRING("again", lv_subject_get_string(own_raw));
}

/* A computed string with no setter is refused, not silently ignored. */
void test_subject_computed_string_without_setter_is_refused(void)
{
    shout_build();
    lv_subject_set_string_setter(own_shout, NULL, NULL);
    TEST_ASSERT_FALSE(lv_subject_is_writable(own_shout));
    TEST_ASSERT_FALSE(lv_subject_copy_string(own_shout, "NOPE"));
    TEST_ASSERT_EQUAL_STRING("hello", lv_subject_get_string(own_raw));
}

/* An owned pointer replaced inside a transaction is not freed until the commit, so an
 * abort can put the old one back instead of reaching into freed memory. */
static lv_subject_t * own_ptr;
static uint32_t own_frees;
static void * own_last_freed;

static void own_free_cb(void * p)
{
    own_frees++;
    own_last_freed = p;
    lv_free(p);
}

void test_subject_owned_pointer_release_waits_for_the_commit(void)
{
    own_ptr = subject_create(LV_SUBJECT_TYPE_POINTER);

    void * first = lv_malloc(16);
    lv_subject_set_pointer_owned(own_ptr, first, own_free_cb);

    own_frees = 0;
    void * second = lv_malloc(16);

    lv_subject_transaction_begin();
    lv_subject_set_pointer_owned(own_ptr, second, own_free_cb);
    /* Still inside: the old value has to stay alive in case this is undone. */
    TEST_ASSERT_EQUAL(0, own_frees);
    lv_subject_transaction_commit();

    /* Committed, so the replaced value goes. */
    TEST_ASSERT_EQUAL(1, own_frees);
    TEST_ASSERT_EQUAL_PTR(first, own_last_freed);
    TEST_ASSERT_EQUAL_PTR(second, lv_subject_get_pointer(own_ptr));
}

/*---------------------------------------------------------------
 * An Observer's write is a transaction of its own
 *--------------------------------------------------------------*/

static lv_subject_t * obs_src;
static lv_subject_t * obs_a;
static lv_subject_t * obs_b;

/* What a downstream Observer saw, in order. */
static char obs_seen[8][32];
static uint32_t obs_seen_cnt;
static uint32_t obs_seen_txn[8];

static void obs_witness_cb(lv_observer_t * observer, lv_subject_t * subject)
{
    LV_UNUSED(observer);
    LV_UNUSED(subject);
    if(obs_seen_cnt >= 8) return;

    lv_snprintf(obs_seen[obs_seen_cnt], sizeof(obs_seen[0]), "a=%d b=%d",
                (int)lv_subject_get_int(obs_a), (int)lv_subject_get_int(obs_b));
    obs_seen_txn[obs_seen_cnt] = lv_subject_get_transaction_id();
    obs_seen_cnt++;
}

/* Two writes in a row, from inside a notification. */
static void obs_recompute_cb(lv_observer_t * observer, lv_subject_t * subject)
{
    LV_UNUSED(observer);
    int32_t v = lv_subject_get_int(subject);
    lv_subject_set_int(obs_a, v);
    lv_subject_set_int(obs_b, v);
}

/* Each write settles and notifies before the next begins, so the witness sees the
 * intermediate state. Treating the two as one nested transaction would hide it, and
 * would quietly make hand-written Observer code look glitch-free without being it. */
void test_subject_observer_write_is_its_own_transaction(void)
{
    obs_src = subject_create(LV_SUBJECT_TYPE_INT);
    obs_a = subject_create(LV_SUBJECT_TYPE_INT);
    obs_b = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int(obs_a, 0);
    lv_subject_set_int(obs_b, 0);

    lv_subject_add_observer(obs_src, obs_recompute_cb, NULL);
    lv_subject_add_observer(obs_a, obs_witness_cb, NULL);
    lv_subject_add_observer(obs_b, obs_witness_cb, NULL);

    obs_seen_cnt = 0;
    lv_subject_set_int(obs_src, 7);

    /* a moved first, so the witness saw a=7 while b was still 0, then both. */
    TEST_ASSERT_EQUAL(2, obs_seen_cnt);
    TEST_ASSERT_EQUAL_STRING("a=7 b=0", obs_seen[0]);
    TEST_ASSERT_EQUAL_STRING("a=7 b=7", obs_seen[1]);

    /* And they were two transactions, not one. */
    TEST_ASSERT_NOT_EQUAL(obs_seen_txn[0], obs_seen_txn[1]);
}

/* A mapper is different: the graph settles inside the transaction, so a dependent that
 * reads both never sees one of them move without the other. */
static bool obs_sum_mapper(lv_subject_t * subject, void * user_data, int32_t * value)
{
    LV_UNUSED(subject);
    LV_UNUSED(user_data);
    int32_t before = *value;
    *value = lv_subject_get_int(obs_a) + lv_subject_get_int(obs_b);
    return *value != before;
}

void test_subject_mapper_graph_settles_before_notifying(void)
{
    obs_a = subject_create(LV_SUBJECT_TYPE_INT);
    obs_b = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int(obs_a, 1);
    lv_subject_set_int(obs_b, 1);

    lv_subject_t * sum = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int_mapper(sum, obs_sum_mapper, NULL);

    observer_called = 0;
    lv_subject_add_observer(sum, observer_basic, NULL);
    observer_called = 0;

    /* Both moved together, so the dependent is notified once. */
    lv_subject_transaction_begin();
    lv_subject_set_int(obs_a, 10);
    lv_subject_set_int(obs_b, 20);
    lv_subject_transaction_commit();

    TEST_ASSERT_EQUAL(30, lv_subject_get_int(sum));
    TEST_ASSERT_EQUAL(1, observer_called);
}

/*---------------------------------------------------------------
 * An arc's change rate decides how many writes a click makes
 *--------------------------------------------------------------*/

static uint32_t arc_writes;

static void arc_write_counter_cb(lv_observer_t * observer, lv_subject_t * subject)
{
    LV_UNUSED(observer);
    LV_UNUSED(subject);
    arc_writes++;
}

/* Press near the top of the arc and hold, so the widget has several frames to move.
 * Returns how many times the bound Subject changed. */
static uint32_t arc_click_writes(uint32_t change_rate)
{
    lv_subject_t * value = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int(value, 0);

    lv_obj_t * arc = lv_arc_create(lv_screen_active());
    lv_obj_set_size(arc, 200, 200);
    lv_obj_set_pos(arc, 0, 0);
    lv_arc_set_range(arc, 0, 1000);
    lv_arc_set_value(arc, 0);
    lv_arc_set_change_rate(arc, change_rate);
    lv_arc_bind_value(arc, value);
    lv_obj_update_layout(arc);

    arc_writes = 0;
    lv_subject_add_observer(value, arc_write_counter_cb, NULL);
    arc_writes = 0;

    /* The far side of the arc from where the knob sits. */
    lv_test_mouse_move_to(190, 100);
    lv_test_mouse_press();
    for(int i = 0; i < 6; i++) lv_test_wait(20);
    lv_test_mouse_release();
    lv_test_wait(20);

    lv_obj_delete(arc);
    return arc_writes;
}

/* The default rate limits how far one frame may move the value, so a click far from the
 * knob walks there over several frames — and every step is a write, a transaction and a
 * notification. Measured here: eight writes at the default, one with the rate raised
 * past the whole range. */
void test_subject_arc_change_rate_decides_the_write_count(void)
{
    uint32_t slow = arc_click_writes(720);      /* the widget default */
    uint32_t fast = arc_click_writes(360000);   /* the full circle in a millisecond */

    TEST_ASSERT_GREATER_THAN_UINT32(1, slow);
    TEST_ASSERT_EQUAL(1, fast);
}

/*---------------------------------------------------------------
 * A guard that short-circuits drops the dependency behind it
 *--------------------------------------------------------------*/

static lv_subject_t * guard_w;
static lv_subject_t * guard_h;
static uint32_t guard_runs;

/* `w > 0 && h > 0 ? w * h : 0`, as the exporter would emit it. When `w` is zero the
 * `&&` never evaluates its right side, so `h` is never read. */
static bool guarded_area_mapper(lv_subject_t * subject, void * user_data, int32_t * value)
{
    LV_UNUSED(subject);
    LV_UNUSED(user_data);
    guard_runs++;

    int32_t next = (lv_subject_get_int(guard_w) > 0 && lv_subject_get_int(guard_h) > 0)
                   ? lv_subject_get_int(guard_w) * lv_subject_get_int(guard_h) : 0;
    if(next == *value) return false;
    *value = next;
    return true;
}

void test_subject_a_short_circuit_drops_the_dependency_behind_it(void)
{
    guard_w = subject_create(LV_SUBJECT_TYPE_INT);
    guard_h = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int(guard_w, 4);
    lv_subject_set_int(guard_h, 5);

    lv_subject_t * area = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int_mapper(area, guarded_area_mapper, NULL);
    TEST_ASSERT_EQUAL(20, lv_subject_get_int(area));

    /* Both were read, so both are dependencies. */
    TEST_ASSERT_EQUAL(2, lv_subject_get_dependency_count(area));

    /* With `w` at zero the guard fails and `h` is never reached, so it stops being a
     * dependency. The edges are rebuilt from whatever the mapper read this time. */
    lv_subject_set_int(guard_w, 0);
    TEST_ASSERT_EQUAL(0, lv_subject_get_int(area));
    TEST_ASSERT_EQUAL(1, lv_subject_get_dependency_count(area));
    TEST_ASSERT_EQUAL_PTR(guard_w, lv_subject_get_dependency(area, 0));

    /* So a change to `h` cannot reach it. Nothing marks it dirty, and reading it does
     * not re-run the mapper: the work is skipped entirely, not merely deferred. */
    guard_runs = 0;
    lv_subject_set_int(guard_h, 9);
    TEST_ASSERT_FALSE(lv_subject_is_dirty(area));
    TEST_ASSERT_EQUAL(0, lv_subject_get_int(area));
    TEST_ASSERT_EQUAL(0, guard_runs);

    /* The dependency comes back by itself once the guard passes again. */
    lv_subject_set_int(guard_w, 3);
    TEST_ASSERT_EQUAL(27, lv_subject_get_int(area));
    TEST_ASSERT_EQUAL(2, lv_subject_get_dependency_count(area));

    /* And `h` reaches it once more. */
    guard_runs = 0;
    lv_subject_set_int(guard_h, 10);
    TEST_ASSERT_EQUAL(30, lv_subject_get_int(area));
    TEST_ASSERT_EQUAL(1, guard_runs);
}

/*---------------------------------------------------------------
 * The guard's asymmetry, and what removes it
 *--------------------------------------------------------------*/

static uint32_t asym_runs;

/* `w > 0 && h > 0 ? w * h : 0`. Reading `w` first is what makes it asymmetric: when `w`
 * is zero `h` is never reached, but when `h` is zero `w` has already been read. */
static bool asym_area_mapper(lv_subject_t * subject, void * user_data, int32_t * value)
{
    LV_UNUSED(subject);
    LV_UNUSED(user_data);
    asym_runs++;
    int32_t next = (lv_subject_get_int(guard_w) > 0 && lv_subject_get_int(guard_h) > 0)
                   ? lv_subject_get_int(guard_w) * lv_subject_get_int(guard_h) : 0;
    if(next == *value) return false;
    *value = next;
    return true;
}

void test_subject_a_single_guard_is_asymmetric(void)
{
    guard_w = subject_create(LV_SUBJECT_TYPE_INT);
    guard_h = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int(guard_w, 4);
    lv_subject_set_int(guard_h, 5);

    lv_subject_t * area = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int_mapper(area, asym_area_mapper, NULL);
    TEST_ASSERT_EQUAL(20, lv_subject_get_int(area));

    /* w at zero: `h` is never read, so it is not a dependency. */
    lv_subject_set_int(guard_w, 0);
    TEST_ASSERT_EQUAL(0, lv_subject_get_int(area));
    TEST_ASSERT_EQUAL(1, lv_subject_get_dependency_count(area));

    /* h at zero is the other way round: `w` was read to get to the second test, so it
     * stays a dependency and a write to it re-runs the mapper for nothing. */
    lv_subject_set_int(guard_w, 4);
    lv_subject_set_int(guard_h, 0);
    TEST_ASSERT_EQUAL(0, lv_subject_get_int(area));
    TEST_ASSERT_EQUAL(2, lv_subject_get_dependency_count(area));

    asym_runs = 0;
    lv_subject_set_int(guard_w, 7);
    TEST_ASSERT_EQUAL(0, lv_subject_get_int(area));
    TEST_ASSERT_EQUAL(1, asym_runs);          /* ran, and had nothing to say */
}

static bool peeking_only_mapper(lv_subject_t * subject, void * user_data, int32_t * value)
{
    LV_UNUSED(subject);
    LV_UNUSED(user_data);
    int32_t next = lv_subject_peek_int(guard_w);
    if(next == *value) return false;
    *value = next;
    return true;
}

/* Peeking fixes it outright: the mapper looks without depending, then says what it
 * depends on. Each case ends up resting on exactly what its answer rests on. */
static uint32_t peek_runs;

static bool peeking_area_mapper(lv_subject_t * subject, void * user_data, int32_t * value)
{
    LV_UNUSED(subject);
    LV_UNUSED(user_data);
    peek_runs++;

    int32_t next;
    if(lv_subject_peek_int(guard_w) == 0) {
        lv_subject_track_dependency(guard_w);
        next = 0;
    }
    else if(lv_subject_peek_int(guard_h) == 0) {
        lv_subject_track_dependency(guard_h);
        next = 0;
    }
    else {
        next = lv_subject_get_int(guard_w) * lv_subject_get_int(guard_h);
    }

    if(next == *value) return false;
    *value = next;
    return true;
}

void test_subject_peek_gives_an_exact_dependency_set(void)
{
    guard_w = subject_create(LV_SUBJECT_TYPE_INT);
    guard_h = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int(guard_w, 4);
    lv_subject_set_int(guard_h, 5);

    lv_subject_t * area = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int_mapper(area, peeking_area_mapper, NULL);
    TEST_ASSERT_EQUAL(20, lv_subject_get_int(area));
    TEST_ASSERT_EQUAL(2, lv_subject_get_dependency_count(area));

    /* w at zero: it depends on w alone, and h is free. */
    lv_subject_set_int(guard_w, 0);
    TEST_ASSERT_EQUAL(0, lv_subject_get_int(area));
    TEST_ASSERT_EQUAL(1, lv_subject_get_dependency_count(area));
    TEST_ASSERT_EQUAL_PTR(guard_w, lv_subject_get_dependency(area, 0));

    peek_runs = 0;
    lv_subject_set_int(guard_h, 9);
    TEST_ASSERT_FALSE(lv_subject_is_dirty(area));
    TEST_ASSERT_EQUAL(0, lv_subject_get_int(area));
    TEST_ASSERT_EQUAL(0, peek_runs);

    /* h at zero: the other way round, which the single guard could not manage. The
     * mapper peeked at w to get here, and that look cost nothing. */
    lv_subject_set_int(guard_w, 4);
    lv_subject_set_int(guard_h, 0);
    TEST_ASSERT_EQUAL(0, lv_subject_get_int(area));
    TEST_ASSERT_EQUAL(1, lv_subject_get_dependency_count(area));
    TEST_ASSERT_EQUAL_PTR(guard_h, lv_subject_get_dependency(area, 0));

    peek_runs = 0;
    lv_subject_set_int(guard_w, 7);
    TEST_ASSERT_FALSE(lv_subject_is_dirty(area));
    TEST_ASSERT_EQUAL(0, lv_subject_get_int(area));
    TEST_ASSERT_EQUAL(0, peek_runs);

    /* Both back, and both are dependencies again. */
    lv_subject_set_int(guard_h, 3);
    TEST_ASSERT_EQUAL(21, lv_subject_get_int(area));
    TEST_ASSERT_EQUAL(2, lv_subject_get_dependency_count(area));
}

/* A peek on its own does not subscribe: that is the point, and the hazard. */
void test_subject_peek_alone_does_not_subscribe(void)
{
    guard_w = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int(guard_w, 1);

    lv_subject_t * mirror = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int_mapper(mirror, peeking_only_mapper, NULL);
    TEST_ASSERT_EQUAL(1, lv_subject_get_int(mirror));
    TEST_ASSERT_EQUAL(0, lv_subject_get_dependency_count(mirror));

    /* Nothing connects them, so the change never arrives. */
    lv_subject_set_int(guard_w, 2);
    TEST_ASSERT_FALSE(lv_subject_is_dirty(mirror));
    TEST_ASSERT_EQUAL(1, lv_subject_get_int(mirror));
}

#endif
