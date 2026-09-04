/**
 * @file lv_observer.c
 *
 */

/*********************
 *      INCLUDES
 *********************/

#include "lv_observer_private.h"
#if LV_USE_OBSERVER

#include "../lvgl_public.h"
#include "../core/lv_global.h"
#include "../core/lv_obj_private.h"
#include "../misc/lv_event_private.h"
/*********************
 *      DEFINES
 *********************/


#define subject_list LV_GLOBAL_DEFAULT()->subject_ll

/**********************
 *      TYPEDEFS
 **********************/
typedef enum {
    FLAG_COND_EQ = 0,
    FLAG_COND_GT = 1,
    FLAG_COND_GE = 2
} flag_cond_t;

typedef struct {
    uint32_t flag;
    lv_subject_value_t value;
    uint32_t inv     : 1;
    flag_cond_t cond : 3;
} flag_and_cond_t;

/* One edge of the dependency graph. `seen_version` is the dependency's version at the
 * moment the mapper last read it, which is what lets a dependent skip its mapper when
 * nothing it reads has actually changed. */
typedef struct {
    lv_subject_t * subject;
    uint32_t seen_version;
} subject_ref_t;

typedef struct {
    lv_subject_t * subject;
    int32_t value;
} subject_set_int_user_data_t;

typedef struct {
    lv_subject_t * subject;
    float value;
} subject_set_float_user_data_t;

typedef struct {
    lv_subject_t * subject;
    const char * value;
} subject_set_string_user_data_t;

/**********************
 *  STATIC PROTOTYPES
 **********************/

static void subject_toggle_cb(lv_event_t * e);
static void subject_set_int_cb(lv_event_t * e);
#if LV_USE_FLOAT
    static void subject_set_float_cb(lv_event_t * e);
#endif

static void subject_set_string_cb(lv_event_t * e);
static void subject_increment_cb(lv_event_t * e);

static void unsubscribe_on_delete_cb(lv_event_t * e);
static lv_observer_t * bind_to_bitfield(lv_subject_t * subject, lv_obj_t * obj, lv_observer_cb_t cb, uint32_t flag,
                                        int32_t ref_value, bool inv, flag_cond_t cond);

static void obj_flag_observer_cb(lv_observer_t * observer, lv_subject_t * subject);
static void obj_state_observer_cb(lv_observer_t * observer, lv_subject_t * subject);
static void obj_value_changed_event_cb(lv_event_t * e);


static void subject_set_string_free_user_data_event_cb(lv_event_t * e);

static void set_bool_observer(lv_observer_t * observer, lv_subject_t * subject);
static void set_int_observer(lv_observer_t * observer, lv_subject_t * subject);
static void set_string_observer(lv_observer_t * observer, lv_subject_t * subject);
static void set_color_observer(lv_observer_t * observer, lv_subject_t * subject);
static void set_pointer_observer(lv_observer_t * observer, lv_subject_t * subject);
static void set_style_int_observer(lv_observer_t * observer, lv_subject_t * subject);
static void set_style_color_observer(lv_observer_t * observer, lv_subject_t * subject);
static void set_style_opa_observer(lv_observer_t * observer, lv_subject_t * subject);
static lv_observer_t * bind_style(lv_obj_t * obj, lv_subject_t * subject, lv_observer_cb_t cb,
                                  void (*setter)(void), lv_style_selector_t selector);

#if LV_USE_FLOAT
    static void set_float_observer(lv_observer_t * observer, lv_subject_t * subject);
    static void init_float(lv_subject_t * subject, float value);
#endif /*LV_USE_FLOAT*/

static void init_int(lv_subject_t * subject, int32_t value);
static void init_string(lv_subject_t * subject, char * buf, size_t size, const char * value);
static void init_pointer(lv_subject_t * subject, void * value);
static void init_color(lv_subject_t * subject, lv_color_t color);
static void init_common(lv_subject_t * subject);

static void deinit(lv_subject_t * subject);

/* Dependency graph */
static bool ref_list_contains(lv_ll_t * list, const lv_subject_t * subject);
static bool ref_list_add(lv_ll_t * list, lv_subject_t * subject);
static void ref_list_remove(lv_ll_t * list, const lv_subject_t * subject);
static uint32_t ref_list_count(const lv_ll_t * list);
static void deps_clear(lv_subject_t * subject);
static void edges_teardown(lv_subject_t * subject);

/* Dirty bookkeeping. Dirty Subjects are kept as a prefix of the global Subject list. */
static bool needs_scanning(lv_subject_t * subject);
static void list_reposition(lv_subject_t * subject);
static void mark_dirty(lv_subject_t * subject);
static void mark_pending_notify(lv_subject_t * subject);
static void clear_pending(lv_subject_t * subject);
static void mark_dependents_dirty(lv_subject_t * subject);
static void process_dirty(bool include_observed);
static void flush_timer_cb(lv_timer_t * timer);
static void flush_timer_update(void);

/* Evaluation */
static bool subject_is_eager_now(const lv_subject_t * subject);
static bool write_allowed(void);
/* Release a pointer, but only if the Subject owns it and nothing refers to it any more.
 *
 * A Subject refers to a pointer from two places: the value it stores and the input it
 * retains. Releasing while either still points at it would be a use-after-free, which is
 * what makes republishing the same pointer — a driver mutating its buffer in place —
 * safe. */
static void release_if_unreferenced(lv_subject_t * subject, void * p, bool owned)
{
    if(p == NULL || !owned) return;
    if(p == subject->value.pointer) return;
    if(p == subject->last_input.pointer) return;

    if(subject->value_free_cb) subject->value_free_cb(p);
    else lv_free(p);
}

static bool input_convertible(const lv_subject_t * subject);
static void set_pointer_value(lv_subject_t * subject, const void * ptr, bool owned,
                              lv_subject_value_free_cb_t free_cb);
static bool buf_reserve(lv_subject_t * subject, size_t needed, bool warn);
static void buf_release(lv_subject_t * subject);
static size_t buf_capacity(const lv_subject_t * subject);
static void release_if_unreferenced(lv_subject_t * subject, void * p, bool owned);
static bool apply_input(lv_subject_t * subject, const void * borrowed_input);
static bool run_mapper(lv_subject_t * subject, const void * borrowed_input);
static bool can_defer_mapper(const lv_subject_t * subject);
static void subject_input_written(lv_subject_t * subject, const void * borrowed_input,
                                  void * superseded_input, bool superseded_input_owned);
static void subject_commit_input(lv_subject_t * subject, const void * borrowed_input,
                                 void * superseded_input, bool superseded_input_owned);
static bool deps_are_unchanged(lv_subject_t * subject);
static void subject_recompute(lv_subject_t * subject);
static void subject_pull(lv_subject_t * subject);
static void notify(lv_subject_t * subject);
static void settle_before_subscribe(lv_subject_t * subject);
static bool set_mapper_allowed(lv_subject_t * subject, lv_subject_type_t type);

/* Derived-subject helpers */
static int natural_compare(lv_subject_t * subject, lv_subject_value_t a, lv_subject_value_t b);
static lv_subject_value_t read_source(lv_subject_t * source);
static bool extremum_step(lv_subject_t * subject, void * user_data, lv_subject_value_t * value);
static bool clamp_step(lv_subject_t * subject, void * user_data, lv_subject_value_t input,
                       lv_subject_value_t * value);
static bool ordering_available(const lv_subject_t * source, lv_subject_compare_cb_t compare_cb);

/* Observer mappers */
static lv_observer_t * bind_mapped(lv_obj_t * obj, lv_subject_t * subject, lv_observer_cb_t cb, void (*set_cb)(void),
                                   lv_observer_mapper_t mapper, void * user_data, lv_style_selector_t selector);

/**********************
 *  STATIC VARIABLES
 **********************/

/**********************
 *      MACROS
 **********************/

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

void lv_subject_global_init(void)
{
    lv_global_t * global = LV_GLOBAL_DEFAULT();
    global->subject_evaluating = NULL;
    global->subject_flush_timer = NULL;
    global->subject_flushing = 0;
    lv_ll_init(&subject_list, sizeof(lv_subject_t));
}
void lv_subject_global_deinit(void)
{
    lv_global_t * global = LV_GLOBAL_DEFAULT();
    /* Only drop the reference. `lv_timer_core_deinit()` runs earlier in `lv_deinit()`
     * and has already destroyed every timer, so deleting it here would free it twice. */
    global->subject_flush_timer = NULL;
    global->subject_evaluating = NULL;

    /* Cascade from the head, one Subject at a time. A single cascade may take several
     * Subjects with it, which is exactly what is wanted here, and it means no ordering
     * between sources and their derived Subjects has to be worked out. Each round frees
     * at least the head, so this drains the list. */
    lv_subject_t * curr;
    while((curr = lv_ll_get_head(&subject_list)) != NULL) {
        lv_subject_delete_cascade(curr);
    }
}

/*---------------------------------------------------------------
 * Dependency graph
 *
 * `deps` holds the Subjects a Subject read during its last evaluation, `dependents`
 * the reverse edges. Both are rebuilt as needed and keep their capacity, so a
 * steady-state re-evaluation does not allocate.
 *--------------------------------------------------------------*/

void lv_subject_track_dependency(lv_subject_t * subject)
{
    lv_subject_t * reader = LV_GLOBAL_DEFAULT()->subject_evaluating;
    if(reader == NULL || reader == subject) return;

    /* Already wired from an earlier read in the same evaluation? */
    if(ref_list_contains(&reader->deps, subject)) return;

    if(!ref_list_add(&reader->deps, subject)) return;
    /* The version the mapper is reading right now. */
    ((subject_ref_t *)lv_ll_get_tail(&reader->deps))->seen_version = subject->version;
    if(!ref_list_add(&subject->dependents, reader)) {
        /* Keep both directions consistent if the reverse edge could not be stored. */
        ref_list_remove(&reader->deps, subject);
    }
}

uint32_t lv_subject_get_dependency_count(const lv_subject_t * subject)
{
    LV_CHECK_ARG(subject != NULL, return 0);
    return ref_list_count(&subject->deps);
}

lv_subject_t * lv_subject_get_dependency(const lv_subject_t * subject, uint32_t index)
{
    LV_CHECK_ARG(subject != NULL, return NULL);

    uint32_t i = 0;
    subject_ref_t * ref;
    LV_LL_READ(&subject->deps, ref) {
        if(i == index) return ref->subject;
        i++;
    }
    LV_LOG_WARN("Dependency index %" LV_PRIu32 " is out of bounds", index);
    return NULL;
}

/*---------------------------------------------------------------
 * Mode and mapper
 *--------------------------------------------------------------*/

void lv_subject_set_mode(lv_subject_t * subject, lv_subject_mode_t mode)
{
    LV_CHECK_ARG(subject != NULL, return);

    bool was_eager = subject_is_eager_now(subject);
    subject->mode = (mode == LV_SUBJECT_MODE_EAGER) ? 1U : 0U;

    /* Becoming eager makes any pending work due immediately, and may have moved the
     * Subject into the scanned region. */
    if(!was_eager && subject_is_eager_now(subject)) {
        list_reposition(subject);
        if(subject->dirty || subject->pending_notify) process_dirty(false);
    }
    flush_timer_update();
}

lv_subject_mode_t lv_subject_get_mode(const lv_subject_t * subject)
{
    LV_CHECK_ARG(subject != NULL, return LV_SUBJECT_MODE_LAZY);
    return subject->mode ? LV_SUBJECT_MODE_EAGER : LV_SUBJECT_MODE_LAZY;
}

bool lv_subject_is_eager(const lv_subject_t * subject)
{
    LV_CHECK_ARG(subject != NULL, return false);
    return subject_is_eager_now(subject);
}

bool lv_subject_is_dirty(const lv_subject_t * subject)
{
    LV_CHECK_ARG(subject != NULL, return false);
    return subject->dirty;
}

void lv_subject_set_user_data(lv_subject_t * subject, void * user_data)
{
    LV_CHECK_ARG(subject != NULL, return);
    subject->user_data = user_data;
}

void * lv_subject_get_user_data(const lv_subject_t * subject)
{
    LV_CHECK_ARG(subject != NULL, return NULL);
    return subject->user_data;
}

void lv_subject_set_int_mapper(lv_subject_t * subject, lv_subject_int_mapper_t mapper, void * user_data)
{
    if(!set_mapper_allowed(subject, LV_SUBJECT_TYPE_INT)) return;
    subject->mapper.int_cb = mapper;
    subject->mapper_user_data = user_data;
    subject->has_mapper = mapper != NULL;
    if(mapper) subject_recompute(subject);
}

#if LV_USE_FLOAT
void lv_subject_set_float_mapper(lv_subject_t * subject, lv_subject_float_mapper_t mapper, void * user_data)
{
    if(!set_mapper_allowed(subject, LV_SUBJECT_TYPE_FLOAT)) return;
    subject->mapper.float_cb = mapper;
    subject->mapper_user_data = user_data;
    subject->has_mapper = mapper != NULL;
    if(mapper) subject_recompute(subject);
}
#endif

void lv_subject_set_pointer_mapper(lv_subject_t * subject, lv_subject_pointer_mapper_t mapper, void * user_data)
{
    if(!set_mapper_allowed(subject, LV_SUBJECT_TYPE_POINTER)) return;
    subject->mapper.pointer_cb = mapper;
    subject->mapper_user_data = user_data;
    subject->has_mapper = mapper != NULL;
    if(mapper) subject_recompute(subject);
}

void lv_subject_set_color_mapper(lv_subject_t * subject, lv_subject_color_mapper_t mapper, void * user_data)
{
    if(!set_mapper_allowed(subject, LV_SUBJECT_TYPE_COLOR)) return;
    subject->mapper.color_cb = mapper;
    subject->mapper_user_data = user_data;
    subject->has_mapper = mapper != NULL;
    if(mapper) subject_recompute(subject);
}

void lv_subject_set_string_mapper(lv_subject_t * subject, lv_subject_string_mapper_t mapper, void * user_data)
{
    if(!set_mapper_allowed(subject, LV_SUBJECT_TYPE_STRING)) return;
    if(mapper != NULL && subject->buf == NULL && subject->value.pointer == NULL) {
        LV_LOG_WARN("Set the string buffer with lv_subject_set_string_buffer_static() before the mapper");
        return;
    }
    subject->mapper.string_cb = mapper;
    subject->mapper_user_data = user_data;
    subject->has_mapper = mapper != NULL;
    if(mapper) subject_recompute(subject);
}

void lv_subject_set_none_mapper(lv_subject_t * subject, lv_subject_none_mapper_t mapper, void * user_data)
{
    LV_CHECK_ARG(subject != NULL, return);
    LV_CHECK_ARG(subject->type == LV_SUBJECT_TYPE_NONE, return);
    if(!subject->in_list) {
        LV_LOG_WARN("A mapper on an application-owned Subject is never re-evaluated by lv_subject_flush()");
    }
    subject->mapper.none_cb = mapper;
    subject->mapper_user_data = user_data;
    subject->has_mapper = mapper != NULL;
    if(mapper) subject_recompute(subject);
}

bool lv_subject_is_value_owned(const lv_subject_t * subject)
{
    LV_CHECK_ARG(subject != NULL, return false);
    return subject->owns_value;
}

void lv_subject_flush(void)
{
    process_dirty(true);
}


lv_subject_t * lv_subject_create(lv_subject_type_t type)
{
    LV_CHECK_ARG(type != LV_SUBJECT_TYPE_INVALID, return NULL);
    LV_CHECK_ARG(LV_USE_FLOAT || type != LV_SUBJECT_TYPE_FLOAT, return NULL);
    lv_subject_t * subject = lv_ll_ins_tail(&subject_list);
    LV_ASSERT_MALLOC(subject);
    if(!subject) {
        return NULL;
    }

    init_common(subject);
    subject->in_list = 1;
    subject->input_type = (uint32_t)type;

    switch(type) {
        case LV_SUBJECT_TYPE_INT:
            init_int(subject, 0);
            break;
        case LV_SUBJECT_TYPE_FLOAT:
#if LV_USE_FLOAT
            init_float(subject, 0.f);
#endif /*LV_USE_FLOAT*/
            break;
        case LV_SUBJECT_TYPE_POINTER:
            init_pointer(subject, NULL);
            break;
        case LV_SUBJECT_TYPE_COLOR:
            init_color(subject, lv_color_black());
            break;
        case LV_SUBJECT_TYPE_STRING:
            init_string(subject, NULL, 0, "");
            break;
        case LV_SUBJECT_TYPE_NONE:
            /* No value of its own; it only aggregates other Subjects through its mapper.
             * Eager by default, so its Observers fire as soon as an input changes. */
            subject->type = LV_SUBJECT_TYPE_NONE;
            subject->mode = 1U; /* LV_SUBJECT_MODE_EAGER */
            break;
        case LV_SUBJECT_TYPE_INVALID:
            LV_UNREACHABLE();
    }
    return subject;
}

/* Tear down and free, with no dependency check. The caller has to have established
 * that nothing depends on `subject`, or be deleting the whole graph. */
static void subject_destroy(lv_subject_t * subject)
{
    deinit(subject);
    lv_ll_remove(&subject_list, subject);
    lv_free(subject);
}

lv_subject_t * lv_subject_create_mapped(lv_subject_type_t input_type, lv_subject_type_t value_type)
{
    LV_CHECK_ARG(input_type != LV_SUBJECT_TYPE_INVALID, return NULL);
    LV_CHECK_ARG(value_type != LV_SUBJECT_TYPE_INVALID, return NULL);
    LV_CHECK_ARG(LV_USE_FLOAT || (input_type != LV_SUBJECT_TYPE_FLOAT && value_type != LV_SUBJECT_TYPE_FLOAT),
                 return NULL);
    /* An LV_SUBJECT_TYPE_NONE Subject has no value, so there is nothing to convert to. */
    LV_CHECK_ARG(value_type != LV_SUBJECT_TYPE_NONE, return NULL);

    lv_subject_t * subject = lv_subject_create(value_type);
    if(subject == NULL) return NULL;

    subject->input_type = (uint32_t)input_type;
    return subject;
}

lv_subject_type_t lv_subject_get_input_type(const lv_subject_t * subject)
{
    LV_CHECK_ARG(subject != NULL, return LV_SUBJECT_TYPE_INVALID);
    return (lv_subject_type_t)subject->input_type;
}

void lv_subject_delete(lv_subject_t * subject)
{
    if(!subject) {
        return;
    }
    if(!subject->in_list) {
        LV_LOG_WARN("Use lv_subject_deinit() for a Subject set up with lv_subject_init_...()");
        return;
    }
    uint32_t dependents = ref_list_count(&subject->dependents);
    if(dependents > 0) {
        LV_LOG_WARN("Subject is a dependency of %" LV_PRIu32 " other Subject(s). Delete those "
                    "first, or use lv_subject_delete_cascade().", dependents);
        return;
    }

    subject_destroy(subject);
}

void lv_subject_delete_cascade(lv_subject_t * subject)
{
    if(!subject) {
        return;
    }
    if(!subject->in_list) {
        LV_LOG_WARN("Use lv_subject_deinit() for a Subject set up with lv_subject_init_...()");
        return;
    }

    /* Depth first, like ON DELETE CASCADE. Each recursive call frees one Subject, and
     * freeing it drops its edge out of `dependents`, so the loop makes progress and a
     * Subject reachable by two paths is still deleted only once. */
    subject_ref_t * ref;
    while((ref = lv_ll_get_head(&subject->dependents)) != NULL) {
        lv_subject_delete_cascade(ref->subject);
    }

    subject_destroy(subject);
}

#if LV_USE_EXT_DATA
void lv_subject_set_external_data(lv_subject_t * subject, void * data, void (* free_cb)(void * data))
{
    LV_CHECK_ARG(subject != NULL, return);

    subject->ext_data.data = data;
    subject->ext_data.free_cb = free_cb;
}
#endif

void lv_subject_init_int(lv_subject_t * subject, int32_t value)
{
    LV_CHECK_ARG(subject != NULL, return);
    init_common(subject);
    init_int(subject, value);
}

void lv_subject_set_int(lv_subject_t * subject, int32_t value)
{
    LV_CHECK_ARG(subject != NULL, return);
    LV_CHECK_ARG(subject->input_type == LV_SUBJECT_TYPE_INT, return);

    if(!write_allowed()) return;
    if(!input_convertible(subject)) return;

    /* Record the input, then let the mapper decide what gets stored. A mapper that
     * ignores `input` and derives from its dependencies discards it. */
    subject->last_input.num = value;
    subject_input_written(subject, NULL, NULL, false);
}

int32_t lv_subject_get_int(lv_subject_t * subject)
{
    LV_CHECK_ARG(subject != NULL, return 0);
    LV_CHECK_ARG(subject->type == LV_SUBJECT_TYPE_INT, return 0);

    lv_subject_track_dependency(subject);
    subject_pull(subject);
    return subject->value.num;
}




#if LV_USE_FLOAT

void lv_subject_init_float(lv_subject_t * subject, float value)
{
    LV_CHECK_ARG(subject != NULL, return);

    init_common(subject);
    init_float(subject, value);
}

void lv_subject_set_float(lv_subject_t * subject, float value)
{
    LV_CHECK_ARG(subject != NULL, return);
    LV_CHECK_ARG(subject->input_type == LV_SUBJECT_TYPE_FLOAT, return);

    if(!write_allowed()) return;
    if(!input_convertible(subject)) return;

    subject->last_input.float_v = value;
    subject_input_written(subject, NULL, NULL, false);
}

float lv_subject_get_float(lv_subject_t * subject)
{
    LV_CHECK_ARG(subject != NULL, return 0.0);
    LV_CHECK_ARG(subject->type == LV_SUBJECT_TYPE_FLOAT, return 0.0);

    lv_subject_track_dependency(subject);
    subject_pull(subject);
    return subject->value.float_v;
}





#endif /*LV_USE_FLOAT*/

void lv_subject_init_string(lv_subject_t * subject, char * buf, size_t size, const char * value)
{
    LV_CHECK_ARG(subject != NULL, return);
    LV_CHECK_ARG(buf != NULL || size == 0, return);
    LV_CHECK_ARG(value != NULL, return);

    init_common(subject);
    init_string(subject, buf, size, value);
}

void lv_subject_set_buffer(lv_subject_t * subject, void * buf, size_t size,
                           lv_subject_realloc_cb_t realloc_cb, lv_subject_value_free_cb_t free_cb)
{
    LV_CHECK_ARG(subject != NULL, return);
    LV_CHECK_ARG(subject->type == LV_SUBJECT_TYPE_STRING || subject->type == LV_SUBJECT_TYPE_POINTER, return);
    /* Either something to start from, or a way to allocate. */
    LV_CHECK_ARG(buf != NULL || realloc_cb != NULL, return);

    /* Replacing a buffer releases the old one with its own free_cb. */
    buf_release(subject);

    lv_subject_buf_t * b = lv_malloc_zeroed(sizeof(lv_subject_buf_t));
    LV_ASSERT_MALLOC(b);
    if(b == NULL) return;

    b->buf = buf;
    b->capacity = buf ? size : 0;
    b->length = 0;
    b->realloc_cb = realloc_cb;
    b->free_cb = free_cb;

    subject->buf = b;
    subject->value.pointer = buf;

    /* A copying Subject never owns its value separately: the buffer is the value. */
    subject->owns_value = 0;
    subject->owns_input = 0;

    if(buf != NULL && size > 0 && subject->type == LV_SUBJECT_TYPE_STRING) {
        ((char *)buf)[0] = '\0';
    }
}

void lv_subject_set_string_buffer_static(lv_subject_t * subject, char * buf, size_t size)
{
    LV_CHECK_ARG(subject != NULL, return);
    LV_CHECK_ARG(subject->type == LV_SUBJECT_TYPE_STRING, return);
    LV_CHECK_ARG(buf != NULL, return);
    LV_CHECK_ARG(size > 0, return);

    lv_subject_set_buffer(subject, buf, size, NULL, NULL);
}

bool lv_subject_copy_pointer(lv_subject_t * subject, const void * data, size_t size)
{
    LV_CHECK_ARG(subject != NULL, return false);
    LV_CHECK_ARG(subject->input_type == LV_SUBJECT_TYPE_POINTER, return false);
    LV_CHECK_ARG(data != NULL || size == 0, return false);
    if(!write_allowed()) return false;
    if(!input_convertible(subject)) return false;

    if(!buf_reserve(subject, size, true)) return false;

    lv_subject_buf_t * b = subject->buf;

    /* The old bytes are still here, so proper change detection comes for free. */
    subject->copy_changed = (b->length != size) ||
                            (size > 0 && lv_memcmp(b->buf, data, size) != 0) ? 1U : 0U;

    if(size > 0) lv_memcpy(b->buf, data, size);
    b->length = size;

    subject->value.pointer = b->buf;
    subject->last_input.pointer = b->buf;
    subject_commit_input(subject, b->buf, NULL, false);
    return true;
}

size_t lv_subject_get_pointer_size(const lv_subject_t * subject)
{
    LV_CHECK_ARG(subject != NULL, return 0);
    return subject->buf ? subject->buf->length : 0;
}

bool lv_subject_copy_string(lv_subject_t * subject, const char * buf)
{
    LV_CHECK_ARG(subject != NULL, return false);
    LV_CHECK_ARG(subject->input_type == LV_SUBJECT_TYPE_STRING, return false);
    LV_CHECK_ARG(buf != NULL, return false);
    if(!write_allowed()) return false;
    if(!input_convertible(subject)) return false;

    size_t needed = lv_strlen(buf) + 1;
    if(!buf_reserve(subject, needed, true)) return false;

    lv_subject_buf_t * b = subject->buf;
    subject->copy_changed = lv_strcmp((const char *)b->buf, buf) != 0 ? 1U : 0U;

    /* The new string is handed to the mapper as `input`; the buffer still holds the
     * previous value, so a mapper can compare the two. */
    subject_commit_input(subject, buf, NULL, false);
    return true;
}

bool lv_subject_copy_string_trimmed(lv_subject_t * subject, const char * buf)
{
    LV_CHECK_ARG(subject != NULL, return false);
    LV_CHECK_ARG(subject->input_type == LV_SUBJECT_TYPE_STRING, return false);
    LV_CHECK_ARG(buf != NULL, return false);
    if(!write_allowed()) return false;
    if(!input_convertible(subject)) return false;

    size_t needed = lv_strlen(buf) + 1;
    /* Quietly, because not fitting is the expected case here rather than a problem. */
    (void)buf_reserve(subject, needed, false);

    size_t capacity = buf_capacity(subject);
    if(capacity == 0) {
        LV_LOG_WARN("This Subject has no buffer. Set one with lv_subject_set_buffer() before "
                    "copying into it.");
        return false;
    }

    /* Work out what will actually be stored, so change detection is about the trimmed
     * result rather than the string that was offered. Done in place, with no scratch
     * buffer, so this stays usable on a project that never allocates. */
    lv_subject_buf_t * b = subject->buf;
    const char * current = b->buf;
    size_t stored_len = lv_strlen(buf);
    if(stored_len > capacity - 1) stored_len = capacity - 1;

    subject->copy_changed = (lv_strlen(current) != stored_len ||
                             lv_memcmp(current, buf, stored_len) != 0) ? 1U : 0U;

    /* `apply_input()` copies with lv_strlcpy(), which trims to the capacity. */
    subject_commit_input(subject, buf, NULL, false);
    return true;
}

bool lv_subject_snprintf(lv_subject_t * subject, const char * format, ...)
{
    LV_CHECK_ARG(subject != NULL, return false);
    LV_CHECK_ARG(subject->input_type == LV_SUBJECT_TYPE_STRING, return false);
    LV_CHECK_ARG(format != NULL, return false);
    if(!write_allowed()) return false;
    if(!input_convertible(subject)) return false;

    /* Measure first, so a growable buffer can be grown to fit instead of truncating. */
    va_list va;
    va_start(va, format);
    int needed = lv_vsnprintf(NULL, 0, format, va);
    va_end(va);
    if(needed < 0) return false;

    size_t total = (size_t)needed + 1;
    if(!buf_reserve(subject, total, true)) return false;

    /* Format into a scratch buffer, because the Subject's buffer still has to hold the
     * previous value for the mapper and for change detection. */
    char * tmp = lv_malloc(total);
    LV_ASSERT_MALLOC(tmp);
    if(tmp == NULL) return false;

    va_start(va, format);
    lv_vsnprintf(tmp, total, format, va);
    va_end(va);

    lv_subject_buf_t * b = subject->buf;
    subject->copy_changed = lv_strcmp((const char *)b->buf, tmp) != 0 ? 1U : 0U;

    subject_commit_input(subject, tmp, NULL, false);
    lv_free(tmp);
    return true;
}

const char * lv_subject_get_string(lv_subject_t * subject)
{
    LV_CHECK_ARG(subject != NULL, return NULL);
    LV_CHECK_ARG(subject->type == LV_SUBJECT_TYPE_STRING, return NULL);

    lv_subject_track_dependency(subject);
    subject_pull(subject);
    return subject->value.pointer;
}


void lv_subject_init_pointer(lv_subject_t * subject, void * value)
{
    LV_CHECK_ARG(subject != NULL, return);

    init_common(subject);
    init_pointer(subject, value);
}

/* Shared write path for the pointer-shaped setters. `owned` says whether this call
 * hands the data over. */
static void set_pointer_value(lv_subject_t * subject, const void * ptr, bool owned,
                              lv_subject_value_free_cb_t free_cb)
{
    if(!write_allowed()) return;
    if(!input_convertible(subject)) return;

    /* The input this one supersedes may be the Subject's to release, and it carries its
     * own ownership: a borrowed write followed by an owned one, or the reverse, both
     * have to do the right thing. */
    void * superseded_input = (void *)subject->last_input.pointer;
    bool superseded_input_owned = subject->owns_input;

    subject->last_input.pointer = ptr;
    subject->owns_input = owned ? 1U : 0U;
    if(owned) subject->value_free_cb = free_cb;

    subject_input_written(subject, ptr, superseded_input, superseded_input_owned);
}

void lv_subject_set_pointer(lv_subject_t * subject, void * ptr)
{
    LV_CHECK_ARG(subject != NULL, return);
    LV_CHECK_ARG(subject->input_type == LV_SUBJECT_TYPE_POINTER, return);

    set_pointer_value(subject, ptr, false, NULL);
}

void lv_subject_set_pointer_owned(lv_subject_t * subject, void * ptr, lv_subject_value_free_cb_t free_cb)
{
    LV_CHECK_ARG(subject != NULL, return);
    LV_CHECK_ARG(subject->input_type == LV_SUBJECT_TYPE_POINTER, return);

    set_pointer_value(subject, ptr, true, free_cb);
}

void lv_subject_set_string(lv_subject_t * subject, const char * str)
{
    LV_CHECK_ARG(subject != NULL, return);
    LV_CHECK_ARG(subject->input_type == LV_SUBJECT_TYPE_STRING, return);
    if(subject->buf != NULL) {
        LV_LOG_WARN("This string Subject has a buffer, so it copies. Use lv_subject_copy_string(), "
                    "or create one without a buffer to store a pointer instead.");
        return;
    }

    set_pointer_value(subject, str, false, NULL);
}

void lv_subject_set_string_owned(lv_subject_t * subject, char * str, lv_subject_value_free_cb_t free_cb)
{
    LV_CHECK_ARG(subject != NULL, return);
    LV_CHECK_ARG(subject->input_type == LV_SUBJECT_TYPE_STRING, return);
    if(subject->buf != NULL) {
        LV_LOG_WARN("This string Subject has a buffer, so it copies. Use lv_subject_copy_string(), "
                    "or create one without a buffer to take ownership of a string instead.");
        return;
    }

    set_pointer_value(subject, str, true, free_cb);
}

const void * lv_subject_get_pointer(lv_subject_t * subject)
{
    LV_CHECK_ARG(subject != NULL, return NULL);
    LV_CHECK_ARG(subject->type == LV_SUBJECT_TYPE_POINTER, return NULL);

    lv_subject_track_dependency(subject);
    subject_pull(subject);
    return subject->value.pointer;
}


void lv_subject_init_color(lv_subject_t * subject, lv_color_t color)
{
    LV_CHECK_ARG(subject != NULL, return);

    init_common(subject);
    init_color(subject, color);
}

void lv_subject_set_color(lv_subject_t * subject, lv_color_t color)
{
    LV_CHECK_ARG(subject != NULL, return);
    LV_CHECK_ARG(subject->input_type == LV_SUBJECT_TYPE_COLOR, return);

    if(!write_allowed()) return;
    if(!input_convertible(subject)) return;

    subject->last_input.color = color;
    subject_input_written(subject, NULL, NULL, false);
}

lv_color_t lv_subject_get_color(lv_subject_t * subject)
{
    LV_CHECK_ARG(subject != NULL, return lv_color_black());
    LV_CHECK_ARG(subject->type == LV_SUBJECT_TYPE_COLOR, return lv_color_black());

    lv_subject_track_dependency(subject);
    subject_pull(subject);
    return subject->value.color;
}





void lv_subject_deinit(lv_subject_t * subject)
{
    if(subject == NULL) return;
    deinit(subject);
}

/* Bring a Subject up to date for a *new* Observer that is about to be linked.
 *
 * Both steps have to happen before linking, and in this order:
 * - a never-evaluated lazy Subject would hand the new Observer a stale value;
 * - a notification left pending from a time when the Subject had no eager Observers
 *   is owed to the Observers that already exist. Delivering it now, before linking,
 *   keeps it away from the new Observer, which gets its own initial callback instead.
 *   Leaving it pending would let it surface at an arbitrary later flush, long after
 *   the value it refers to was current.
 */
static void settle_before_subscribe(lv_subject_t * subject)
{
    subject_pull(subject);

    if(subject->pending_notify) {
        clear_pending(subject);
        notify(subject);
    }
}

lv_observer_t * lv_subject_add_observer(lv_subject_t * subject, lv_observer_cb_t observer_cb, void * user_data)
{
    LV_CHECK_ARG(subject != NULL, return NULL);
    LV_CHECK_ARG(observer_cb != NULL, return NULL);

    lv_observer_t * observer = lv_subject_add_observer_obj(subject, observer_cb, NULL, user_data);
    if(observer == NULL) return NULL;

    observer->for_obj = 0;
    return observer;
}

lv_observer_t * lv_subject_add_observer_obj(lv_subject_t * subject, lv_observer_cb_t observer_cb, lv_obj_t * obj,
                                            void * user_data)
{
    LV_CHECK_ARG(subject != NULL, return NULL);
    LV_CHECK_ARG(observer_cb != NULL, return NULL);
    LV_CHECK_ARG(subject->type != LV_SUBJECT_TYPE_INVALID, return NULL);

    settle_before_subscribe(subject);

    lv_observer_t * observer = lv_ll_ins_tail(&(subject->subs_ll));
    LV_ASSERT_MALLOC(observer);
    if(observer == NULL) return NULL;

    lv_memzero(observer, sizeof(*observer));

    observer->subject = subject;
    observer->cb = observer_cb;
    observer->user_data = user_data;
    observer->target = obj;
    observer->for_obj = 1;
    subject->immediate_observer_cnt++;  /* LV_OBSERVER_MODE_IMMEDIATE is the default */
    /* subscribe to delete event of the object */
    if(obj != NULL) {
        lv_obj_add_event_cb(obj, unsubscribe_on_delete_cb, LV_EVENT_DELETE, observer);
    }

    /* Update Observer immediately. */
    observer->cb(observer, subject);

    return observer;
}

lv_observer_t * lv_subject_add_observer_with_target(lv_subject_t * subject, lv_observer_cb_t observer_cb, void * target,
                                                    void * user_data)
{
    LV_CHECK_ARG(subject != NULL, return NULL);
    LV_CHECK_ARG(observer_cb != NULL, return NULL);
    LV_CHECK_ARG(subject->type != LV_SUBJECT_TYPE_INVALID, return NULL);

    settle_before_subscribe(subject);

    lv_observer_t * observer = lv_ll_ins_tail(&(subject->subs_ll));
    LV_ASSERT_MALLOC(observer);
    if(observer == NULL) return NULL;

    lv_memzero(observer, sizeof(*observer));

    observer->subject = subject;
    observer->cb = observer_cb;
    observer->user_data = user_data;
    observer->target = target;
    subject->immediate_observer_cnt++;  /* LV_OBSERVER_MODE_IMMEDIATE is the default */

    /* Update Observer immediately. */
    observer->cb(observer, subject);

    return observer;
}


void lv_observer_delete(lv_observer_t * observer)
{
    if(observer == NULL) return;

    if(observer->for_obj && observer->target) {
        lv_obj_remove_event_cb_with_user_data(observer->target, unsubscribe_on_delete_cb, observer);
        lv_obj_remove_event_cb_with_user_data(observer->target, NULL, observer->subject);
    }

    observer->subject->notify_restart_query = 1;

    if(observer->mode == 0U && observer->subject->immediate_observer_cnt > 0) {
        observer->subject->immediate_observer_cnt--;
    }

    lv_ll_remove(&(observer->subject->subs_ll), observer);
    list_reposition(observer->subject);

    if(observer->auto_free_user_data) {
        lv_free(observer->user_data);
    }
    lv_free(observer);
}

void lv_obj_remove_from_subject(lv_obj_t * obj, lv_subject_t * subject)
{
    LV_CHECK_ARG(obj != NULL, return);
    /* subject == NULL is documented as valid: remove from ALL subjects */

    /*
     * Look for the `observer` that connects `obj` and `subject`
     * Since the obj is associated with the subject,
     *  the `obj` will have an LV_EVENT_REMOVE event with the `unsubscribe_on_delete_cb` callback
     *  associated.
     * From the event we can then find the observer in the event's `user_data` field
     */
    int32_t i;
    int32_t event_cnt = (int32_t)(obj->spec_attr ? lv_event_get_count(&obj->spec_attr->event_list) : 0);
    for(i = event_cnt - 1; i >= 0; i--) {
        lv_event_dsc_t * event_dsc = lv_obj_get_event_dsc(obj, i);
        if(event_dsc->cb == unsubscribe_on_delete_cb) {
            lv_observer_t * observer = event_dsc->user_data;
            if(subject == NULL || subject == observer->subject) {
                /* lv_observer_delete handles the deletion of all possible event callbacks */
                lv_observer_delete(observer);
            }
        }
    }
    /* Gracefully de-couple `subject` from Widget by deleting any existing
     * `LV_EVENT_VALUE_CHANGED` event associated with `subject` in case
     * one of the `..._bind_value()` functions was used. */
    lv_obj_remove_event_cb_with_user_data(obj, NULL, subject);

}

void * lv_observer_get_target(lv_observer_t * observer)
{
    LV_CHECK_ARG(observer != NULL, return NULL);

    return observer->target;
}

void lv_subject_notify(lv_subject_t * subject)
{
    LV_CHECK_ARG(subject != NULL, return);

    notify(subject);
}

lv_subject_increment_dsc_t * lv_obj_add_subject_increment_event(lv_obj_t * obj, lv_subject_t * subject,
                                                                lv_event_code_t trigger, int32_t step)
{
    LV_CHECK_ARG(obj != NULL, return NULL);
    LV_CHECK_ARG(subject != NULL, return NULL);
    LV_CHECK_ARG(subject->type == LV_SUBJECT_TYPE_INT || subject->type == LV_SUBJECT_TYPE_FLOAT, return NULL);
    /* It reads the value and writes it back, so both sides have to match. */
    LV_CHECK_ARG(subject->input_type == subject->type, return NULL);

    lv_subject_increment_dsc_t * user_data = lv_malloc(sizeof(lv_subject_increment_dsc_t));
    if(user_data == NULL) {
        LV_ASSERT_MALLOC(user_data);
        LV_LOG_WARN("Couldn't allocate user_data in in <lv_obj-subject_increment>");
        return NULL;
    }

    user_data->step = step;
    user_data->subject = subject;
    user_data->rollover = false;
    user_data->min_value = INT32_MIN;
    user_data->max_value = INT32_MAX;
    lv_obj_add_event_cb(obj, subject_increment_cb, trigger, user_data);
    lv_obj_add_event_cb(obj, lv_event_free_user_data_cb, LV_EVENT_DELETE, user_data);

    return user_data;
}

void lv_obj_set_subject_increment_event_min_value(lv_obj_t * obj, lv_subject_increment_dsc_t * dsc, int32_t min_value)
{
    LV_UNUSED(obj);
    LV_CHECK_OBJ(obj, &lv_obj_class, return);
    LV_CHECK_ARG(dsc != NULL, return);

    dsc->min_value = min_value;
    if(dsc->subject->type == LV_SUBJECT_TYPE_INT) {
        if(lv_subject_get_int(dsc->subject) < min_value) {
            lv_subject_set_int(dsc->subject, min_value);
        }
    }
#if LV_USE_FLOAT
    else if(dsc->subject->type == LV_SUBJECT_TYPE_FLOAT) {
        if(lv_subject_get_float(dsc->subject) < (float)min_value) {
            lv_subject_set_float(dsc->subject, (float)min_value);
        }
    }
#endif
}

void lv_obj_set_subject_increment_event_max_value(lv_obj_t * obj, lv_subject_increment_dsc_t * dsc, int32_t max_value)
{
    LV_UNUSED(obj);
    LV_CHECK_OBJ(obj, &lv_obj_class, return);
    LV_CHECK_ARG(dsc != NULL, return);

    dsc->max_value = max_value;
    if(dsc->subject->type == LV_SUBJECT_TYPE_INT) {
        if(lv_subject_get_int(dsc->subject) > max_value) {
            lv_subject_set_int(dsc->subject, max_value);
        }
    }
#if LV_USE_FLOAT
    else if(dsc->subject->type == LV_SUBJECT_TYPE_FLOAT) {
        if(lv_subject_get_float(dsc->subject) > (float)max_value) {
            lv_subject_set_float(dsc->subject, (float)max_value);
        }
    }
#endif
}

void lv_obj_set_subject_increment_event_rollover(lv_obj_t * obj, lv_subject_increment_dsc_t * dsc, bool rollover)
{
    LV_UNUSED(obj);
    LV_CHECK_OBJ(obj, &lv_obj_class, return);
    LV_CHECK_ARG(dsc != NULL, return);

    dsc->rollover = rollover;
}

void lv_obj_add_subject_toggle_event(lv_obj_t * obj, lv_subject_t * subject, lv_event_code_t trigger)
{
    LV_CHECK_ARG(obj != NULL, return);
    LV_CHECK_ARG(subject != NULL, return);
    LV_CHECK_ARG(subject->type == LV_SUBJECT_TYPE_INT, return);
    /* It reads the value and writes it back, so both sides have to be an integer. */
    LV_CHECK_ARG(subject->input_type == LV_SUBJECT_TYPE_INT, return);

    lv_obj_add_event_cb(obj, subject_toggle_cb, trigger, subject);
}

void lv_obj_add_subject_set_int_event(lv_obj_t * obj, lv_subject_t * subject, lv_event_code_t trigger, int32_t value)
{
    LV_CHECK_ARG(obj != NULL, return);
    LV_CHECK_ARG(subject != NULL, return);
    LV_CHECK_ARG(subject->input_type == LV_SUBJECT_TYPE_INT, return);

    subject_set_int_user_data_t * user_data = lv_malloc(sizeof(subject_set_int_user_data_t));
    if(user_data == NULL) {
        LV_ASSERT_MALLOC(user_data);
        LV_LOG_WARN("Couldn't allocate user_data");
        return;
    }

    user_data->subject = subject;
    user_data->value = value;

    lv_obj_add_event_cb(obj, subject_set_int_cb, trigger, user_data);
    lv_obj_add_event_cb(obj, lv_event_free_user_data_cb, LV_EVENT_DELETE, user_data);
}

#if LV_USE_FLOAT
void lv_obj_add_subject_set_float_event(lv_obj_t * obj, lv_subject_t * subject, lv_event_code_t trigger, float value)
{
    LV_CHECK_ARG(obj != NULL, return);
    LV_CHECK_ARG(subject != NULL, return);
    LV_CHECK_ARG(subject->input_type == LV_SUBJECT_TYPE_FLOAT, return);

    subject_set_float_user_data_t * user_data = lv_malloc(sizeof(subject_set_float_user_data_t));
    if(user_data == NULL) {
        LV_ASSERT_MALLOC(user_data);
        LV_LOG_WARN("Couldn't allocate user_data");
        return;
    }

    user_data->subject = subject;
    user_data->value = value;

    lv_obj_add_event_cb(obj, subject_set_float_cb, trigger, user_data);
    lv_obj_add_event_cb(obj, lv_event_free_user_data_cb, LV_EVENT_DELETE, user_data);
}
#endif /*LV_USE_FLOAT*/

void lv_obj_add_subject_set_string_event(lv_obj_t * obj, lv_subject_t * subject, lv_event_code_t trigger,
                                         const char * value)
{
    LV_CHECK_ARG(obj != NULL, return);
    LV_CHECK_ARG(subject != NULL, return);
    LV_CHECK_ARG(subject->input_type == LV_SUBJECT_TYPE_STRING, return);
    LV_CHECK_ARG(value != NULL, return);

    subject_set_string_user_data_t * user_data = lv_malloc(sizeof(subject_set_string_user_data_t));
    if(user_data == NULL) {
        LV_ASSERT_MALLOC(user_data);
        LV_LOG_WARN("Couldn't allocate user_data");
        return;
    }

    user_data->subject = subject;
    user_data->value = lv_strdup(value);
    if(user_data->value == NULL) {
        LV_ASSERT_MALLOC(user_data->value);
        LV_LOG_WARN("Couldn't allocate string value");
        lv_free(user_data);
        return;
    }

    lv_obj_add_event_cb(obj, subject_set_string_cb, trigger, user_data);
    lv_obj_add_event_cb(obj, subject_set_string_free_user_data_event_cb, LV_EVENT_DELETE, user_data);
}

lv_observer_t * lv_obj_bind_bool(lv_obj_t * obj, lv_subject_t * subject, lv_obj_set_bool_t set_bool_cb)
{
    LV_CHECK_ARG(obj != NULL, return NULL);
    LV_CHECK_ARG(subject != NULL, return NULL);
    LV_CHECK_ARG(set_bool_cb != NULL, return NULL);
    LV_CHECK_ARG(subject->type == LV_SUBJECT_TYPE_INT, return NULL);

    lv_observer_t * observable = lv_subject_add_observer_obj(subject, set_bool_observer, obj, NULL);
    if(observable == NULL) {
        LV_LOG_WARN("Couldn't add observer to subject");
        return NULL;
    }

    /* Passing a function pointer as void * user_data generates warning so set it here, and call the callback manually */
    observable->user_cb = (void (*)(void))set_bool_cb;
    set_bool_observer(observable, subject);
    return observable;
}

lv_observer_t * lv_obj_bind_int(lv_obj_t * obj, lv_subject_t * subject, lv_obj_set_int_t set_int_cb)
{
    LV_CHECK_ARG(obj != NULL, return NULL);
    LV_CHECK_ARG(subject != NULL, return NULL);
    LV_CHECK_ARG(set_int_cb != NULL, return NULL);
    LV_CHECK_ARG(subject->type == LV_SUBJECT_TYPE_INT, return NULL);
    lv_observer_t * observable = lv_subject_add_observer_obj(subject, set_int_observer, obj, NULL);
    if(observable == NULL) {
        LV_LOG_WARN("Couldn't add observer to subject");
        return NULL;
    }

    /* Passing a function pointer as void * user_data generates warning so set it here, and call the callback manually */
    observable->user_cb = (void (*)(void))set_int_cb;
    set_int_observer(observable, subject);
    return observable;
}

#if LV_USE_FLOAT
lv_observer_t * lv_obj_bind_float(lv_obj_t * obj, lv_subject_t * subject, lv_obj_set_float_t set_float_cb)
{
    LV_CHECK_ARG(obj != NULL, return NULL);
    LV_CHECK_ARG(subject != NULL, return NULL);
    LV_CHECK_ARG(set_float_cb != NULL, return NULL);
    LV_CHECK_ARG(subject->type == LV_SUBJECT_TYPE_FLOAT, return NULL);

    lv_observer_t * observable = lv_subject_add_observer_obj(subject, set_float_observer, obj, NULL);
    if(observable == NULL) {
        LV_LOG_WARN("Couldn't add observer to subject");
        return NULL;
    }

    /* Passing a function pointer as void * user_data generates warning so set it here, and call the callback manually */
    observable->user_cb = (void (*)(void))set_float_cb;
    set_float_observer(observable, subject);
    return observable;
}
#endif

lv_observer_t * lv_obj_bind_string(lv_obj_t * obj, lv_subject_t * subject, lv_obj_set_string_t set_string_cb)
{
    LV_CHECK_ARG(obj != NULL, return NULL);
    LV_CHECK_ARG(subject != NULL, return NULL);
    LV_CHECK_ARG(set_string_cb != NULL, return NULL);
    LV_CHECK_ARG(subject->type == LV_SUBJECT_TYPE_STRING, return NULL);

    lv_observer_t * observable = lv_subject_add_observer_obj(subject, set_string_observer, obj, NULL);
    if(observable == NULL) {
        LV_LOG_WARN("Couldn't add observer to subject");
        return NULL;
    }

    /* Passing a function pointer as void * user_data generates warning so set it here, and call the callback manually */
    observable->user_cb = (void (*)(void))set_string_cb;
    set_string_observer(observable, subject);
    return observable;
}

lv_observer_t * lv_obj_bind_color(lv_obj_t * obj, lv_subject_t * subject, lv_obj_set_color_t set_color_cb)
{
    LV_CHECK_ARG(obj != NULL, return NULL);
    LV_CHECK_ARG(subject != NULL, return NULL);
    LV_CHECK_ARG(set_color_cb != NULL, return NULL);
    LV_CHECK_ARG(subject->type == LV_SUBJECT_TYPE_COLOR, return NULL);

    lv_observer_t * observable = lv_subject_add_observer_obj(subject, set_color_observer, obj, NULL);
    if(observable == NULL) {
        LV_LOG_WARN("Couldn't add observer to subject");
        return NULL;
    }

    /* Passing a function pointer as void * user_data generates warning so set it here, and call the callback manually */
    observable->user_cb = (void (*)(void))set_color_cb;
    set_color_observer(observable, subject);
    return observable;
}

lv_observer_t * lv_obj_bind_pointer(lv_obj_t * obj, lv_subject_t * subject, lv_obj_set_pointer_t set_pointer_cb)
{
    LV_CHECK_ARG(obj != NULL, return NULL);
    LV_CHECK_ARG(subject != NULL, return NULL);
    LV_CHECK_ARG(set_pointer_cb != NULL, return NULL);
    LV_CHECK_ARG(subject->type == LV_SUBJECT_TYPE_POINTER, return NULL);

    lv_observer_t * observable = lv_subject_add_observer_obj(subject, set_pointer_observer, obj, NULL);
    if(observable == NULL) {
        LV_LOG_WARN("Couldn't add observer to subject");
        return NULL;
    }

    /* Passing a function pointer as void * user_data generates warning so set it here, and call the callback manually */
    observable->user_cb = (void (*)(void))set_pointer_cb;
    set_pointer_observer(observable, subject);
    return observable;
}

lv_observer_t * lv_obj_bind_bool_mapped(lv_obj_t * obj, lv_subject_t * subject, lv_obj_set_bool_t set_bool_cb,
                                        lv_observer_bool_mapper_t mapper, void * user_data)
{
    LV_CHECK_ARG(obj != NULL, return NULL);
    LV_CHECK_ARG(subject != NULL, return NULL);
    LV_CHECK_ARG(set_bool_cb != NULL, return NULL);
    LV_CHECK_ARG(mapper != NULL, return NULL);
    /* No type check on `subject`: mapping the value is the whole point. */

    lv_observer_mapper_t m = { 0 };
    m.bool_cb = mapper;
    return bind_mapped(obj, subject, set_bool_observer, (void (*)(void))set_bool_cb, m, user_data, 0);
}

lv_observer_t * lv_obj_bind_int_mapped(lv_obj_t * obj, lv_subject_t * subject, lv_obj_set_int_t set_int_cb,
                                       lv_observer_int_mapper_t mapper, void * user_data)
{
    LV_CHECK_ARG(obj != NULL, return NULL);
    LV_CHECK_ARG(subject != NULL, return NULL);
    LV_CHECK_ARG(set_int_cb != NULL, return NULL);
    LV_CHECK_ARG(mapper != NULL, return NULL);

    lv_observer_mapper_t m = { 0 };
    m.int_cb = mapper;
    return bind_mapped(obj, subject, set_int_observer, (void (*)(void))set_int_cb, m, user_data, 0);
}

#if LV_USE_FLOAT
lv_observer_t * lv_obj_bind_float_mapped(lv_obj_t * obj, lv_subject_t * subject, lv_obj_set_float_t set_float_cb,
                                         lv_observer_float_mapper_t mapper, void * user_data)
{
    LV_CHECK_ARG(obj != NULL, return NULL);
    LV_CHECK_ARG(subject != NULL, return NULL);
    LV_CHECK_ARG(set_float_cb != NULL, return NULL);
    LV_CHECK_ARG(mapper != NULL, return NULL);

    lv_observer_mapper_t m = { 0 };
    m.float_cb = mapper;
    return bind_mapped(obj, subject, set_float_observer, (void (*)(void))set_float_cb, m, user_data, 0);
}
#endif

lv_observer_t * lv_obj_bind_string_mapped(lv_obj_t * obj, lv_subject_t * subject, lv_obj_set_string_t set_string_cb,
                                          lv_observer_string_mapper_t mapper, void * user_data)
{
    LV_CHECK_ARG(obj != NULL, return NULL);
    LV_CHECK_ARG(subject != NULL, return NULL);
    LV_CHECK_ARG(set_string_cb != NULL, return NULL);
    LV_CHECK_ARG(mapper != NULL, return NULL);

    lv_observer_mapper_t m = { 0 };
    m.string_cb = mapper;
    return bind_mapped(obj, subject, set_string_observer, (void (*)(void))set_string_cb, m, user_data, 0);
}

lv_observer_t * lv_obj_bind_color_mapped(lv_obj_t * obj, lv_subject_t * subject, lv_obj_set_color_t set_color_cb,
                                         lv_observer_color_mapper_t mapper, void * user_data)
{
    LV_CHECK_ARG(obj != NULL, return NULL);
    LV_CHECK_ARG(subject != NULL, return NULL);
    LV_CHECK_ARG(set_color_cb != NULL, return NULL);
    LV_CHECK_ARG(mapper != NULL, return NULL);

    lv_observer_mapper_t m = { 0 };
    m.color_cb = mapper;
    return bind_mapped(obj, subject, set_color_observer, (void (*)(void))set_color_cb, m, user_data, 0);
}

lv_observer_t * lv_obj_bind_pointer_mapped(lv_obj_t * obj, lv_subject_t * subject, lv_obj_set_pointer_t set_pointer_cb,
                                           lv_observer_pointer_mapper_t mapper, void * user_data)
{
    LV_CHECK_ARG(obj != NULL, return NULL);
    LV_CHECK_ARG(subject != NULL, return NULL);
    LV_CHECK_ARG(set_pointer_cb != NULL, return NULL);
    LV_CHECK_ARG(mapper != NULL, return NULL);

    lv_observer_mapper_t m = { 0 };
    m.pointer_cb = mapper;
    return bind_mapped(obj, subject, set_pointer_observer, (void (*)(void))set_pointer_cb, m, user_data, 0);
}

lv_observer_t * lv_obj_bind_flag_if_eq(lv_obj_t * obj, lv_subject_t * subject, lv_obj_flag_t flag, int32_t ref_value)
{
    LV_CHECK_ARG(obj != NULL, return NULL);
    LV_CHECK_ARG(subject != NULL, return NULL);
    LV_CHECK_ARG_FORMAT_MSG(subject->type == LV_SUBJECT_TYPE_INT, return NULL, "Incompatible subject type: %d",
                            subject->type);

    lv_observer_t * observable = bind_to_bitfield(subject, obj, obj_flag_observer_cb, flag, ref_value, false, FLAG_COND_EQ);
    return observable;
}

lv_observer_t * lv_obj_bind_flag_if_not_eq(lv_obj_t * obj, lv_subject_t * subject, lv_obj_flag_t flag,
                                           int32_t ref_value)
{
    LV_CHECK_ARG(obj != NULL, return NULL);
    LV_CHECK_ARG(subject != NULL, return NULL);
    LV_CHECK_ARG_FORMAT_MSG(subject->type == LV_SUBJECT_TYPE_INT, return NULL, "Incompatible subject type: %d",
                            subject->type);

    lv_observer_t * observable = bind_to_bitfield(subject, obj, obj_flag_observer_cb, flag, ref_value, true, FLAG_COND_EQ);
    return observable;
}
lv_observer_t * lv_obj_bind_flag_if_gt(lv_obj_t * obj, lv_subject_t * subject, lv_obj_flag_t flag, int32_t ref_value)
{
    LV_CHECK_ARG(obj != NULL, return NULL);
    LV_CHECK_ARG(subject != NULL, return NULL);
    LV_CHECK_ARG_FORMAT_MSG(subject->type == LV_SUBJECT_TYPE_INT, return NULL, "Incompatible subject type: %d",
                            subject->type);

    lv_observer_t * observable = bind_to_bitfield(subject, obj, obj_flag_observer_cb, flag, ref_value, false, FLAG_COND_GT);
    return observable;
}

lv_observer_t * lv_obj_bind_flag_if_ge(lv_obj_t * obj, lv_subject_t * subject, lv_obj_flag_t flag, int32_t ref_value)
{
    LV_CHECK_ARG(obj != NULL, return NULL);
    LV_CHECK_ARG(subject != NULL, return NULL);
    LV_CHECK_ARG_FORMAT_MSG(subject->type == LV_SUBJECT_TYPE_INT, return NULL, "Incompatible subject type: %d",
                            subject->type);

    lv_observer_t * observable = bind_to_bitfield(subject, obj, obj_flag_observer_cb, flag, ref_value, false, FLAG_COND_GE);
    return observable;
}

lv_observer_t * lv_obj_bind_flag_if_lt(lv_obj_t * obj, lv_subject_t * subject, lv_obj_flag_t flag, int32_t ref_value)
{
    LV_CHECK_ARG(obj != NULL, return NULL);
    LV_CHECK_ARG(subject != NULL, return NULL);
    LV_CHECK_ARG_FORMAT_MSG(subject->type == LV_SUBJECT_TYPE_INT, return NULL, "Incompatible subject type: %d",
                            subject->type);

    /* a < b == !(a >= b) */
    lv_observer_t * observable = bind_to_bitfield(subject, obj, obj_flag_observer_cb, flag, ref_value, true, FLAG_COND_GE);
    return observable;
}

lv_observer_t * lv_obj_bind_flag_if_le(lv_obj_t * obj, lv_subject_t * subject, lv_obj_flag_t flag, int32_t ref_value)
{
    LV_CHECK_ARG(obj != NULL, return NULL);
    LV_CHECK_ARG(subject != NULL, return NULL);
    LV_CHECK_ARG_FORMAT_MSG(subject->type == LV_SUBJECT_TYPE_INT, return NULL, "Incompatible subject type: %d",
                            subject->type);

    /* a <= b == !(a > b) */
    lv_observer_t * observable = bind_to_bitfield(subject, obj, obj_flag_observer_cb, flag, ref_value, true, FLAG_COND_GT);
    return observable;

}

lv_observer_t * lv_obj_bind_state_if_eq(lv_obj_t * obj, lv_subject_t * subject, lv_state_t state, int32_t ref_value)
{
    LV_CHECK_ARG(obj != NULL, return NULL);
    LV_CHECK_ARG(subject != NULL, return NULL);
    LV_CHECK_ARG_FORMAT_MSG(subject->type == LV_SUBJECT_TYPE_INT, return NULL, "Incompatible subject type: %d",
                            subject->type);

    lv_observer_t * observable = bind_to_bitfield(subject, obj, obj_state_observer_cb, state, ref_value, false,
                                                  FLAG_COND_EQ);
    return observable;
}

lv_observer_t * lv_obj_bind_state_if_not_eq(lv_obj_t * obj, lv_subject_t * subject, lv_state_t state,
                                            int32_t ref_value)
{
    LV_CHECK_ARG(obj != NULL, return NULL);
    LV_CHECK_ARG(subject != NULL, return NULL);
    LV_CHECK_ARG_FORMAT_MSG(subject->type == LV_SUBJECT_TYPE_INT, return NULL, "Incompatible subject type: %d",
                            subject->type);

    lv_observer_t * observable = bind_to_bitfield(subject, obj, obj_state_observer_cb, state, ref_value, true,
                                                  FLAG_COND_EQ);
    return observable;
}

lv_observer_t * lv_obj_bind_state_if_gt(lv_obj_t * obj, lv_subject_t * subject, lv_state_t state, int32_t ref_value)
{
    LV_CHECK_ARG(obj != NULL, return NULL);
    LV_CHECK_ARG(subject != NULL, return NULL);
    LV_CHECK_ARG_FORMAT_MSG(subject->type == LV_SUBJECT_TYPE_INT, return NULL, "Incompatible subject type: %d",
                            subject->type);

    lv_observer_t * observable = bind_to_bitfield(subject, obj, obj_state_observer_cb, state, ref_value, false,
                                                  FLAG_COND_GT);
    return observable;
}

lv_observer_t * lv_obj_bind_state_if_ge(lv_obj_t * obj, lv_subject_t * subject, lv_state_t state, int32_t ref_value)
{
    LV_CHECK_ARG(obj != NULL, return NULL);
    LV_CHECK_ARG(subject != NULL, return NULL);
    LV_CHECK_ARG_FORMAT_MSG(subject->type == LV_SUBJECT_TYPE_INT, return NULL, "Incompatible subject type: %d",
                            subject->type);

    lv_observer_t * observable = bind_to_bitfield(subject, obj, obj_state_observer_cb, state, ref_value, false,
                                                  FLAG_COND_GE);
    return observable;
}

lv_observer_t * lv_obj_bind_state_if_lt(lv_obj_t * obj, lv_subject_t * subject, lv_state_t state, int32_t ref_value)
{
    LV_CHECK_ARG(obj != NULL, return NULL);
    LV_CHECK_ARG(subject != NULL, return NULL);
    LV_CHECK_ARG_FORMAT_MSG(subject->type == LV_SUBJECT_TYPE_INT, return NULL, "Incompatible subject type: %d",
                            subject->type);

    /* a < b == !(a >= b) */
    lv_observer_t * observable = bind_to_bitfield(subject, obj, obj_state_observer_cb, state, ref_value, true,
                                                  FLAG_COND_GE);
    return observable;

}

lv_observer_t * lv_obj_bind_state_if_le(lv_obj_t * obj, lv_subject_t * subject, lv_state_t state, int32_t ref_value)
{
    LV_CHECK_ARG(obj != NULL, return NULL);
    LV_CHECK_ARG(subject != NULL, return NULL);
    LV_CHECK_ARG_FORMAT_MSG(subject->type == LV_SUBJECT_TYPE_INT, return NULL, "Incompatible subject type: %d",
                            subject->type);

    /* a <= b == !(a > b) */
    lv_observer_t * observable = bind_to_bitfield(subject, obj, obj_state_observer_cb, state, ref_value, true,
                                                  FLAG_COND_GT);
    return observable;
}


lv_observer_t * lv_obj_bind_checked(lv_obj_t * obj, lv_subject_t * subject)
{
    LV_CHECK_ARG(obj != NULL, return NULL);
    LV_CHECK_ARG(subject != NULL, return NULL);
    LV_CHECK_ARG_FORMAT_MSG(subject->type == LV_SUBJECT_TYPE_INT, return NULL, "Incompatible subject type: %d",
                            subject->type);

    lv_observer_t * observable = bind_to_bitfield(subject, obj, obj_state_observer_cb, LV_STATE_CHECKED, 0, true,
                                                  FLAG_COND_EQ);

    lv_obj_add_event_cb(obj, obj_value_changed_event_cb, LV_EVENT_VALUE_CHANGED, subject);

    return observable;
}


lv_obj_t * lv_observer_get_target_obj(lv_observer_t * observer)
{
    LV_CHECK_ARG(observer != NULL, return NULL);

    return (lv_obj_t *)lv_observer_get_target(observer);
}

void * lv_observer_get_user_data(const lv_observer_t * observer)
{
    LV_CHECK_ARG(observer != NULL, return NULL);

    return observer->user_data;
}

lv_subject_t * lv_observer_get_subject(const lv_observer_t * observer)
{
    LV_CHECK_ARG(observer != NULL, return NULL);

    return observer->subject;
}

void lv_observer_set_mode(lv_observer_t * observer, lv_observer_mode_t mode)
{
    LV_CHECK_ARG(observer != NULL, return);

    uint32_t new_mode = (mode == LV_OBSERVER_MODE_BATCHED) ? 1U : 0U;
    if(observer->mode == new_mode) return;
    observer->mode = new_mode;

    lv_subject_t * subject = observer->subject;
    if(new_mode == 1U) {
        if(subject->immediate_observer_cnt > 0) subject->immediate_observer_cnt--;
    }
    else {
        subject->immediate_observer_cnt++;
        /* The Subject is due now, so do not make this Observer wait for a flush. */
        list_reposition(subject);
        if(subject->dirty || subject->pending_notify) process_dirty(false);
    }
}

lv_observer_mode_t lv_observer_get_mode(const lv_observer_t * observer)
{
    LV_CHECK_ARG(observer != NULL, return LV_OBSERVER_MODE_IMMEDIATE);

    return observer->mode ? LV_OBSERVER_MODE_BATCHED : LV_OBSERVER_MODE_IMMEDIATE;
}

void lv_observer_set_user_data(lv_observer_t * observer, void * user_data)
{
    LV_CHECK_ARG(observer != NULL, return);

    /* Release internally-owned data only when replacing it. */
    if(observer->auto_free_user_data && observer->user_data != user_data) {
        lv_free(observer->user_data);
    }

    /* Data assigned through the public setter is always caller-owned. */
    observer->user_data = user_data;
    observer->auto_free_user_data = 0;
}

/**********************
 *   STATIC FUNCTIONS
 **********************/

/*---------------------------------------------------------------
 * Dependency graph helpers
 *
 * `deps` holds the Subjects a Subject read during its last evaluation, `dependents`
 * the reverse edges. Both are `lv_ll_t` lists of `subject_ref_t`.
 *--------------------------------------------------------------*/

static bool ref_list_contains(lv_ll_t * list, const lv_subject_t * subject)
{
    subject_ref_t * ref;
    LV_LL_READ(list, ref) {
        if(ref->subject == subject) return true;
    }
    return false;
}

static bool ref_list_add(lv_ll_t * list, lv_subject_t * subject)
{
    subject_ref_t * ref = lv_ll_ins_tail(list);
    LV_ASSERT_MALLOC(ref);
    if(ref == NULL) return false;
    ref->subject = subject;
    return true;
}

static void ref_list_remove(lv_ll_t * list, const lv_subject_t * subject)
{
    subject_ref_t * ref;
    LV_LL_READ(list, ref) {
        if(ref->subject != subject) continue;
        lv_ll_remove(list, ref);
        lv_free(ref);
        return;
    }
}

static uint32_t ref_list_count(const lv_ll_t * list)
{
    return lv_ll_get_len(list);
}

/* Drop `subject`'s outgoing edges. Called before every evaluation, because a mapper
 * may read a different set of Subjects each time it runs. */
static void deps_clear(lv_subject_t * subject)
{
    subject_ref_t * ref = lv_ll_get_head(&subject->deps);
    while(ref) {
        subject_ref_t * next = lv_ll_get_next(&subject->deps, ref);
        ref_list_remove(&ref->subject->dependents, subject);
        lv_ll_remove(&subject->deps, ref);
        lv_free(ref);
        ref = next;
    }
}

/* Remove `subject` from the graph in both directions, so no dangling edge is left
 * when a Subject in the middle of a graph is deleted. */
static void edges_teardown(lv_subject_t * subject)
{
    deps_clear(subject);

    subject_ref_t * ref = lv_ll_get_head(&subject->dependents);
    while(ref) {
        subject_ref_t * next = lv_ll_get_next(&subject->dependents, ref);
        ref_list_remove(&ref->subject->deps, subject);
        lv_ll_remove(&subject->dependents, ref);
        lv_free(ref);
        ref = next;
    }
}

/*---------------------------------------------------------------
 * Dirty bookkeeping
 *
 * Invariant: every dirty Subject sits in a prefix of the global Subject list, so a
 * walk can start at the head and stop at the first clean Subject. `lv_ll_move_before`
 * only relinks, so a Subject's address never changes and user-held pointers stay valid.
 *--------------------------------------------------------------*/

/* Keep the "dirty prefix" invariant after `dirty` or `pending_notify` changed.
 * An application-owned Subject is not in the list, so it cannot be relinked. It keeps
 * its flags and is brought up to date when something reads it. */
/* Will a drain or a flush ever have to visit this Subject?
 *
 * A Subject that is dirty but has no Observers and is not eager is due for nothing: no
 * drain evaluates it and no flush evaluates it. It is computed when something reads it,
 * through `subject_pull()`, which needs no scan. Such Subjects otherwise accumulate at
 * the front of the list and every drain walks past all of them, so the cost of a write
 * grows with the number of derived Subjects nobody reads. Keeping them out of the
 * scanned region is what stops that. */
static bool needs_scanning(lv_subject_t * subject)
{
    if(subject->pending_notify) return true;         /* a flush owes a notification */
    if(!subject->dirty) return false;
    if(subject_is_eager_now(subject)) return true;   /* a drain must evaluate it */
    return !lv_ll_is_empty(&subject->subs_ll);       /* a flush must evaluate it */
}

static void list_reposition(lv_subject_t * subject)
{
    if(!subject->in_list) return;

    if(needs_scanning(subject)) {
        lv_ll_move_before(&subject_list, subject, lv_ll_get_head(&subject_list));
    }
    else {
        lv_ll_move_before(&subject_list, subject, NULL);
    }
}

static void mark_dirty(lv_subject_t * subject)
{
    subject->dirty = 1;
    list_reposition(subject);
    flush_timer_update();
}

static void mark_pending_notify(lv_subject_t * subject)
{
    subject->pending_notify = 1;
    list_reposition(subject);
    flush_timer_update();
}

static void clear_pending(lv_subject_t * subject)
{
    subject->dirty = 0;
    subject->pending_notify = 0;
    list_reposition(subject);
}

static void mark_dependents_dirty(lv_subject_t * subject)
{
    subject_ref_t * ref;
    LV_LL_READ(&subject->dependents, ref) {
        lv_subject_t * dependent = ref->subject;
        /* Already dirty means its own subtree was walked too. This also stops a cycle. */
        if(dependent->dirty) continue;
        mark_dirty(dependent);
        mark_dependents_dirty(dependent);
    }
}

/* Evaluate the dirty Subjects that are due now.
 *
 * `include_observed` false is the drain that runs inside `lv_subject_set_...()`: only
 * effectively eager Subjects are due. True is `lv_subject_flush()`, which also picks
 * up lazy Subjects that have Observers waiting.
 *
 * A Subject is skipped rather than ending the walk, because the dirty prefix mixes
 * due and not-yet-due Subjects. The walk restarts after each evaluation, since an
 * eager mapper may have written another Subject and re-ordered the list. */
static void process_dirty(bool include_observed)
{
    lv_global_t * global = LV_GLOBAL_DEFAULT();
    if(global->subject_flushing) return;  /* the outermost call owns the loop */
    global->subject_flushing = 1;

    /* No round cap is needed: a mapper may not write a Subject, so an evaluation cannot
     * re-dirty anything, and the dirty set only ever shrinks here. */
    bool progress = true;
    while(progress) {
        progress = false;
        lv_subject_t * subject = lv_ll_get_head(&subject_list);
        while(subject && needs_scanning(subject)) {
            lv_subject_t * next = lv_ll_get_next(&subject_list, subject);
            bool due = subject_is_eager_now(subject) ||
                       (include_observed && !lv_ll_is_empty(&subject->subs_ll));
            if(due) {
                /* Re-run a stale mapper. This may notify by itself if the Subject is
                 * eager, or leave a pending notification if it is not. */
                if(subject->dirty) subject_recompute(subject);
                /* Deliver whatever notification is still owed. */
                if(subject->pending_notify) {
                    clear_pending(subject);
                    notify(subject);
                }
                progress = true;
                break;
            }
            subject = next;
        }
    }

    global->subject_flushing = 0;
    flush_timer_update();
}

static void flush_timer_cb(lv_timer_t * timer)
{
    LV_UNUSED(timer);
    lv_subject_flush();
}

/* Keep a once-per-`lv_timer_handler()`-pass timer alive only while there is pending
 * work. The timer is created on first need, because `lv_subject_global_init()` runs
 * before `lv_timer_core_init()`. */
static void flush_timer_update(void)
{
    lv_global_t * global = LV_GLOBAL_DEFAULT();
    if(global->subject_flushing) return;

    bool pending = false;
    lv_subject_t * head = lv_ll_get_head(&subject_list);
    if(head && needs_scanning(head)) pending = true;

    if(!pending) {
        if(global->subject_flush_timer) lv_timer_pause(global->subject_flush_timer);
        return;
    }

    if(global->subject_flush_timer == NULL) {
        global->subject_flush_timer = lv_timer_create(flush_timer_cb, 0, NULL);
        if(global->subject_flush_timer == NULL) return;
    }
    lv_timer_resume(global->subject_flush_timer);
}

/*---------------------------------------------------------------
 * Evaluation
 *--------------------------------------------------------------*/

static bool subject_is_eager_now(const lv_subject_t * subject)
{
    return subject->mode == 1U || subject->immediate_observer_cnt > 0;
}

/* A mapper must be pure with respect to the Subject graph: it may read Subjects, never
 * write them. Allowing a write would mean an evaluation could re-enter the update path
 * and re-dirty what is being computed, so ordering and termination stop being
 * predictable. Observer callbacks *may* write, which is why this only guards the window
 * while a mapper is running. */
/* A Subject whose input type differs from its value type has nothing to convert with
 * unless it has a mapper. */
static size_t buf_capacity(const lv_subject_t * subject)
{
    return subject->buf ? subject->buf->capacity : 0;
}

/* Release the Subject's buffer with its own free_cb. A static buffer has no free_cb, so
 * nothing happens, which is what a static buffer wants. */
static void buf_release(lv_subject_t * subject)
{
    lv_subject_buf_t * b = subject->buf;
    if(b == NULL) return;

    if(b->buf != NULL && b->free_cb != NULL) b->free_cb(b->buf);

    lv_free(b);
    subject->buf = NULL;
    subject->value.pointer = NULL;
    subject->last_input.pointer = NULL;
}

/* Make sure the buffer can hold `needed` bytes, growing it if it can and must.
 * Returns false without touching anything if it cannot, so the caller can refuse the
 * write and leave the Subject's previous value intact. */
static bool buf_reserve(lv_subject_t * subject, size_t needed, bool warn)
{
    lv_subject_buf_t * b = subject->buf;
    if(b == NULL) {
        if(warn) {
            LV_LOG_WARN("This Subject has no buffer. Set one with lv_subject_set_buffer() before "
                        "copying into it.");
        }
        return false;
    }
    if(b->capacity >= needed) return true;

    if(b->realloc_cb == NULL) {
        if(warn) {
            LV_LOG_WARN("This Subject's buffer is fixed at %" LV_PRIu32 " bytes and cannot hold "
                        "%" LV_PRIu32 ". The write is ignored. Use lv_subject_copy_string_trimmed() "
                        "to store as much as fits instead.", (uint32_t)b->capacity, (uint32_t)needed);
        }
        return false;
    }

    void * grown = b->realloc_cb(b->buf, needed);
    if(grown == NULL) {
        if(warn) {
            LV_LOG_WARN("Growing this Subject's buffer to %" LV_PRIu32 " bytes failed. The write is "
                        "ignored and the previous value is kept.", (uint32_t)needed);
        }
        return false;
    }

    b->buf = grown;
    b->capacity = needed;
    /* The value is the buffer, so it moves with it. */
    subject->value.pointer = grown;
    subject->last_input.pointer = grown;
    return true;
}

static bool input_convertible(const lv_subject_t * subject)
{
    if(subject->has_mapper) return true;
    if(subject->input_type == subject->type) return true;

    LV_LOG_WARN("This Subject is written as one type and observed as another, but has no "
                "mapper to convert with. The write is IGNORED. Set a mapper for the value type.");
    return false;
}

static bool write_allowed(void)
{
    lv_subject_t * evaluating = LV_GLOBAL_DEFAULT()->subject_evaluating;
    if(evaluating == NULL) return true;

    LV_LOG_WARN("A Subject must not be written from inside a mapper. The write is IGNORED. "
                "A mapper may only read Subjects; put side effects in an Observer of an "
                "LV_SUBJECT_MODE_EAGER Subject instead.");
    return false;
}

/* Change detection for a Subject with no mapper: compare the value just written against
 * the stored one, and store it. No history is kept, so there is nothing else to compare
 * against. `borrowed_input` is the new string for LV_SUBJECT_TYPE_STRING and unused otherwise. */
static bool apply_input(lv_subject_t * subject, const void * borrowed_input)
{
    switch(subject->type) {
        case LV_SUBJECT_TYPE_INT:
            if(subject->value.num == subject->last_input.num) return false;
            subject->value.num = subject->last_input.num;
            return true;
#if LV_USE_FLOAT
        case LV_SUBJECT_TYPE_FLOAT:
            if(subject->value.float_v == subject->last_input.float_v) return false;
            subject->value.float_v = subject->last_input.float_v;
            return true;
#endif
        case LV_SUBJECT_TYPE_COLOR:
            if(lv_color_to_u32(subject->value.color) == lv_color_to_u32(subject->last_input.color)) return false;
            subject->value.color = subject->last_input.color;
            return true;
        case LV_SUBJECT_TYPE_POINTER:
            if(subject->buf != NULL) {
                /* Copying: the bytes are already in the buffer, and the comparison
                 * against the previous ones was done while they were still there. */
                return subject->copy_changed;
            }
            /* Referring: documented to notify whether or not the pointer itself changed,
             * because the data behind an unchanged pointer may have changed. */
            subject->value.pointer = subject->last_input.pointer;
            return true;
        case LV_SUBJECT_TYPE_STRING:
            if(subject->buf == NULL) {
                /* No buffer, so this Subject stores the pointer rather than copying. */
                subject->value.pointer = subject->last_input.pointer;
                return true;
            }
            if(borrowed_input == NULL) return false;
            if(!subject->copy_changed) return false;
            lv_strlcpy((char *)subject->buf->buf, borrowed_input, buf_capacity(subject));
            subject->buf->length = lv_strlen((const char *)subject->buf->buf);
            return true;
        default:
            /* LV_SUBJECT_TYPE_NONE has no value; it only ever aggregates. */
            return true;
    }
}

/* Run the mapper with dependency tracking on. Returns whether it reported a change. */
static bool run_mapper(lv_subject_t * subject, const void * borrowed_input)
{
    if(subject->evaluating) {
        LV_LOG_WARN("Dependency cycle detected, a subject's mapper reads the subject itself");
        return false;
    }

    lv_global_t * global = LV_GLOBAL_DEFAULT();
    lv_subject_t * outer = global->subject_evaluating;
    void * ud = subject->mapper_user_data;

    subject->evaluating = 1;
    global->subject_evaluating = subject;
    deps_clear(subject);

    /* The input arrives as the union, because a Subject's input type need not be the
     * type of its value. The mapper reads the member matching the input type.
     *
     * A pointer input is retained, so a re-evaluation caused by a dependency change gets
     * the same pointer the last write carried. The caller answers for it: it has to stay
     * valid until a new input is written, or its ownership has to be transferred with
     * `lv_subject_set_pointer_owned()`.
     *
     * A string input is the one exception: NULL on a re-evaluation. `lv_subject_snprintf()`
     * formats into a temporary it frees before returning, so retaining that pointer would
     * leave a dangling one; and it is not needed, because a string mapper re-derives from
     * `buf`, which still holds the last value. The *value* is retained either way. */
    lv_subject_value_t input = subject->last_input;
    if(subject->input_type == LV_SUBJECT_TYPE_STRING) input.pointer = borrowed_input;

    bool changed = false;
    switch(subject->type) {
        case LV_SUBJECT_TYPE_INT: {
                int32_t value = subject->value.num;
                changed = subject->mapper.int_cb(subject, ud, input, &value);
                subject->value.num = value;
                break;
            }
#if LV_USE_FLOAT
        case LV_SUBJECT_TYPE_FLOAT: {
                float value = subject->value.float_v;
                changed = subject->mapper.float_cb(subject, ud, input, &value);
                subject->value.float_v = value;
                break;
            }
#endif
        case LV_SUBJECT_TYPE_POINTER: {
                const void * value = subject->value.pointer;
                changed = subject->mapper.pointer_cb(subject, ud, input, &value);
                subject->value.pointer = value;
                break;
            }
        case LV_SUBJECT_TYPE_COLOR: {
                lv_color_t value = subject->value.color;
                changed = subject->mapper.color_cb(subject, ud, input, &value);
                subject->value.color = value;
                break;
            }
        case LV_SUBJECT_TYPE_STRING:
            changed = subject->mapper.string_cb(subject, ud, input, (char *)subject->value.pointer,
                                                buf_capacity(subject));
            break;
        case LV_SUBJECT_TYPE_NONE:
            changed = subject->mapper.none_cb(subject, ud);
            break;
        default:
            break;
    }

    /* Restoring from a local keeps this correct when an evaluation pulls a dependency,
     * which evaluates that dependency inside this one. */
    global->subject_evaluating = outer;
    subject->evaluating = 0;
    return changed;
}

/* Update the stored value from the mapper (or the plain comparison) and propagate if it
 * changed. */
/* Can a write just record the input and mark the Subject stale, leaving the mapper for
 * whoever reads it next?
 *
 * Only for a Subject that is genuinely lazy, and only when nothing about it needs the
 * mapper to have run already. Three cases have to evaluate on the spot:
 *
 * - it is evaluating eagerly, which is the whole meaning of eager;
 * - it owns its value, because ownership of the incoming pointer is only transferred
 *   once the mapper stores it. Deferring would let a second write supersede an input
 *   that was never stored, and nothing would ever release it;
 * - its input is pointer-shaped, i.e. `LV_SUBJECT_TYPE_POINTER` or
 *   `LV_SUBJECT_TYPE_STRING`. Such an input is borrowed: it is only guaranteed valid for
 *   the duration of the write, and the mapper may well read *through* it rather than
 *   just storing it. Running the mapper later could dereference memory that is already
 *   gone, and a string input is not retained at all so deferring would lose it.
 *
 * So deferral applies to the scalar inputs, which is where coalescing a run of writes
 * into one evaluation is worth anything anyway.
 */
static bool can_defer_mapper(const lv_subject_t * subject)
{
    if(!subject->has_mapper) return false;   /* nothing to defer */
    if(subject_is_eager_now(subject)) return false;
    /* Subsumed by the two checks below today, because ownership can only be established
     * through a pointer-shaped write. Kept because it states the actual requirement:
     * ownership of an incoming pointer only transfers once the mapper stores it. */
    if(subject->owns_value) return false;
    if(subject->input_type == LV_SUBJECT_TYPE_POINTER) return false;
    if(subject->input_type == LV_SUBJECT_TYPE_STRING) return false;
    return true;
}

/* The path every `lv_subject_set_...()` goes through. */
static void subject_input_written(lv_subject_t * subject, const void * borrowed_input,
                                  void * superseded_input, bool superseded_input_owned)
{
    if(!can_defer_mapper(subject)) {
        subject_commit_input(subject, borrowed_input, superseded_input, superseded_input_owned);
        return;
    }

    /* Lazy: record the input and go no further. A run of writes therefore costs one
     * evaluation instead of one per write, and a Subject nothing ever reads is never
     * evaluated at all. */
    subject->input_pending = 1;
    mark_dirty(subject);

    /* The dependents have to be marked stale even though it is not yet known whether the
     * value actually changed. Each of them does its own change detection when it
     * re-evaluates, so at worst this costs a re-evaluation that reports no change. */
    mark_dependents_dirty(subject);

    /* An eager dependent is due now, and evaluating it pulls this Subject, so the mapper
     * still runs inside this call whenever something downstream needs it. */
    process_dirty(false);
}

static void subject_commit_input(lv_subject_t * subject, const void * borrowed_input,
                                 void * superseded_input, bool superseded_input_owned)
{
    /* Remember what is about to be replaced. The mapper may still read it to decide
     * whether anything changed, so it can only be released afterwards. */
    void * outgoing = (void *)subject->value.pointer;
    bool outgoing_owned = subject->owns_value;

    /* The mapper runs here, so the stored value is the mapped one and the raw input is
     * never observable through a getter. */
    bool changed = subject->has_mapper ? run_mapper(subject, borrowed_input) : apply_input(subject, borrowed_input);
    subject->input_pending = 0;

    /* Settle who owns the value that is now stored. Ownership follows what the write
     * handed in: the input's if the mapper stored the input, the old value's if the
     * mapper kept it, and borrowed for anything else the mapper came up with on its own. */
    if(subject->type == LV_SUBJECT_TYPE_POINTER || subject->type == LV_SUBJECT_TYPE_STRING) {
        if(subject->value.pointer == subject->last_input.pointer) subject->owns_value = subject->owns_input;
        else if(subject->value.pointer == outgoing) subject->owns_value = outgoing_owned;
        else subject->owns_value = 0;
    }

    /* Now the mapper is done with them. A mapper that kept the old value, either by
     * returning false or by storing it again, keeps it alive. `outgoing` and
     * `superseded_input` are usually the same pointer, so release each at most once. */
    release_if_unreferenced(subject, outgoing, outgoing_owned);
    if(superseded_input != outgoing) release_if_unreferenced(subject, superseded_input, superseded_input_owned);

    subject->dirty = 0;
    if(!changed) {
        list_reposition(subject);
        return;
    }

    /* A real change, so anything that read the old value is out of date. */
    subject->version++;

    /* Phase 1: everything downstream is now stale. Always synchronous, so a read of a
     * dependent can never return a stale value. */
    mark_dependents_dirty(subject);

    /* Phase 2a: this Subject's own Observers. */
    if(subject_is_eager_now(subject)) {
        clear_pending(subject);
        notify(subject);
    }
    else {
        /* The value is up to date; the Observers wait for the next flush. This is what
         * makes LV_OBSERVER_MODE_BATCHED coalesce a run of writes into one notification. */
        mark_pending_notify(subject);
    }

    /* Phase 2b: evaluate the eager Subjects that just went stale. This runs whatever
     * this Subject's own mode is, because eagerness is a property of the *dependents*:
     * a plain lazy source feeding an eager derived Subject still has to drive it. Their
     * mappers pull their dependencies, so each evaluates once on consistent inputs. */
    process_dirty(false);
}


/* Would re-running this Subject's mapper be pointless?
 *
 * A dirty Subject was marked stale because *something* upstream might have changed, not
 * because anything definitely did. Bring its dependencies up to date and compare each
 * against the version it read last time. If they all still match, the mapper is a
 * function of unchanged inputs and cannot produce a different answer, so it is skipped —
 * and because skipping leaves this Subject's own version alone, its dependents skip too,
 * all the way up. One value that settles back to what it was therefore costs one
 * evaluation, not one per level.
 *
 * Only for a dependency-driven re-evaluation: a direct write goes through
 * `subject_input_written()` and always runs the mapper. */
static bool deps_are_unchanged(lv_subject_t * subject)
{
    /* A deferred write lands here too, and then the input itself is new, so the mapper
     * has to run whatever the dependencies say. */
    if(subject->input_pending) return false;

    uint32_t dep_cnt = ref_list_count(&subject->deps);
    if(dep_cnt == 0) return false;   /* nothing recorded yet, so it has to run */

    /* Two passes. Pulling a dependency runs arbitrary mapper and Observer code, which
     * could in principle delete a Subject and relink this list, so nothing is compared
     * while the list may be moving. */
    for(uint32_t i = 0; i < dep_cnt; i++) {
        subject_ref_t * ref = lv_ll_get_head(&subject->deps);
        for(uint32_t k = 0; k < i && ref != NULL; k++) ref = lv_ll_get_next(&subject->deps, ref);
        if(ref == NULL) return false;   /* the list changed under us: just run the mapper */
        subject_pull(ref->subject);
    }

    if(ref_list_count(&subject->deps) != dep_cnt) return false;

    subject_ref_t * ref;
    LV_LL_READ(&subject->deps, ref) {
        if(ref->seen_version != ref->subject->version) return false;
    }
    return true;
}

/* Re-evaluate a dirty Subject. */
static void subject_recompute(lv_subject_t * subject)
{
    if(deps_are_unchanged(subject)) {
        subject->dirty = 0;
        list_reposition(subject);
        return;
    }

    subject_commit_input(subject, NULL, NULL, false);
}

/* Called by every getter: bring a dirty Subject up to date before its value is read.
 *
 * This deliberately does not notify. A getter that notified would let any reader break
 * LV_OBSERVER_MODE_BATCHED's "once per frame" guarantee; pending notifications are the
 * flush's job. The value itself is always fresh. */
static void subject_pull(lv_subject_t * subject)
{
    if(!subject->dirty) return;
    subject_recompute(subject);
}

static void notify(lv_subject_t * subject)
{
    lv_global_t * global = LV_GLOBAL_DEFAULT();
    lv_subject_t * outer = global->subject_evaluating;
    /* Observer callbacks read Subjects all over LVGL. Without this they would register
     * as dependencies of whatever is being evaluated further up the stack. */
    global->subject_evaluating = NULL;

    lv_observer_t * observer;
    LV_LL_READ(&(subject->subs_ll), observer) {
        observer->notified = 0;
    }

    do {
        subject->notify_restart_query = 0;
        LV_LL_READ(&(subject->subs_ll), observer) {
            if(observer->cb && observer->notified == 0) {
                observer->cb(observer, subject);
                if(subject->notify_restart_query) break;
                observer->notified = 1;
            }
        }
    } while(subject->notify_restart_query);

    global->subject_evaluating = outer;
}



static bool set_mapper_allowed(lv_subject_t * subject, lv_subject_type_t type)
{
    LV_UNUSED(type);  /* only read by LV_CHECK_ARG, which compiles away when disabled */
    LV_CHECK_ARG(subject != NULL, return false);
    LV_CHECK_ARG(subject->type == type, return false);
    if(!subject->in_list) {
        LV_LOG_WARN("A mapper on an application-owned Subject is never re-evaluated by lv_subject_flush()");
    }
    return true;
}

/*---------------------------------------------------------------
 * Derived-subject helpers
 *
 * `lv_subject_create_min()` / `_max()` record the extremum a source has passed through.
 * `lv_subject_create_clamped()` mirrors a source bounded to a range.
 *
 * All of them keep their configuration in the mapper's user data, which the Subject
 * owns and frees. That is what lets one mapper serve every instance.
 *--------------------------------------------------------------*/

typedef struct {
    lv_subject_t * source;
    lv_subject_compare_cb_t compare_cb;  /**< NULL means natural ordering */
    lv_subject_value_t min_value;        /**< Clamp only */
    lv_subject_value_t max_value;        /**< Clamp only */
    bool want_max;                       /**< Extremum only */
    bool seeded;                         /**< Extremum only: has a first value been recorded? */
} derived_dsc_t;

int lv_subject_compare_int(lv_subject_t * subject, lv_subject_value_t a, lv_subject_value_t b)
{
    LV_UNUSED(subject);
    return (a.num > b.num) - (a.num < b.num);
}

#if LV_USE_FLOAT
int lv_subject_compare_float(lv_subject_t * subject, lv_subject_value_t a, lv_subject_value_t b)
{
    LV_UNUSED(subject);
    return (a.float_v > b.float_v) - (a.float_v < b.float_v);
}
#endif

int lv_subject_compare_color(lv_subject_t * subject, lv_subject_value_t a, lv_subject_value_t b)
{
    LV_UNUSED(subject);
    uint32_t av = lv_color_to_u32(a.color);
    uint32_t bv = lv_color_to_u32(b.color);
    return (av > bv) - (av < bv);
}

int32_t lv_subject_clamp_int(int32_t value, int32_t min_value, int32_t max_value)
{
    return LV_CLAMP(min_value, value, max_value);
}

#if LV_USE_FLOAT
float lv_subject_clamp_float(float value, float min_value, float max_value)
{
    return LV_CLAMP(min_value, value, max_value);
}
#endif

/* Ordering for the types that have one. A pointer or string Subject has no natural
 * ordering, which is why those need a compare callback. */
static int natural_compare(lv_subject_t * subject, lv_subject_value_t a, lv_subject_value_t b)
{
    switch(subject->type) {
        case LV_SUBJECT_TYPE_INT:
            return lv_subject_compare_int(subject, a, b);
#if LV_USE_FLOAT
        case LV_SUBJECT_TYPE_FLOAT:
            return lv_subject_compare_float(subject, a, b);
#endif
        case LV_SUBJECT_TYPE_COLOR:
            return lv_subject_compare_color(subject, a, b);
        default:
            LV_LOG_WARN("This subject type has no natural ordering, pass a compare callback");
            return 0;
    }
}

/* Read a Subject of any type. Reading is what registers the dependency. */
static lv_subject_value_t read_source(lv_subject_t * source)
{
    lv_subject_value_t v;
    lv_memzero(&v, sizeof(v));

    switch(source->type) {
        case LV_SUBJECT_TYPE_INT:
            v.num = lv_subject_get_int(source);
            break;
#if LV_USE_FLOAT
        case LV_SUBJECT_TYPE_FLOAT:
            v.float_v = lv_subject_get_float(source);
            break;
#endif
        case LV_SUBJECT_TYPE_COLOR:
            v.color = lv_subject_get_color(source);
            break;
        case LV_SUBJECT_TYPE_STRING:
            v.pointer = lv_subject_get_string(source);
            break;
        default:
            v.pointer = lv_subject_get_pointer(source);
            break;
    }
    return v;
}

/* Shared body of the extremum mappers: keep the source's value if it beats the stored
 * one. `*value` is the Subject's own slot, so the extremum survives across evaluations. */
static bool extremum_step(lv_subject_t * subject, void * user_data, lv_subject_value_t * value)
{
    derived_dsc_t * dsc = user_data;
    lv_subject_value_t next = read_source(dsc->source);

    if(!dsc->seeded) {
        dsc->seeded = true;
        *value = next;
        return true;
    }

    lv_subject_compare_cb_t compare = dsc->compare_cb ? dsc->compare_cb : natural_compare;
    int order = compare(subject, next, *value);
    if(dsc->want_max ? (order <= 0) : (order >= 0)) return false;

    *value = next;
    return true;
}

static bool extremum_int_mapper(lv_subject_t * subject, void * user_data, lv_subject_value_t input, int32_t * value)
{
    LV_UNUSED(input);
    lv_subject_value_t slot;
    lv_memzero(&slot, sizeof(slot));
    slot.num = *value;
    if(!extremum_step(subject, user_data, &slot)) return false;
    *value = slot.num;
    return true;
}

#if LV_USE_FLOAT
static bool extremum_float_mapper(lv_subject_t * subject, void * user_data, lv_subject_value_t input, float * value)
{
    LV_UNUSED(input);
    lv_subject_value_t slot;
    lv_memzero(&slot, sizeof(slot));
    slot.float_v = *value;
    if(!extremum_step(subject, user_data, &slot)) return false;
    *value = slot.float_v;
    return true;
}
#endif

static bool extremum_color_mapper(lv_subject_t * subject, void * user_data, lv_subject_value_t input,
                                  lv_color_t * value)
{
    LV_UNUSED(input);
    lv_subject_value_t slot;
    lv_memzero(&slot, sizeof(slot));
    slot.color = *value;
    if(!extremum_step(subject, user_data, &slot)) return false;
    *value = slot.color;
    return true;
}

static bool extremum_pointer_mapper(lv_subject_t * subject, void * user_data, lv_subject_value_t input,
                                    const void ** value)
{
    LV_UNUSED(input);
    lv_subject_value_t slot;
    lv_memzero(&slot, sizeof(slot));
    slot.pointer = *value;
    if(!extremum_step(subject, user_data, &slot)) return false;
    *value = slot.pointer;
    return true;
}

/* Bound the source's value, ordering with the compare callback so the same code works
 * for a colour or a struct pointer as for an int. */
static bool clamp_step(lv_subject_t * subject, void * user_data, lv_subject_value_t input,
                       lv_subject_value_t * value)
{
    derived_dsc_t * dsc = user_data;

    /* The bounds are the Subject's input: writing an lv_subject_range_t re-clamps.
     * The values are copied out, so the caller's struct may be transient. */
    if(input.pointer != NULL) {
        const lv_subject_range_t * range = input.pointer;
        dsc->min_value = range->min_value;
        dsc->max_value = range->max_value;
    }

    lv_subject_value_t next = read_source(dsc->source);
    lv_subject_compare_cb_t compare = dsc->compare_cb ? dsc->compare_cb : natural_compare;

    if(compare(subject, next, dsc->min_value) < 0) next = dsc->min_value;
    else if(compare(subject, next, dsc->max_value) > 0) next = dsc->max_value;

    /* The first evaluation always stores. The slot starts out neutral (NULL for a
     * pointer Subject), which a compare callback cannot meaningfully order against. */
    if(!dsc->seeded) {
        dsc->seeded = true;
        *value = next;
        return true;
    }

    if(compare(subject, next, *value) == 0) return false;
    *value = next;
    return true;
}

static bool clamp_int_mapper(lv_subject_t * subject, void * user_data, lv_subject_value_t input, int32_t * value)
{
    lv_subject_value_t slot;
    lv_memzero(&slot, sizeof(slot));
    slot.num = *value;
    if(!clamp_step(subject, user_data, input, &slot)) return false;
    *value = slot.num;
    return true;
}

#if LV_USE_FLOAT
static bool clamp_float_mapper(lv_subject_t * subject, void * user_data, lv_subject_value_t input, float * value)
{
    lv_subject_value_t slot;
    lv_memzero(&slot, sizeof(slot));
    slot.float_v = *value;
    if(!clamp_step(subject, user_data, input, &slot)) return false;
    *value = slot.float_v;
    return true;
}
#endif

static bool clamp_color_mapper(lv_subject_t * subject, void * user_data, lv_subject_value_t input, lv_color_t * value)
{
    lv_subject_value_t slot;
    lv_memzero(&slot, sizeof(slot));
    slot.color = *value;
    if(!clamp_step(subject, user_data, input, &slot)) return false;
    *value = slot.color;
    return true;
}

static bool clamp_pointer_mapper(lv_subject_t * subject, void * user_data, lv_subject_value_t input,
                                 const void ** value)
{
    lv_subject_value_t slot;
    lv_memzero(&slot, sizeof(slot));
    slot.pointer = *value;
    if(!clamp_step(subject, user_data, input, &slot)) return false;
    *value = slot.pointer;
    return true;
}

/* Create the derived Subject, attach the right typed mapper for the source's type, and
 * hand it ownership of `dsc`. */
static lv_subject_t * create_derived(lv_subject_t * source, derived_dsc_t * dsc, bool is_clamp)
{
    lv_subject_t * subject = lv_subject_create((lv_subject_type_t)source->type);
    if(subject == NULL) {
        lv_free(dsc);
        return NULL;
    }

    subject->owns_mapper_user_data = 1;

    /* A clamped Subject takes its bounds through its own setter, as an
     * lv_subject_range_t, so it is written as a pointer whatever its value type is. */
    if(is_clamp) subject->input_type = (uint32_t)LV_SUBJECT_TYPE_POINTER;

    /* An extremum has to see every value the source passes through, not only the ones
     * somebody happened to read, so it is eager. A clamp is a pure mirror and stays lazy. */
    if(!is_clamp) subject->mode = 1U; /* LV_SUBJECT_MODE_EAGER */

    switch(source->type) {
        case LV_SUBJECT_TYPE_INT:
            lv_subject_set_int_mapper(subject, is_clamp ? clamp_int_mapper : extremum_int_mapper, dsc);
            break;
#if LV_USE_FLOAT
        case LV_SUBJECT_TYPE_FLOAT:
            lv_subject_set_float_mapper(subject, is_clamp ? clamp_float_mapper : extremum_float_mapper, dsc);
            break;
#endif
        case LV_SUBJECT_TYPE_COLOR:
            lv_subject_set_color_mapper(subject, is_clamp ? clamp_color_mapper : extremum_color_mapper, dsc);
            break;
        case LV_SUBJECT_TYPE_STRING:
            /* A string Subject owns a buffer, so this would have to copy into it. Track
             * the winning string through a pointer Subject instead. */
            LV_LOG_WARN("The min/max/clamped helpers do not support string subjects, "
                        "use a pointer subject");
            subject->owns_mapper_user_data = 0;
            lv_subject_delete(subject);
            lv_free(dsc);
            return NULL;
        default:
            lv_subject_set_pointer_mapper(subject, is_clamp ? clamp_pointer_mapper : extremum_pointer_mapper, dsc);
            break;
    }

    return subject;
}

static derived_dsc_t * derived_dsc_create(lv_subject_t * source, lv_subject_compare_cb_t compare_cb)
{
    derived_dsc_t * dsc = lv_malloc_zeroed(sizeof(derived_dsc_t));
    LV_ASSERT_MALLOC(dsc);
    if(dsc == NULL) return NULL;
    dsc->source = source;
    dsc->compare_cb = compare_cb;
    return dsc;
}

/* A type with no natural ordering cannot be compared without a callback. */
static bool ordering_available(const lv_subject_t * source, lv_subject_compare_cb_t compare_cb)
{
    if(compare_cb != NULL) return true;
    if(source->type == LV_SUBJECT_TYPE_INT || source->type == LV_SUBJECT_TYPE_FLOAT ||
       source->type == LV_SUBJECT_TYPE_COLOR) {
        return true;
    }
    LV_LOG_WARN("This subject type has no natural ordering, pass a compare callback");
    return false;
}

lv_subject_t * lv_subject_create_min(lv_subject_t * source, lv_subject_compare_cb_t compare_cb)
{
    LV_CHECK_ARG(source != NULL, return NULL);
    LV_CHECK_ARG(source->type != LV_SUBJECT_TYPE_INVALID && source->type != LV_SUBJECT_TYPE_NONE, return NULL);
    if(!ordering_available(source, compare_cb)) return NULL;

    derived_dsc_t * dsc = derived_dsc_create(source, compare_cb);
    if(dsc == NULL) return NULL;
    dsc->want_max = false;
    return create_derived(source, dsc, false);
}

lv_subject_t * lv_subject_create_max(lv_subject_t * source, lv_subject_compare_cb_t compare_cb)
{
    LV_CHECK_ARG(source != NULL, return NULL);
    LV_CHECK_ARG(source->type != LV_SUBJECT_TYPE_INVALID && source->type != LV_SUBJECT_TYPE_NONE, return NULL);
    if(!ordering_available(source, compare_cb)) return NULL;

    derived_dsc_t * dsc = derived_dsc_create(source, compare_cb);
    if(dsc == NULL) return NULL;
    dsc->want_max = true;
    return create_derived(source, dsc, false);
}

lv_subject_t * lv_subject_create_clamped(lv_subject_t * source, lv_subject_value_t min_value,
                                         lv_subject_value_t max_value, lv_subject_compare_cb_t compare_cb)
{
    LV_CHECK_ARG(source != NULL, return NULL);
    LV_CHECK_ARG(source->type != LV_SUBJECT_TYPE_INVALID && source->type != LV_SUBJECT_TYPE_NONE, return NULL);
    if(!ordering_available(source, compare_cb)) return NULL;

    derived_dsc_t * dsc = derived_dsc_create(source, compare_cb);
    if(dsc == NULL) return NULL;
    dsc->min_value = min_value;
    dsc->max_value = max_value;
    return create_derived(source, dsc, true);
}

/*---------------------------------------------------------------
 * Observer mappers
 *--------------------------------------------------------------*/

/* Create an Observer that maps the Subject's value before pushing it to the Widget.
 * The mapper has to be in place before the Observer's first notification, which is
 * why it is passed at bind time instead of set afterwards. */
static lv_observer_t * bind_mapped(lv_obj_t * obj, lv_subject_t * subject, lv_observer_cb_t cb, void (*set_cb)(void),
                                   lv_observer_mapper_t mapper, void * user_data, lv_style_selector_t selector)
{
    settle_before_subscribe(subject);

    lv_observer_t * observer = lv_ll_ins_tail(&(subject->subs_ll));
    LV_ASSERT_MALLOC(observer);
    if(observer == NULL) {
        LV_LOG_WARN("Couldn't add observer to subject");
        return NULL;
    }

    lv_memzero(observer, sizeof(*observer));
    observer->subject = subject;
    observer->cb = cb;
    observer->target = obj;
    observer->user_cb = set_cb;
    observer->mapper = mapper;
    observer->user_data = user_data;
    observer->style_selector = selector;
    observer->has_mapper = 1;
    observer->for_obj = 1;
    subject->immediate_observer_cnt++;  /* LV_OBSERVER_MODE_IMMEDIATE is the default */

    if(obj != NULL) lv_obj_add_event_cb(obj, unsubscribe_on_delete_cb, LV_EVENT_DELETE, observer);

    cb(observer, subject);

    return observer;
}


static void init_common(lv_subject_t * subject)
{
    LV_ASSERT(subject != NULL);
    lv_memzero(subject, sizeof(*subject));
    lv_ll_init(&(subject->subs_ll), sizeof(lv_observer_t));
    lv_ll_init(&(subject->deps), sizeof(subject_ref_t));
    lv_ll_init(&(subject->dependents), sizeof(subject_ref_t));
}
static void init_int(lv_subject_t * subject, int32_t value)
{
    LV_ASSERT(subject != NULL);
    subject->type = LV_SUBJECT_TYPE_INT;
    subject->input_type = LV_SUBJECT_TYPE_INT;
    subject->value.num = value;
    subject->last_input.num = value;
}

#if LV_USE_FLOAT

static void init_float(lv_subject_t * subject, float value)
{
    LV_ASSERT(subject != NULL);
    subject->type = LV_SUBJECT_TYPE_FLOAT;
    subject->input_type = LV_SUBJECT_TYPE_FLOAT;
    subject->value.float_v = value;
    subject->last_input.float_v = value;
}

#endif /*LV_USE_FLOAT*/

static void init_pointer(lv_subject_t * subject, void * value)
{
    LV_ASSERT(subject != NULL);
    subject->type = LV_SUBJECT_TYPE_POINTER;
    subject->input_type = LV_SUBJECT_TYPE_POINTER;
    subject->value.pointer = value;
    subject->last_input.pointer = value;
}

static void init_color(lv_subject_t * subject, lv_color_t color)
{
    LV_ASSERT(subject != NULL);
    subject->type = LV_SUBJECT_TYPE_COLOR;
    subject->input_type = LV_SUBJECT_TYPE_COLOR;
    subject->value.color = color;
    subject->last_input.color = color;
}



static void init_string(lv_subject_t * subject, char * buf, size_t size, const char * value)
{
    LV_ASSERT(subject != NULL);
    LV_ASSERT(buf != NULL || size == 0);

    subject->type = LV_SUBJECT_TYPE_STRING;
    subject->input_type = LV_SUBJECT_TYPE_STRING;

    if(buf != NULL && size > 0) {
        lv_subject_buf_t * b = lv_malloc_zeroed(sizeof(lv_subject_buf_t));
        LV_ASSERT_MALLOC(b);
        if(b != NULL) {
            b->buf = buf;
            b->capacity = size;
            subject->buf = b;
            lv_strlcpy(buf, value, size);
            b->length = lv_strlen(buf);
        }
    }
    subject->value.pointer = buf;
}

static void deinit(lv_subject_t * subject)
{
    LV_ASSERT(subject != NULL);

    /* Both directions, so deleting a Subject in the middle of a graph leaves no
     * dangling edge in its dependencies or its dependents. */
    edges_teardown(subject);
    subject->has_mapper = 0;
    subject->dirty = 0;
    subject->pending_notify = 0;

    /* A copying Subject's storage goes with it, released by its own free_cb. */
    buf_release(subject);

    /* Whatever the Subject owns goes with it: the stored value, and the retained input
     * if it is a different pointer. */
    {
        void * stored = subject->owns_value ? (void *)subject->value.pointer : NULL;
        void * retained = subject->owns_input ? (void *)subject->last_input.pointer : NULL;

        subject->value.pointer = NULL;
        subject->last_input.pointer = NULL;
        subject->owns_value = 0;
        subject->owns_input = 0;

        if(stored != NULL) {
            if(subject->value_free_cb) subject->value_free_cb(stored);
            else lv_free(stored);
        }
        if(retained != NULL && retained != stored) {
            if(subject->value_free_cb) subject->value_free_cb(retained);
            else lv_free(retained);
        }
    }

    /* Captured state allocated by one of the lv_subject_create_...() helpers. */
    if(subject->owns_mapper_user_data) {
        lv_free(subject->mapper_user_data);
        subject->mapper_user_data = NULL;
        subject->owns_mapper_user_data = 0;
    }

    lv_observer_t * observer = lv_ll_get_head(&subject->subs_ll);
    while(observer) {
        lv_observer_t * observer_next = lv_ll_get_next(&subject->subs_ll, observer);

        lv_observer_delete(observer);
        observer = observer_next;
    }

#if LV_USE_EXT_DATA
    if(subject->ext_data.free_cb) {
        subject->ext_data.free_cb(subject->ext_data.data);
        subject->ext_data.data = NULL;
    }
#endif

    lv_ll_clear(&subject->subs_ll);
}

static void subject_toggle_cb(lv_event_t * e)
{
    LV_ASSERT(e != NULL);
    lv_subject_t * subject = lv_event_get_user_data(e);
    int32_t v = lv_subject_get_int(subject);
    v = !v;

    lv_subject_set_int(subject, v);
}

static void subject_set_int_cb(lv_event_t * e)
{
    LV_ASSERT(e != NULL);
    subject_set_int_user_data_t * user_data = lv_event_get_user_data(e);
    lv_subject_set_int(user_data->subject, user_data->value);
}


#if LV_USE_FLOAT
static void subject_set_float_cb(lv_event_t * e)
{
    LV_ASSERT(e != NULL);
    subject_set_float_user_data_t * user_data = lv_event_get_user_data(e);
    lv_subject_set_float(user_data->subject, user_data->value);
}
#endif

static void subject_set_string_cb(lv_event_t * e)
{
    LV_ASSERT(e != NULL);
    subject_set_string_user_data_t * user_data = lv_event_get_user_data(e);
    lv_subject_copy_string(user_data->subject, user_data->value);
}

static void subject_increment_cb(lv_event_t * e)
{
    LV_ASSERT(e != NULL);
    lv_subject_increment_dsc_t * user_data = lv_event_get_user_data(e);

    if(user_data->subject->type == LV_SUBJECT_TYPE_INT) {
        /*Use the smaller range*/
        int32_t max_value = user_data->max_value;
        int32_t min_value = user_data->min_value;

        int32_t value = lv_subject_get_int(user_data->subject);
        value += user_data->step;

        if(user_data->rollover) {
            if(value > max_value) {
                value = min_value;
            }
            else if(value < min_value) {
                value = max_value;
            }
        }
        else {
            value = LV_CLAMP(min_value, value, max_value);
        }

        lv_subject_set_int(user_data->subject, value);
    }
#if LV_USE_FLOAT
    else if(user_data->subject->type == LV_SUBJECT_TYPE_FLOAT) {
        /*Use the smaller range*/
        float max_value = (float)user_data->max_value;
        float min_value = (float)user_data->min_value;


        float value = lv_subject_get_float(user_data->subject);
        value += (float)user_data->step;

        if(user_data->rollover) {
            if(value > max_value) {
                value = min_value;
            }
            else if(value < min_value) {
                value = max_value;
            }
        }
        else {
            value = LV_CLAMP(min_value, value, max_value);
        }

        lv_subject_set_float(user_data->subject, value);
    }
#endif
}


static void unsubscribe_on_delete_cb(lv_event_t * e)
{
    LV_ASSERT(e != NULL);
    lv_observer_t * observer = lv_event_get_user_data(e);
    lv_observer_delete(observer);
}

static lv_observer_t * bind_to_bitfield(lv_subject_t * subject, lv_obj_t * obj, lv_observer_cb_t cb, uint32_t flag,
                                        int32_t ref_value, bool inv, flag_cond_t cond)
{
    LV_ASSERT(subject != NULL);
    LV_ASSERT(obj != NULL);
    LV_ASSERT(subject->type == LV_SUBJECT_TYPE_INT);


    flag_and_cond_t * p = lv_malloc(sizeof(flag_and_cond_t));
    if(p == NULL) {
        LV_LOG_WARN("Out of memory");
        return NULL;
    }

    p->flag = flag;
    p->value.num = ref_value;
    p->inv = inv;
    p->cond = cond;

    lv_observer_t * observable = lv_subject_add_observer_obj(subject, cb, obj, p);
    observable->auto_free_user_data = 1;

    return observable;
}

static void obj_flag_observer_cb(lv_observer_t * observer, lv_subject_t * subject)
{
    LV_ASSERT(observer != NULL);
    LV_ASSERT(subject != NULL);
    flag_and_cond_t * p = observer->user_data;
    LV_ASSERT(p != NULL);

    /* Initializing this keeps some compilers happy */
    bool res = false;
    switch(p->cond) {
        case FLAG_COND_EQ:
            res = subject->value.num == p->value.num;
            break;
        case FLAG_COND_GT:
            res = subject->value.num > p->value.num;
            break;
        case FLAG_COND_GE:
            res = subject->value.num >= p->value.num;
            break;
    }
    if(p->inv) res = !res;

    /*TODO: the flag binding API is deprecated separately; suppress the warning until then*/
    LV_DEPRECATIONS_IGNORE_BEGIN
    if(res) {
        lv_obj_add_flag(observer->target, p->flag);
    }
    else {
        lv_obj_remove_flag(observer->target, p->flag);
    }
    LV_DEPRECATIONS_IGNORE_END
}

static void obj_state_observer_cb(lv_observer_t * observer, lv_subject_t * subject)
{
    LV_ASSERT(observer != NULL);
    LV_ASSERT(subject != NULL);
    flag_and_cond_t * p = observer->user_data;
    LV_ASSERT(p != NULL);

    /* Initializing this keeps some compilers happy */
    bool res = false;
    switch(p->cond) {
        case FLAG_COND_EQ:
            res = subject->value.num == p->value.num;
            break;
        case FLAG_COND_GT:
            res = subject->value.num > p->value.num;
            break;
        case FLAG_COND_GE:
            res = subject->value.num >= p->value.num;
            break;
    }
    if(p->inv) res = !res;

    if(res) {
        lv_obj_add_state(observer->target, p->flag);
    }
    else {
        lv_obj_remove_state(observer->target, p->flag);
    }
}

static void obj_value_changed_event_cb(lv_event_t * e)
{
    LV_ASSERT(e != NULL);
    lv_obj_t * obj = lv_event_get_current_target(e);
    lv_subject_t * subject = lv_event_get_user_data(e);
    LV_ASSERT(subject != NULL);
    LV_ASSERT(obj != NULL);

    lv_subject_set_int(subject, lv_obj_has_state(obj, LV_STATE_CHECKED));
}


static void subject_set_string_free_user_data_event_cb(lv_event_t * e)
{
    LV_ASSERT(e != NULL);
    subject_set_string_user_data_t * user_data = lv_event_get_user_data(e);
    LV_ASSERT(user_data != NULL);
    lv_free((void *)user_data->value);
    lv_free(user_data);
}

static void set_bool_observer(lv_observer_t * observer, lv_subject_t * subject)
{
    lv_obj_t * obj = (lv_obj_t *)observer->target;
    lv_obj_set_bool_t set_bool_cb = (lv_obj_set_bool_t)observer->user_cb;
    if(set_bool_cb == NULL) return;

    bool value;
    if(observer->has_mapper) {
        value = observer->out.num != 0;
        if(!observer->mapper.bool_cb(observer, &value)) return;
        observer->out.num = value ? 1 : 0;
    }
    else {
        value = subject->value.num;
    }
    set_bool_cb(obj, value);
}

static void set_int_observer(lv_observer_t * observer, lv_subject_t * subject)
{
    lv_obj_t * obj = (lv_obj_t *)observer->target;
    lv_obj_set_int_t set_int_cb = (lv_obj_set_int_t)observer->user_cb;
    if(set_int_cb == NULL) return;

    int32_t value;
    if(observer->has_mapper) {
        value = observer->out.num;
        if(!observer->mapper.int_cb(observer, &value)) return;
        observer->out.num = value;
    }
    else {
        value = subject->value.num;
    }
    set_int_cb(obj, value);
}

#if LV_USE_FLOAT
static void set_float_observer(lv_observer_t * observer, lv_subject_t * subject)
{
    lv_obj_t * obj = (lv_obj_t *)observer->target;
    lv_obj_set_float_t set_float_cb = (lv_obj_set_float_t)observer->user_cb;
    if(set_float_cb == NULL) return;

    float value;
    if(observer->has_mapper) {
        value = observer->out.float_v;
        if(!observer->mapper.float_cb(observer, &value)) return;
        observer->out.float_v = value;
    }
    else {
        value = subject->value.float_v;
    }
    set_float_cb(obj, value);
}
#endif /*LV_USE_FLOAT*/

static void set_string_observer(lv_observer_t * observer, lv_subject_t * subject)
{
    lv_obj_t * obj = (lv_obj_t *)observer->target;
    lv_obj_set_string_t set_string_cb = (lv_obj_set_string_t)observer->user_cb;
    if(set_string_cb == NULL) return;

    const char * value;
    if(observer->has_mapper) {
        value = observer->out.pointer;
        if(!observer->mapper.string_cb(observer, &value)) return;
        observer->out.pointer = value;  /* the mapper owns the storage, we keep the pointer */
    }
    else {
        value = subject->value.pointer;
    }
    set_string_cb(obj, value);
}


static void set_color_observer(lv_observer_t * observer, lv_subject_t * subject)
{
    lv_obj_t * obj = (lv_obj_t *)observer->target;
    lv_obj_set_color_t set_color_cb = (lv_obj_set_color_t)observer->user_cb;
    if(set_color_cb == NULL) return;

    lv_color_t value;
    if(observer->has_mapper) {
        value = observer->out.color;
        if(!observer->mapper.color_cb(observer, &value)) return;
        observer->out.color = value;
    }
    else {
        value = subject->value.color;
    }
    set_color_cb(obj, value);
}

/* A style setter needs a selector as well as a value, so the Observer carries one. It
 * has its own field rather than sharing the output slot, because a mapped style binding
 * needs both. */
static lv_observer_t * bind_style(lv_obj_t * obj, lv_subject_t * subject, lv_observer_cb_t cb,
                                  void (*setter)(void), lv_style_selector_t selector)
{
    lv_observer_t * observer = lv_subject_add_observer_obj(subject, cb, obj, NULL);
    if(observer == NULL) {
        LV_LOG_WARN("Couldn't add observer to subject");
        return NULL;
    }

    observer->user_cb = setter;
    observer->style_selector = selector;
    cb(observer, subject);
    return observer;
}

lv_observer_t * lv_obj_bind_style_int(lv_obj_t * obj, lv_subject_t * subject, lv_obj_set_style_int_t setter,
                                      lv_style_selector_t selector)
{
    LV_CHECK_ARG(obj != NULL, return NULL);
    LV_CHECK_ARG(subject != NULL, return NULL);
    LV_CHECK_ARG(setter != NULL, return NULL);
    LV_CHECK_ARG(subject->type == LV_SUBJECT_TYPE_INT, return NULL);

    return bind_style(obj, subject, set_style_int_observer, (void (*)(void))setter, selector);
}

lv_observer_t * lv_obj_bind_style_color(lv_obj_t * obj, lv_subject_t * subject, lv_obj_set_style_color_t setter,
                                        lv_style_selector_t selector)
{
    LV_CHECK_ARG(obj != NULL, return NULL);
    LV_CHECK_ARG(subject != NULL, return NULL);
    LV_CHECK_ARG(setter != NULL, return NULL);
    LV_CHECK_ARG(subject->type == LV_SUBJECT_TYPE_COLOR, return NULL);

    return bind_style(obj, subject, set_style_color_observer, (void (*)(void))setter, selector);
}

lv_observer_t * lv_obj_bind_style_opa(lv_obj_t * obj, lv_subject_t * subject, lv_obj_set_style_opa_t setter,
                                      lv_style_selector_t selector)
{
    LV_CHECK_ARG(obj != NULL, return NULL);
    LV_CHECK_ARG(subject != NULL, return NULL);
    LV_CHECK_ARG(setter != NULL, return NULL);
    LV_CHECK_ARG(subject->type == LV_SUBJECT_TYPE_INT, return NULL);

    return bind_style(obj, subject, set_style_opa_observer, (void (*)(void))setter, selector);
}

lv_observer_t * lv_obj_bind_style_int_mapped(lv_obj_t * obj, lv_subject_t * subject,
                                             lv_obj_set_style_int_t setter, lv_style_selector_t selector,
                                             lv_observer_int_mapper_t mapper, void * user_data)
{
    LV_CHECK_ARG(obj != NULL, return NULL);
    LV_CHECK_ARG(subject != NULL, return NULL);
    LV_CHECK_ARG(setter != NULL, return NULL);
    LV_CHECK_ARG(mapper != NULL, return NULL);
    /* No type check on `subject`: mapping the value is the whole point. */

    lv_observer_mapper_t m = { 0 };
    m.int_cb = mapper;
    return bind_mapped(obj, subject, set_style_int_observer, (void (*)(void))setter, m, user_data, selector);
}

lv_observer_t * lv_obj_bind_style_color_mapped(lv_obj_t * obj, lv_subject_t * subject,
                                               lv_obj_set_style_color_t setter, lv_style_selector_t selector,
                                               lv_observer_color_mapper_t mapper, void * user_data)
{
    LV_CHECK_ARG(obj != NULL, return NULL);
    LV_CHECK_ARG(subject != NULL, return NULL);
    LV_CHECK_ARG(setter != NULL, return NULL);
    LV_CHECK_ARG(mapper != NULL, return NULL);

    lv_observer_mapper_t m = { 0 };
    m.color_cb = mapper;
    return bind_mapped(obj, subject, set_style_color_observer, (void (*)(void))setter, m, user_data, selector);
}

lv_observer_t * lv_obj_bind_style_opa_mapped(lv_obj_t * obj, lv_subject_t * subject,
                                             lv_obj_set_style_opa_t setter, lv_style_selector_t selector,
                                             lv_observer_int_mapper_t mapper, void * user_data)
{
    LV_CHECK_ARG(obj != NULL, return NULL);
    LV_CHECK_ARG(subject != NULL, return NULL);
    LV_CHECK_ARG(setter != NULL, return NULL);
    LV_CHECK_ARG(mapper != NULL, return NULL);

    lv_observer_mapper_t m = { 0 };
    m.int_cb = mapper;
    return bind_mapped(obj, subject, set_style_opa_observer, (void (*)(void))setter, m, user_data, selector);
}

static void set_style_int_observer(lv_observer_t * observer, lv_subject_t * subject)
{
    lv_obj_set_style_int_t setter = (lv_obj_set_style_int_t)observer->user_cb;
    if(setter == NULL) return;

    int32_t value;
    if(observer->has_mapper) {
        value = observer->out.num;
        if(!observer->mapper.int_cb(observer, &value)) return;
        observer->out.num = value;
    }
    else {
        value = subject->value.num;
    }
    setter(lv_observer_get_target_obj(observer), value, observer->style_selector);
}

static void set_style_color_observer(lv_observer_t * observer, lv_subject_t * subject)
{
    lv_obj_set_style_color_t setter = (lv_obj_set_style_color_t)observer->user_cb;
    if(setter == NULL) return;

    lv_color_t value;
    if(observer->has_mapper) {
        value = observer->out.color;
        if(!observer->mapper.color_cb(observer, &value)) return;
        observer->out.color = value;
    }
    else {
        value = subject->value.color;
    }
    setter(lv_observer_get_target_obj(observer), value, observer->style_selector);
}

static void set_style_opa_observer(lv_observer_t * observer, lv_subject_t * subject)
{
    lv_obj_set_style_opa_t setter = (lv_obj_set_style_opa_t)observer->user_cb;
    if(setter == NULL) return;

    int32_t value;
    if(observer->has_mapper) {
        value = observer->out.num;
        if(!observer->mapper.int_cb(observer, &value)) return;
        observer->out.num = value;
    }
    else {
        value = subject->value.num;
    }

    /* An opacity is a byte, so bound the value rather than truncating it. */
    setter(lv_observer_get_target_obj(observer), (lv_opa_t)lv_subject_clamp_int(value, 0, 255),
           observer->style_selector);
}

static void set_pointer_observer(lv_observer_t * observer, lv_subject_t * subject)
{
    lv_obj_t * obj = (lv_obj_t *)observer->target;
    lv_obj_set_pointer_t set_pointer_cb = (lv_obj_set_pointer_t)observer->user_cb;
    if(set_pointer_cb == NULL) return;

    const void * value;
    if(observer->has_mapper) {
        value = observer->out.pointer;
        if(!observer->mapper.pointer_cb(observer, &value)) return;
        observer->out.pointer = value;
    }
    else {
        value = subject->value.pointer;
    }
    set_pointer_cb(obj, value);
}



#endif /*LV_USE_OBSERVER*/
