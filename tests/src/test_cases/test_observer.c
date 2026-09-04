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

/*--------------------------------------------------------
 * A mapper that reads the value slot: a write transform
 *-------------------------------------------------------*/

/* Replaces the removed lv_subject_set_min_value_int()/lv_subject_set_max_value_int(). */
static bool clamp_0_100_mapper(lv_subject_t * subject, void * user_data, lv_subject_value_t input, int32_t * value)
{
    LV_UNUSED(subject);
    LV_UNUSED(user_data);
    /* `*value` holds the previous stored value on entry, so keep it to answer
     * "did anything change". No previous-value bookkeeping is needed anywhere else. */
    int32_t before = *value;
    *value = lv_subject_clamp_int(input.num, 0, 100);
    return *value != before;
}

void test_subject_mapper_clamps_written_value(void)
{
    lv_subject_t * subject = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int_mapper(subject, clamp_0_100_mapper, NULL);

    lv_subject_set_int(subject, 50);
    TEST_ASSERT_EQUAL(50, lv_subject_get_int(subject));

    lv_subject_set_int(subject, 150);
    TEST_ASSERT_EQUAL(100, lv_subject_get_int(subject));

    lv_subject_set_int(subject, -20);
    TEST_ASSERT_EQUAL(0, lv_subject_get_int(subject));

    /* A write transform reads no other Subject, so it wires no dependency. */
    TEST_ASSERT_EQUAL(0, lv_subject_get_dependency_count(subject));
}

/* The same mapper reused by two Subjects, each with its own limits in user_data.
 * This is why the mapper receives the Subject. */
typedef struct {
    int32_t min;
    int32_t max;
} range_t;

static bool clamp_user_data_mapper(lv_subject_t * subject, void * user_data, lv_subject_value_t input, int32_t * value)
{
    LV_UNUSED(subject);
    /* The range comes from the mapper's captured state, which is what makes one mapper
     * function reusable across several Subjects. */
    const range_t * range = user_data;
    int32_t before = *value;
    *value = lv_subject_clamp_int(input.num, range->min, range->max);
    return *value != before;
}

void test_subject_mapper_reused_via_user_data(void)
{
    static const range_t narrow = { .min = 0, .max = 10 };
    static const range_t wide = { .min = -100, .max = 100 };

    /* The range travels with the mapper, not with the Subject: this is the closure
     * capture that makes one mapper function serve several Subjects. */
    lv_subject_t * a = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int_mapper(a, clamp_user_data_mapper, (void *)&narrow);

    lv_subject_t * b = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int_mapper(b, clamp_user_data_mapper, (void *)&wide);

    lv_subject_set_int(a, 50);
    lv_subject_set_int(b, 50);
    TEST_ASSERT_EQUAL(10, lv_subject_get_int(a));
    TEST_ASSERT_EQUAL(50, lv_subject_get_int(b));
}

/* A clamp whose limits are themselves Subjects. Not expressible with the removed
 * min_value/max_value fields. */
static bool clamp_reactive_mapper(lv_subject_t * subject, void * user_data, lv_subject_value_t input, int32_t * value)
{
    LV_UNUSED(subject);
    LV_UNUSED(user_data);
    int32_t before = *value;
    /* Clamping the last *input* rather than the stored value, so widening a limit
     * restores a value that was previously clamped away. */
    *value = lv_subject_clamp_int(input.num, lv_subject_get_int(dep_a), lv_subject_get_int(dep_b));
    return *value != before;
}

void test_subject_mapper_clamp_with_reactive_limits(void)
{
    dep_a = subject_create(LV_SUBJECT_TYPE_INT);  /* the minimum */
    dep_b = subject_create(LV_SUBJECT_TYPE_INT);  /* the maximum */
    lv_subject_set_int(dep_a, 0);
    lv_subject_set_int(dep_b, 100);

    lv_subject_t * value = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int_mapper(value, clamp_reactive_mapper, NULL);

    lv_subject_set_int(value, 150);
    TEST_ASSERT_EQUAL(100, lv_subject_get_int(value));

    /* Both limits were read, so both are dependencies. */
    TEST_ASSERT_EQUAL(2, lv_subject_get_dependency_count(value));

    /* Tightening the maximum re-clamps the value that is already stored. */
    lv_subject_set_int(dep_b, 40);
    TEST_ASSERT_EQUAL(40, lv_subject_get_int(value));

    /* Raising the minimum above the value pushes it up. */
    lv_subject_set_int(dep_a, 60);
    TEST_ASSERT_EQUAL(60, lv_subject_get_int(value));
}

/*--------------------------------------------------------
 * A mapper that overwrites the slot: a derived value
 *-------------------------------------------------------*/

static bool sum_mapper(lv_subject_t * subject, void * user_data, lv_subject_value_t input, int32_t * value)
{
    LV_UNUSED(user_data);
    LV_UNUSED(input);
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
static bool switched_mapper(lv_subject_t * subject, void * user_data, lv_subject_value_t input, int32_t * value)
{
    LV_UNUSED(user_data);
    LV_UNUSED(input);
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

static bool double_mapper(lv_subject_t * subject, void * user_data, lv_subject_value_t input, int32_t * value)
{
    LV_UNUSED(user_data);
    LV_UNUSED(input);
    LV_UNUSED(subject);
    *value = lv_subject_get_int(dep_a) * 2;
    return true;
}

static bool triple_mapper(lv_subject_t * subject, void * user_data, lv_subject_value_t input, int32_t * value)
{
    LV_UNUSED(user_data);
    LV_UNUSED(input);
    LV_UNUSED(subject);
    *value = lv_subject_get_int(dep_a) * 3;
    return true;
}

static bool diamond_sink_mapper(lv_subject_t * subject, void * user_data, lv_subject_value_t input, int32_t * value)
{
    LV_UNUSED(user_data);
    LV_UNUSED(input);
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

static bool chain_l1_mapper(lv_subject_t * subject, void * user_data, lv_subject_value_t input, int32_t * value)
{
    LV_UNUSED(user_data);
    LV_UNUSED(input);
    LV_UNUSED(subject);
    l1_runs++;
    *value = lv_subject_get_int(dep_a) * 2;
    return true;
}

static bool chain_l2_mapper(lv_subject_t * subject, void * user_data, lv_subject_value_t input, int32_t * value)
{
    LV_UNUSED(user_data);
    LV_UNUSED(input);
    LV_UNUSED(subject);
    l2_runs++;
    *value = lv_subject_get_int(chain_l1) + 1;
    return true;
}

static bool chain_l3_mapper(lv_subject_t * subject, void * user_data, lv_subject_value_t input, int32_t * value)
{
    LV_UNUSED(user_data);
    LV_UNUSED(input);
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

static bool self_reading_mapper(lv_subject_t * subject, void * user_data, lv_subject_value_t input, int32_t * value)
{
    LV_UNUSED(user_data);
    LV_UNUSED(input);
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
static bool illegally_writing_mapper(lv_subject_t * subject, void * user_data, lv_subject_value_t input,
                                     int32_t * value)
{
    LV_UNUSED(subject);
    LV_UNUSED(user_data);
    LV_UNUSED(input);
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
static bool float_half_mapper(lv_subject_t * subject, void * user_data, lv_subject_value_t input, float * value)
{
    LV_UNUSED(user_data);
    LV_UNUSED(input);
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
static bool string_format_mapper(lv_subject_t * subject, void * user_data, lv_subject_value_t input, char * buf,
                                 size_t size)
{
    LV_UNUSED(user_data);
    LV_UNUSED(input);
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

static bool color_mapper(lv_subject_t * subject, void * user_data, lv_subject_value_t input, lv_color_t * value)
{
    LV_UNUSED(user_data);
    LV_UNUSED(input);
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

static bool pointer_mapper(lv_subject_t * subject, void * user_data, lv_subject_value_t input, const void ** value)
{
    LV_UNUSED(user_data);
    LV_UNUSED(input);
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
static bool int_to_text_mapper(lv_observer_t * observer, const char ** out)
{
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
static bool constant_int_mapper(lv_observer_t * observer, int32_t * out)
{
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
static bool string_non_empty_mapper(lv_observer_t * observer, bool * out)
{
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
static bool dependency_probing_mapper(lv_observer_t * observer, int32_t * out)
{
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
static bool notifying_mapper(lv_subject_t * subject, void * user_data, lv_subject_value_t input, int32_t * value)
{
    LV_UNUSED(user_data);
    LV_UNUSED(input);
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

/* A lazy Subject only records the input on a write. Its mapper runs when something
 * reads it, so a run of writes costs one evaluation instead of one per write, and a
 * Subject nothing ever reads is never evaluated at all. */
void test_subject_lazy_defers_the_mapper_until_read(void)
{
    lv_subject_t * subject = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int_mapper(subject, clamp_0_100_mapper, NULL);

    lv_observer_t * observer = lv_subject_add_observer(subject, observer_basic, NULL);
    lv_observer_set_mode(observer, LV_OBSERVER_MODE_BATCHED);
    TEST_ASSERT_FALSE(lv_subject_is_eager(subject));

    observer_called = 0;
    lv_subject_set_int(subject, 500);
    lv_subject_set_int(subject, 600);
    lv_subject_set_int(subject, 700);

    /* Read the field directly: the mapper has not run, so nothing has been stored. */
    TEST_ASSERT_TRUE(lv_subject_is_dirty(subject));
    TEST_ASSERT_EQUAL(0, observer_called);

    /* The getter brings it up to date, and only then does the mapper run: once. */
    TEST_ASSERT_EQUAL(100, lv_subject_get_int(subject));
    TEST_ASSERT_FALSE(lv_subject_is_dirty(subject));
}

/* An eager Subject still evaluates inside set(), which is what eager means. */
void test_subject_eager_maps_inside_set(void)
{
    lv_subject_t * subject = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int_mapper(subject, clamp_0_100_mapper, NULL);
    lv_subject_set_mode(subject, LV_SUBJECT_MODE_EAGER);

    lv_subject_set_int(subject, 500);
    /* Stored already, without anyone reading it. */
    TEST_ASSERT_EQUAL(100, subject->value.num);
    TEST_ASSERT_FALSE(lv_subject_is_dirty(subject));
}

/* An immediate Observer promotes the Subject, so the mapper runs on the spot again. */
void test_subject_immediate_observer_undoes_the_deferral(void)
{
    lv_subject_t * subject = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int_mapper(subject, clamp_0_100_mapper, NULL);
    lv_subject_add_observer(subject, observer_basic, NULL);   /* IMMEDIATE by default */

    observer_called = 0;
    lv_subject_set_int(subject, 500);
    TEST_ASSERT_EQUAL(100, subject->value.num);
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

    /* `sum`'s mapper reads dep_a, so deleting dep_a would leave it without an input. */
    lv_subject_delete(dep_a);

    /* Still there and still working. */
    lv_subject_set_int(dep_a, 10);
    TEST_ASSERT_EQUAL(13, lv_subject_get_int(sum));

    /* Deleting the dependent first makes the dependency deletable. */
    subject_delete(sum);
    TEST_ASSERT_EQUAL(0, lv_ll_get_len(&dep_a->dependents));
    subject_delete(dep_a);
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

    /* a -> l1 -> l2: deleting the root takes the whole chain with it. */
    lv_subject_delete_cascade(dep_a);

    /* Nothing is left to walk; if an edge had survived this would touch freed memory. */
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

    lv_subject_delete_cascade(dep_a);
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
    lv_subject_delete_cascade(sum);  /* nothing depends on it */

    /* The dependencies survive, with their edges cleaned up. */
    TEST_ASSERT_EQUAL(0, lv_ll_get_len(&dep_a->dependents));
    lv_subject_set_int(dep_a, 5);
    TEST_ASSERT_EQUAL(5, lv_subject_get_int(dep_a));
}

/*=====================================================================
 * Derived-subject helpers
 *====================================================================*/

void test_subject_create_min_and_max_int(void)
{
    lv_subject_t * temp = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int(temp, 20);

    /* An int has a natural ordering, so no compare callback is needed. */
    lv_subject_t * lowest = lv_subject_create_min(temp, NULL);
    lv_subject_t * highest = lv_subject_create_max(temp, NULL);
    TEST_ASSERT_NOT_NULL(lowest);
    TEST_ASSERT_NOT_NULL(highest);

    TEST_ASSERT_EQUAL(20, lv_subject_get_int(lowest));
    TEST_ASSERT_EQUAL(20, lv_subject_get_int(highest));

    lv_subject_set_int(temp, 25);
    TEST_ASSERT_EQUAL(20, lv_subject_get_int(lowest));
    TEST_ASSERT_EQUAL(25, lv_subject_get_int(highest));

    lv_subject_set_int(temp, 15);
    TEST_ASSERT_EQUAL(15, lv_subject_get_int(lowest));
    TEST_ASSERT_EQUAL(25, lv_subject_get_int(highest));

    /* Back inside the range: neither extremum moves. */
    lv_subject_set_int(temp, 18);
    TEST_ASSERT_EQUAL(15, lv_subject_get_int(lowest));
    TEST_ASSERT_EQUAL(25, lv_subject_get_int(highest));

    /* They are eager, so they record every value rather than only the read ones. */
    TEST_ASSERT_TRUE(lv_subject_is_eager(lowest));

    lv_subject_delete(lowest);
    lv_subject_delete(highest);
}

/* An extremum Subject notifies like any other, so it can drive a Widget. */
void test_subject_create_max_notifies_observers(void)
{
    lv_subject_t * temp = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int(temp, 10);

    lv_subject_t * highest = lv_subject_create_max(temp, NULL);
    observer_called = 0;
    lv_subject_add_observer(highest, observer_basic, NULL);
    TEST_ASSERT_EQUAL(1, observer_called);  /* initial */

    lv_subject_set_int(temp, 50);
    TEST_ASSERT_EQUAL(2, observer_called);

    /* No new maximum, so nothing changed and nobody is notified. */
    lv_subject_set_int(temp, 30);
    TEST_ASSERT_EQUAL(2, observer_called);

    lv_subject_delete(highest);
}

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

void test_subject_create_min_with_compare_callback(void)
{
    static const item_t apple = { .name = "apple", .price = 30 };
    static const item_t pear  = { .name = "pear",  .price = 50 };
    static const item_t plum  = { .name = "plum",  .price = 10 };

    lv_subject_t * offered = subject_create(LV_SUBJECT_TYPE_POINTER);
    lv_subject_set_pointer(offered, (void *)&apple);

    lv_subject_t * cheapest = lv_subject_create_min(offered, cheaper_cb);
    TEST_ASSERT_NOT_NULL(cheapest);
    TEST_ASSERT_EQUAL_STRING("apple", ((const item_t *)lv_subject_get_pointer(cheapest))->name);

    lv_subject_set_pointer(offered, (void *)&pear);
    TEST_ASSERT_EQUAL_STRING("apple", ((const item_t *)lv_subject_get_pointer(cheapest))->name);

    lv_subject_set_pointer(offered, (void *)&plum);
    TEST_ASSERT_EQUAL_STRING("plum", ((const item_t *)lv_subject_get_pointer(cheapest))->name);

    lv_subject_delete(cheapest);
}

void test_subject_create_min_needs_a_compare_for_pointers(void)
{
    lv_subject_t * ptr = subject_create(LV_SUBJECT_TYPE_POINTER);
    /* No natural ordering for a pointer, so this has to be refused rather than guess. */
    TEST_ASSERT_NULL(lv_subject_create_min(ptr, NULL));
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

/* A helper keeps its configuration in the mapper's user data and frees it with the
 * Subject. The Subject's own user_data stays free for the application. */
void test_subject_helper_keeps_its_state_in_the_mapper(void)
{
    lv_subject_t * temp = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int(temp, 5);

    lv_subject_t * highest = lv_subject_create_max(temp, NULL);
    TEST_ASSERT_NOT_NULL(highest);

    /* Not exposed as the Subject's user data, which the application may still use. */
    TEST_ASSERT_NULL(lv_subject_get_user_data(highest));
    int dummy = 0;
    lv_subject_set_user_data(highest, &dummy);
    TEST_ASSERT_EQUAL_PTR(&dummy, lv_subject_get_user_data(highest));

    /* Still tracks the maximum, so the mapper's own state survived. */
    lv_subject_set_int(temp, 40);
    TEST_ASSERT_EQUAL(40, lv_subject_get_int(highest));
    lv_subject_set_int(temp, 10);
    TEST_ASSERT_EQUAL(40, lv_subject_get_int(highest));

    lv_subject_delete(highest);
}

/* A helper-created Subject is an ordinary dependent, so the delete rule covers it. */
void test_subject_helper_is_a_dependency_of_its_source(void)
{
    lv_subject_t * temp = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int(temp, 5);

    lv_subject_t * highest = lv_subject_create_max(temp, NULL);
    TEST_ASSERT_EQUAL(1, lv_ll_get_len(&temp->dependents));

    /* Refused while the extremum still reads it. */
    lv_subject_delete(temp);
    TEST_ASSERT_EQUAL(1, lv_ll_get_len(&temp->dependents));

    lv_subject_delete(highest);
    TEST_ASSERT_EQUAL(0, lv_ll_get_len(&temp->dependents));
}


/*=====================================================================
 * The mapper contract: input, stored value, captured state
 *====================================================================*/

/* The Subject keeps the last raw input, so a re-evaluation can re-derive from it. That
 * is what lets a widening limit restore a value that was clamped away. */
void test_subject_mapper_reevaluates_from_the_last_input(void)
{
    dep_a = subject_create(LV_SUBJECT_TYPE_INT);   /* the minimum */
    dep_b = subject_create(LV_SUBJECT_TYPE_INT);   /* the maximum */
    lv_subject_set_int(dep_a, 0);
    lv_subject_set_int(dep_b, 100);

    lv_subject_t * value = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int_mapper(value, clamp_reactive_mapper, NULL);

    lv_subject_set_int(value, 150);
    TEST_ASSERT_EQUAL(100, lv_subject_get_int(value));

    /* Widening the maximum brings the clamped-away 150 back, because the mapper is
     * handed the original input rather than the stored 100. */
    lv_subject_set_int(dep_b, 200);
    TEST_ASSERT_EQUAL(150, lv_subject_get_int(value));

    /* Narrowing again clamps it back down. */
    lv_subject_set_int(dep_b, 60);
    TEST_ASSERT_EQUAL(60, lv_subject_get_int(value));
}

/* The stored value handed to the mapper is the previous one, which is the whole reason
 * no previous-value bookkeeping is needed. */
static uint32_t seen_stored;
static int32_t seen_input;

static bool recording_mapper(lv_subject_t * subject, void * user_data, lv_subject_value_t input, int32_t * value)
{
    LV_UNUSED(subject);
    LV_UNUSED(user_data);
    seen_stored = (uint32_t) * value;
    seen_input = input.num;
    *value = input.num * 2;
    return true;
}

void test_subject_mapper_sees_the_previous_stored_value(void)
{
    lv_subject_t * subject = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int_mapper(subject, recording_mapper, NULL);

    /* Lazy, so reading is what runs the mapper. */
    lv_subject_set_int(subject, 5);
    TEST_ASSERT_EQUAL(10, lv_subject_get_int(subject));
    TEST_ASSERT_EQUAL(5, seen_input);

    lv_subject_set_int(subject, 7);
    TEST_ASSERT_EQUAL(14, lv_subject_get_int(subject));
    TEST_ASSERT_EQUAL(7, seen_input);
    /* On entry the slot still held the result of the previous run. */
    TEST_ASSERT_EQUAL(10, seen_stored);
}

/* A mapper reaches other Subjects through its captured state, so it does not need them
 * to be file-scope globals. */
typedef struct {
    lv_subject_t * left;
    lv_subject_t * right;
} pair_t;

static bool pair_sum_mapper(lv_subject_t * subject, void * user_data, lv_subject_value_t input, int32_t * value)
{
    LV_UNUSED(subject);
    LV_UNUSED(input);
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
 * A string Subject keeps no second buffer
 *====================================================================*/

/* The mapper receives the new string as `input` while the buffer still holds the
 * previous one, so it can compare the two without any stored history. */
static bool uppercase_mapper(lv_subject_t * subject, void * user_data, lv_subject_value_t input, char * buf,
                             size_t size)
{
    LV_UNUSED(subject);
    LV_UNUSED(user_data);
    const char * text = input.pointer;
    if(text == NULL) return false;   /* a dependency-driven re-run, nothing to do */

    char next[32];
    size_t i;
    for(i = 0; i + 1 < sizeof(next) && text[i] != '\0'; i++) {
        next[i] = (text[i] >= 'a' && text[i] <= 'z') ? (char)(text[i] - 'a' + 'A') : text[i];
    }
    next[i] = '\0';

    if(lv_strcmp(next, buf) == 0) return false;
    lv_strlcpy(buf, next, size);
    return true;
}

void test_subject_string_mapper_compares_input_against_the_buffer(void)
{
    static char buf[32];
    lv_subject_t * text = subject_create(LV_SUBJECT_TYPE_STRING);
    lv_subject_set_string_buffer_static(text, buf, sizeof(buf));
    lv_subject_set_string_mapper(text, uppercase_mapper, NULL);

    observer_called = 0;
    lv_subject_add_observer(text, observer_basic, NULL);
    observer_called = 0;

    lv_subject_copy_string(text, "hello");
    TEST_ASSERT_EQUAL_STRING("HELLO", lv_subject_get_string(text));
    TEST_ASSERT_EQUAL(1, observer_called);

    /* The same input maps to the same output, so nothing is notified. */
    lv_subject_copy_string(text, "hello");
    TEST_ASSERT_EQUAL(1, observer_called);

    /* Different input that maps to the same output is also not a change. */
    lv_subject_copy_string(text, "HELLO");
    TEST_ASSERT_EQUAL(1, observer_called);

    lv_subject_copy_string(text, "world");
    TEST_ASSERT_EQUAL_STRING("WORLD", lv_subject_get_string(text));
    TEST_ASSERT_EQUAL(2, observer_called);
}

void test_subject_snprintf_goes_through_the_mapper(void)
{
    static char buf[32];
    lv_subject_t * text = subject_create(LV_SUBJECT_TYPE_STRING);
    lv_subject_set_string_buffer_static(text, buf, sizeof(buf));
    lv_subject_set_string_mapper(text, uppercase_mapper, NULL);

    lv_subject_snprintf(text, "temp %d c", 21);
    TEST_ASSERT_EQUAL_STRING("TEMP 21 C", lv_subject_get_string(text));
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

/* The mapper must still be able to read the outgoing value, so the release happens
 * strictly after it has run. */
static bool inspecting_owned_mapper(lv_subject_t * subject, void * user_data, lv_subject_value_t input,
                                    const void ** value)
{
    LV_UNUSED(subject);
    LV_UNUSED(user_data);
    /* If the old value had already been freed this would be a use-after-free, which
     * ASan would catch. Reading its first byte proves it is still alive. */
    if(*value != NULL) {
        const uint8_t * old = *value;
        seen_stored = old[0];
    }
    *value = input.pointer;
    return true;
}

void test_subject_owned_value_is_freed_only_after_the_mapper_ran(void)
{
    lv_subject_t * frame = subject_create(LV_SUBJECT_TYPE_POINTER);
    lv_subject_set_pointer_mapper(frame, inspecting_owned_mapper, NULL);

    uint8_t * first = lv_malloc(16);
    first[0] = 0xAB;
    lv_subject_set_pointer_owned(frame, first, counting_free_cb);

    freed_cnt = 0;
    seen_stored = 0;
    uint8_t * second = lv_malloc(16);
    second[0] = 0xCD;
    lv_subject_set_pointer_owned(frame, second, counting_free_cb);

    /* The mapper saw the previous contents before they were released. */
    TEST_ASSERT_EQUAL(0xAB, seen_stored);
    TEST_ASSERT_EQUAL(1, freed_cnt);
    TEST_ASSERT_EQUAL_PTR(first, last_freed);
}

/* A mapper that rejects the new value keeps the old one, so the old one is not released.
 * The rejected input is still the Subject's, because it retains it. */
static bool rejecting_owned_mapper(lv_subject_t * subject, void * user_data, lv_subject_value_t input,
                                   const void ** value)
{
    LV_UNUSED(subject);
    LV_UNUSED(user_data);
    LV_UNUSED(input);
    LV_UNUSED(value);
    return false;
}

void test_subject_owned_value_survives_a_mapper_that_keeps_it(void)
{
    lv_subject_t * frame = subject_create(LV_SUBJECT_TYPE_POINTER);

    void * kept = lv_malloc(16);
    lv_subject_set_pointer_owned(frame, kept, counting_free_cb);

    lv_subject_set_pointer_mapper(frame, rejecting_owned_mapper, NULL);

    freed_cnt = 0;
    void * rejected = lv_malloc(16);
    lv_subject_set_pointer_owned(frame, rejected, counting_free_cb);

    /* The stored value never changed, so it was not released. */
    TEST_ASSERT_EQUAL(0, freed_cnt);
    TEST_ASSERT_EQUAL_PTR(kept, lv_subject_get_pointer(frame));

    /* The rejected input is not leaked either: the Subject retains and owns it, so
     * writing a third value releases it. */
    void * third = lv_malloc(16);
    lv_subject_set_pointer_owned(frame, third, counting_free_cb);
    TEST_ASSERT_EQUAL(1, freed_cnt);
    TEST_ASSERT_EQUAL_PTR(rejected, last_freed);
}

void test_subject_owned_discarded_input_is_released_at_teardown(void)
{
    lv_subject_t * frame = subject_create(LV_SUBJECT_TYPE_POINTER);

    void * stored = lv_malloc(16);
    lv_subject_set_pointer_owned(frame, stored, counting_free_cb);

    lv_subject_set_pointer_mapper(frame, rejecting_owned_mapper, NULL);

    void * discarded = lv_malloc(16);
    lv_subject_set_pointer_owned(frame, discarded, counting_free_cb);
    TEST_ASSERT_EQUAL_PTR(stored, lv_subject_get_pointer(frame));

    /* Two distinct owned pointers now: the stored value and the retained input. */
    freed_cnt = 0;
    subject_delete(frame);
    TEST_ASSERT_EQUAL(2, freed_cnt);
}

/* Ownership follows what the write handed in. A mapper that publishes its own allocation
 * is responsible for it, and the Subject never releases it. */
static void * mapper_owned_block;

static bool allocating_mapper(lv_subject_t * subject, void * user_data, lv_subject_value_t input,
                              const void ** value)
{
    LV_UNUSED(subject);
    LV_UNUSED(user_data);
    LV_UNUSED(input);
    *value = mapper_owned_block;
    return true;
}

void test_subject_mapper_allocation_is_not_owned_by_the_subject(void)
{
    mapper_owned_block = lv_malloc(16);

    lv_subject_t * frame = subject_create(LV_SUBJECT_TYPE_POINTER);
    lv_subject_set_pointer_mapper(frame, allocating_mapper, NULL);

    void * handed_in = lv_malloc(16);
    freed_cnt = 0;
    lv_subject_set_pointer_owned(frame, handed_in, counting_free_cb);

    /* The mapper stored its own block, so the Subject does not own the stored value. */
    TEST_ASSERT_FALSE(lv_subject_is_value_owned(frame));
    TEST_ASSERT_EQUAL_PTR(mapper_owned_block, lv_subject_get_pointer(frame));

    /* The handed-in pointer is still owned as the retained input, so it goes at teardown
     * while the mapper's own block does not. */
    freed_cnt = 0;
    subject_delete(frame);
    TEST_ASSERT_EQUAL(1, freed_cnt);
    TEST_ASSERT_EQUAL_PTR(handed_in, last_freed);

    lv_free(mapper_owned_block);   /* the mapper's, as documented */
}

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

static bool sample_changed_mapper(lv_subject_t * subject, void * user_data, lv_subject_value_t input,
                                  const void ** value)
{
    LV_UNUSED(subject);
    sample_watch_t * watch = user_data;
    const sample_t * now = input.pointer;
    if(now == NULL) return false;

    if(watch->seeded && lv_memcmp(&watch->last, now, sizeof(*now)) == 0) {
        return false;   /* same contents: not a change */
    }

    lv_memcpy(&watch->last, now, sizeof(*now));
    watch->seeded = true;
    *value = now;
    return true;
}

void test_subject_pointer_mapper_detects_mutation_behind_the_pointer(void)
{
    static sample_t sample;
    static sample_watch_t watch;
    lv_memzero(&watch, sizeof(watch));

    sample.x = 1;
    sample.y = 2;

    lv_subject_t * samples = subject_create(LV_SUBJECT_TYPE_POINTER);
    lv_subject_set_pointer_mapper(samples, sample_changed_mapper, &watch);

    observer_called = 0;
    lv_subject_add_observer(samples, observer_basic, NULL);
    observer_called = 0;

    /* First publish: a change. */
    lv_subject_set_pointer(samples, &sample);
    TEST_ASSERT_EQUAL(1, observer_called);

    /* Same pointer, same contents: NOT a change, even though a plain pointer Subject
     * would have notified. */
    lv_subject_set_pointer(samples, &sample);
    TEST_ASSERT_EQUAL(1, observer_called);

    /* Same pointer, mutated contents: a change, which is the case this exists for. */
    sample.y = 99;
    lv_subject_set_pointer(samples, &sample);
    TEST_ASSERT_EQUAL(2, observer_called);

    /* And again unchanged. */
    lv_subject_set_pointer(samples, &sample);
    TEST_ASSERT_EQUAL(2, observer_called);
}


/*=====================================================================
 * Where the mapper cannot be deferred
 *====================================================================*/

/* A Subject that owns its value has to evaluate on every write. Ownership of an
 * incoming pointer is only transferred once the mapper stores it, so if a write were
 * deferred a second write could supersede an input that was never stored, and nothing
 * would ever release it. */
void test_subject_owned_value_is_not_deferred(void)
{
    lv_subject_t * frame = subject_create(LV_SUBJECT_TYPE_POINTER);
    lv_subject_set_pointer_mapper(frame, inspecting_owned_mapper, NULL);

    /* No observers and mode LAZY, so this would be deferred if ownership allowed it. */
    TEST_ASSERT_FALSE(lv_subject_is_eager(frame));

    freed_cnt = 0;
    uint8_t * first = lv_malloc(16);
    first[0] = 1;
    lv_subject_set_pointer_owned(frame, first, counting_free_cb);
    /* Stored straight away, not on a later read. */
    TEST_ASSERT_FALSE(lv_subject_is_dirty(frame));
    TEST_ASSERT_EQUAL_PTR(first, frame->value.pointer);

    uint8_t * second = lv_malloc(16);
    second[0] = 2;
    lv_subject_set_pointer_owned(frame, second, counting_free_cb);

    /* Every write is accounted for, so nothing leaks. */
    TEST_ASSERT_EQUAL(1, freed_cnt);
    TEST_ASSERT_EQUAL_PTR(first, last_freed);
    TEST_ASSERT_EQUAL_PTR(second, frame->value.pointer);
}

/* A string Subject cannot defer either: `last_input` has nowhere to keep a string, so
 * the written text would be lost. */
void test_subject_string_mapper_is_not_deferred(void)
{
    static char buf[32];
    lv_subject_t * text = subject_create(LV_SUBJECT_TYPE_STRING);
    lv_subject_set_string_buffer_static(text, buf, sizeof(buf));
    lv_subject_set_string_mapper(text, uppercase_mapper, NULL);

    /* Lazy and unobserved, and still applied on the spot. */
    TEST_ASSERT_FALSE(lv_subject_is_eager(text));

    lv_subject_copy_string(text, "hello");
    TEST_ASSERT_FALSE(lv_subject_is_dirty(text));
    TEST_ASSERT_EQUAL_STRING("HELLO", buf);

    lv_subject_copy_string(text, "world");
    TEST_ASSERT_EQUAL_STRING("WORLD", buf);
}

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
/* Set with a float temperature, observed as a level. One Subject, not two. */
static bool temperature_level_mapper(lv_subject_t * subject, void * user_data, lv_subject_value_t input,
                                     int32_t * value)
{
    LV_UNUSED(subject);
    LV_UNUSED(user_data);
    int32_t next = input.float_v > 80.0f ? LEVEL_CRITICAL :
                   input.float_v > 60.0f ? LEVEL_HIGH : LEVEL_NORMAL;
    if(next == *value) return false;
    *value = next;
    return true;
}

void test_subject_mapped_float_in_int_out(void)
{
    lv_subject_t * level = lv_subject_create_mapped(LV_SUBJECT_TYPE_FLOAT, LV_SUBJECT_TYPE_INT);
    TEST_ASSERT_NOT_NULL(level);
    subjects[0] = subjects[0];   /* not registered: deleted at the end of the test */

    TEST_ASSERT_EQUAL(LV_SUBJECT_TYPE_FLOAT, lv_subject_get_input_type(level));
    lv_subject_set_int_mapper(level, temperature_level_mapper, NULL);

    observer_called = 0;
    lv_subject_add_observer(level, observer_basic, NULL);
    observer_called = 0;

    /* Written as a float... */
    lv_subject_set_float(level, 20.0f);
    /* ...read as an int. */
    TEST_ASSERT_EQUAL(LEVEL_NORMAL, lv_subject_get_int(level));

    lv_subject_set_float(level, 72.5f);
    TEST_ASSERT_EQUAL(LEVEL_HIGH, lv_subject_get_int(level));
    TEST_ASSERT_EQUAL(1, observer_called);

    /* A different temperature in the same band is not a change, so nobody is told. */
    lv_subject_set_float(level, 75.0f);
    TEST_ASSERT_EQUAL(LEVEL_HIGH, lv_subject_get_int(level));
    TEST_ASSERT_EQUAL(1, observer_called);

    lv_subject_set_float(level, 90.0f);
    TEST_ASSERT_EQUAL(LEVEL_CRITICAL, lv_subject_get_int(level));
    TEST_ASSERT_EQUAL(2, observer_called);

    lv_subject_delete(level);
}

/* The same shape, producing a boolean "too hot". */
static bool too_hot_mapper(lv_subject_t * subject, void * user_data, lv_subject_value_t input, int32_t * value)
{
    LV_UNUSED(subject);
    LV_UNUSED(user_data);
    int32_t next = input.float_v > 70.0f ? 1 : 0;
    if(next == *value) return false;
    *value = next;
    return true;
}

void test_subject_mapped_float_in_bool_out(void)
{
    lv_subject_t * too_hot = lv_subject_create_mapped(LV_SUBJECT_TYPE_FLOAT, LV_SUBJECT_TYPE_INT);
    lv_subject_set_int_mapper(too_hot, too_hot_mapper, NULL);

    lv_obj_t * obj = lv_obj_create(lv_screen_active());
    lv_obj_bind_bool(obj, too_hot, lv_obj_set_hidden);

    lv_subject_set_float(too_hot, 20.0f);
    TEST_ASSERT_FALSE(lv_obj_is_hidden(obj));

    lv_subject_set_float(too_hot, 85.0f);
    TEST_ASSERT_TRUE(lv_obj_is_hidden(obj));

    lv_obj_delete(obj);
    lv_subject_delete(too_hot);
}
#endif /*LV_USE_FLOAT*/

/* Writing the wrong type is refused, as it is for an ordinary Subject. */
void test_subject_mapped_rejects_the_wrong_input_type(void)
{
    lv_subject_t * level = lv_subject_create_mapped(LV_SUBJECT_TYPE_POINTER, LV_SUBJECT_TYPE_INT);
    lv_subject_set_int_mapper(level, recording_mapper, NULL);

    /* The input type is POINTER, so an int write is not accepted. */
    lv_subject_set_int(level, 5);
    TEST_ASSERT_EQUAL(0, lv_subject_get_int(level));

    lv_subject_delete(level);
}

/* Differing types with no mapper has nothing to convert with, so a write is refused
 * rather than silently storing something meaningless. */
void test_subject_mapped_without_a_mapper_refuses_writes(void)
{
    lv_subject_t * level = lv_subject_create_mapped(LV_SUBJECT_TYPE_POINTER, LV_SUBJECT_TYPE_INT);

    static int32_t dummy = 7;
    lv_subject_set_pointer(level, &dummy);
    TEST_ASSERT_EQUAL(0, lv_subject_get_int(level));

    lv_subject_delete(level);
}

void test_subject_ordinary_input_type_matches_its_value(void)
{
    lv_subject_t * plain = subject_create(LV_SUBJECT_TYPE_INT);
    TEST_ASSERT_EQUAL(LV_SUBJECT_TYPE_INT, lv_subject_get_input_type(plain));
}

/*=====================================================================
 * A clamped Subject takes its bounds through its own setter
 *====================================================================*/

void test_subject_clamped_bounds_are_writable(void)
{
    lv_subject_t * reading = subject_create(LV_SUBJECT_TYPE_INT);

    lv_subject_value_t lo;
    lv_subject_value_t hi;
    lv_memzero(&lo, sizeof(lo));
    lv_memzero(&hi, sizeof(hi));
    lo.num = 1;
    hi.num = 25;

    lv_subject_t * bounded = lv_subject_create_clamped(reading, lo, hi, NULL);
    TEST_ASSERT_NOT_NULL(bounded);
    /* Written with a range, whatever its value type is. */
    TEST_ASSERT_EQUAL(LV_SUBJECT_TYPE_POINTER, lv_subject_get_input_type(bounded));

    observer_called = 0;
    lv_subject_add_observer(bounded, observer_basic, NULL);
    observer_called = 0;

    /* Bounded to 1..25, and the reading is 30. */
    lv_subject_set_int(reading, 30);
    TEST_ASSERT_EQUAL(25, lv_subject_get_int(bounded));
    TEST_ASSERT_EQUAL(1, observer_called);

    /* Widen the maximum: the reading it had clamped away comes back. */
    lv_subject_range_t wider;
    wider.min_value.num = 1;
    wider.max_value.num = 100;
    lv_subject_set_pointer(bounded, &wider);
    TEST_ASSERT_EQUAL(30, lv_subject_get_int(bounded));
    TEST_ASSERT_EQUAL(2, observer_called);

    /* Raise the minimum above the reading: it is pushed up. */
    lv_subject_range_t higher;
    higher.min_value.num = 50;
    higher.max_value.num = 100;
    lv_subject_set_pointer(bounded, &higher);
    TEST_ASSERT_EQUAL(50, lv_subject_get_int(bounded));
    TEST_ASSERT_EQUAL(3, observer_called);

    /* The source was never touched by any of this. */
    TEST_ASSERT_EQUAL(30, lv_subject_get_int(reading));

    /* A range that changes nothing notifies nobody. */
    lv_subject_range_t same;
    same.min_value.num = 50;
    same.max_value.num = 100;
    lv_subject_set_pointer(bounded, &same);
    TEST_ASSERT_EQUAL(3, observer_called);

    lv_subject_delete(bounded);
}

/* The range is a retained pointer input, so it has to stay valid until a new range is
 * written. A static is the normal way to hold one. */
void test_subject_clamped_range_must_stay_valid(void)
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

    static lv_subject_range_t range;
    range.min_value.num = 0;
    range.max_value.num = 200;
    lv_subject_set_pointer(bounded, &range);
    TEST_ASSERT_EQUAL(200, lv_subject_get_int(bounded));

    /* Still valid, so a source change re-clamps against it correctly. */
    lv_subject_set_int(reading, 150);
    TEST_ASSERT_EQUAL(150, lv_subject_get_int(bounded));

    lv_subject_set_int(reading, 900);
    TEST_ASSERT_EQUAL(200, lv_subject_get_int(bounded));

    lv_subject_delete(bounded);
}


/*=====================================================================
 * Lifetime of a pointer value
 *====================================================================*/


/* The same holds when a mapper is what re-stores the pointer. */
void test_subject_owned_value_not_freed_when_mapper_restores_it(void)
{
    static sample_watch_t watch;
    lv_memzero(&watch, sizeof(watch));

    lv_subject_t * samples = subject_create(LV_SUBJECT_TYPE_POINTER);
    lv_subject_set_pointer_mapper(samples, sample_changed_mapper, &watch);

    sample_t * s = lv_malloc(sizeof(sample_t));
    s->x = 1;
    s->y = 2;
    lv_subject_set_pointer_owned(samples, s, counting_free_cb);

    freed_cnt = 0;

    /* Mutated in place and republished: the mapper stores the same pointer, so it is
     * not released even though the value "changed". */
    s->y = 3;
    lv_subject_set_pointer_owned(samples, s, counting_free_cb);
    TEST_ASSERT_EQUAL(0, freed_cnt);

    /* Republished unchanged: the mapper reports no change, still not released. */
    lv_subject_set_pointer_owned(samples, s, counting_free_cb);
    TEST_ASSERT_EQUAL(0, freed_cnt);

    TEST_ASSERT_EQUAL_PTR(s, lv_subject_get_pointer(samples));
    TEST_ASSERT_EQUAL(3, ((const sample_t *)lv_subject_get_pointer(samples))->y);
}




/* A pointer input IS retained, so a dependency-driven re-run gets the same pointer the
 * last write carried. That is what lets a mapper re-derive from its input rather than
 * only from what it stored, and it is why the caller has to keep that pointer valid
 * until a new input is written. */
static const void * rerun_seen_input;
static uint32_t rerun_count;

static bool retention_mapper(lv_subject_t * subject, void * user_data, lv_subject_value_t input,
                             const void ** value)
{
    LV_UNUSED(subject);
    LV_UNUSED(user_data);
    rerun_count++;
    rerun_seen_input = input.pointer;

    /* A dependency, so a change to it re-runs this mapper. */
    (void)lv_subject_get_int(dep_a);

    *value = input.pointer;
    return true;
}

void test_subject_pointer_input_is_retained(void)
{
    dep_a = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int(dep_a, 1);

    lv_subject_t * subject = subject_create(LV_SUBJECT_TYPE_POINTER);
    lv_subject_set_pointer_mapper(subject, retention_mapper, NULL);
    lv_subject_set_mode(subject, LV_SUBJECT_MODE_EAGER);

    /* Static, because the Subject keeps referring to it until a new input arrives. */
    static int32_t payload = 7;
    rerun_count = 0;
    lv_subject_set_pointer(subject, &payload);

    TEST_ASSERT_EQUAL_PTR(&payload, rerun_seen_input);
    TEST_ASSERT_EQUAL_PTR(&payload, lv_subject_get_pointer(subject));

    rerun_seen_input = NULL;
    lv_subject_set_int(dep_a, 2);   /* dependency change -> re-run */

    /* The re-run saw the retained pointer, not NULL. */
    TEST_ASSERT_EQUAL(2, rerun_count);
    TEST_ASSERT_EQUAL_PTR(&payload, rerun_seen_input);
    TEST_ASSERT_EQUAL_PTR(&payload, lv_subject_get_pointer(subject));
}

/* A source change re-runs the clamp mapper with the range it retained, which is exactly
 * why that range has to stay valid. */
void test_subject_clamped_reclamps_on_source_change(void)
{
    lv_subject_t * reading = subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int(reading, 5);

    lv_subject_value_t lo;
    lv_subject_value_t hi;
    lv_memzero(&lo, sizeof(lo));
    lv_memzero(&hi, sizeof(hi));
    lo.num = 0;
    hi.num = 10;
    lv_subject_t * bounded = lv_subject_create_clamped(reading, lo, hi, NULL);

    static lv_subject_range_t wide;
    wide.min_value.num = 0;
    wide.max_value.num = 100;
    lv_subject_set_pointer(bounded, &wide);
    TEST_ASSERT_EQUAL(5, lv_subject_get_int(bounded));

    lv_subject_set_int(reading, 60);
    TEST_ASSERT_EQUAL(60, lv_subject_get_int(bounded));

    lv_subject_set_int(reading, 500);
    TEST_ASSERT_EQUAL(100, lv_subject_get_int(bounded));

    lv_subject_delete(bounded);
}


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
static bool accumulate_mapper(lv_subject_t * subject, void * user_data, lv_subject_value_t input,
                              int32_t * value)
{
    LV_UNUSED(subject);
    LV_UNUSED(user_data);
    LV_UNUSED(input);
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

#endif
