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
#define txn_writes LV_GLOBAL_DEFAULT()->subject_txn_writes

/* How deep a chain of Observers writing Subjects may go before it is cut. */
#define LV_SUBJECT_MAX_NOTIFY_DEPTH 8

/* One entry of a transaction's write set: what a Subject held before the transaction
 * first changed it. Recorded once per Subject per transaction, so it is the state to go
 * back to if a setter fails. */
typedef struct {
    lv_subject_t * subject;
    lv_subject_value_t old_value;
    uint32_t old_version;
    uint32_t old_changed_at;
    uint8_t restorable;      /**< Can the value itself be put back? */
    uint8_t old_owned;       /**< Did the Subject own `old_value`? Its release waits for the
                              *   commit, because freeing cannot be undone. */
    uint8_t direct;          /**< Written by a setter, rather than recomputed */
    uint8_t old_pending;     /**< Was a notification already owed before the transaction?
                              *   A rollback queues none of its own, but must not swallow
                              *   one that was outstanding. */
    lv_subject_value_t direct_value;
    void * snapshot;         /**< A copying Subject's bytes before the transaction @nullable */
    size_t snapshot_len;
} txn_write_t;

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

/* Transactions */
static void txn_enter(void);
static void txn_leave(void);
static void txn_record_change(lv_subject_t * subject);
static void txn_abort(void);
static bool txn_setter_write_ok(lv_subject_t * subject, lv_subject_value_t v);
static bool run_setter(lv_subject_t * subject, lv_subject_value_t value);
static bool mapped_write(lv_subject_t * subject, lv_subject_value_t value);
static bool copy_write_handled(lv_subject_t * subject, const void * data, bool * ok);
static bool static_deps_contain(const lv_subject_t * subject, const lv_subject_t * dep);
static void static_deps_release(lv_subject_t * subject);
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

    if(subject->value_free_cb) subject->value_free_cb(p);
    else lv_free(p);
}

static void set_pointer_value(lv_subject_t * subject, const void * ptr, bool owned,
                              lv_subject_value_free_cb_t free_cb);
static bool buf_reserve(lv_subject_t * subject, size_t needed, bool warn);
static void buf_release(lv_subject_t * subject);
static size_t buf_capacity(const lv_subject_t * subject);
static void release_if_unreferenced(lv_subject_t * subject, void * p, bool owned);
static bool store_written(lv_subject_t * subject, lv_subject_value_t v, const void * str);
static bool run_mapper(lv_subject_t * subject);
static void publish(lv_subject_t * subject, bool changed);
static void subject_evaluate(lv_subject_t * subject);
static void subject_write(lv_subject_t * subject, lv_subject_value_t v, const void * str,
                          bool owned, void * superseded, bool superseded_owned);
static bool deps_are_unchanged(lv_subject_t * subject);
static void subject_recompute(lv_subject_t * subject);
static void subject_pull(lv_subject_t * subject);
static void notify(lv_subject_t * subject);
static void settle_before_subscribe(lv_subject_t * subject);
static bool set_mapper_allowed(lv_subject_t * subject, lv_subject_type_t type);

/* Derived-subject helpers */
static int natural_compare(lv_subject_t * subject, lv_subject_value_t a, lv_subject_value_t b);
static lv_subject_value_t read_source(lv_subject_t * source);
static bool clamp_step(lv_subject_t * subject, void * user_data, lv_subject_value_t * value);
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
    global->subject_txn_depth = 0;
    global->subject_txn_id = 0;
    global->subject_txn_aborting = 0;
    lv_ll_init(&subject_list, sizeof(lv_subject_t));
    lv_ll_init(&txn_writes, sizeof(txn_write_t));
}
void lv_subject_global_deinit(void)
{
    lv_global_t * global = LV_GLOBAL_DEFAULT();
    /* Only drop the reference. `lv_timer_core_deinit()` runs earlier in `lv_deinit()`
     * and has already destroyed every timer, so deleting it here would free it twice. */
    global->subject_flush_timer = NULL;
    global->subject_evaluating = NULL;
    lv_ll_clear(&txn_writes);
    global->subject_txn_depth = 0;

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

    /* A mapper reading something it never declared is the use-after-free arriving by
     * another route: that Subject looks deletable to `lv_subject_delete()`. Only checked
     * once a declaration exists, so a hand-written mapper that declares nothing is left
     * alone. */
    if(reader->static_deps != NULL && !static_deps_contain(reader, subject)) {
        LV_LOG_WARN("A mapper read a Subject that is not in its static dependencies, so that "
                    "Subject can still be deleted while this one needs it. Add it to the list "
                    "passed to lv_subject_set_static_deps().");
    }

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
/* A setter only means something on a Subject whose value its mapper owns. */
static bool set_setter_allowed(lv_subject_t * subject, lv_subject_type_t type)
{
    LV_UNUSED(type);
    LV_CHECK_ARG(subject != NULL, return false);
    LV_CHECK_ARG(subject->type == type, return false);
    if(!subject->has_mapper) {
        LV_LOG_WARN("A setter is only useful on a Subject that has a mapper. A plain Subject "
                    "is already writable.");
        return false;
    }
    return true;
}

void lv_subject_set_int_setter(lv_subject_t * subject, lv_subject_int_setter_t setter, void * user_data)
{
    if(!set_setter_allowed(subject, LV_SUBJECT_TYPE_INT)) return;
    subject->setter.int_cb = setter;
    subject->setter_user_data = user_data;
    subject->has_setter = setter != NULL;
}

#if LV_USE_FLOAT
void lv_subject_set_float_setter(lv_subject_t * subject, lv_subject_float_setter_t setter, void * user_data)
{
    if(!set_setter_allowed(subject, LV_SUBJECT_TYPE_FLOAT)) return;
    subject->setter.float_cb = setter;
    subject->setter_user_data = user_data;
    subject->has_setter = setter != NULL;
}
#endif

void lv_subject_set_color_setter(lv_subject_t * subject, lv_subject_color_setter_t setter, void * user_data)
{
    if(!set_setter_allowed(subject, LV_SUBJECT_TYPE_COLOR)) return;
    subject->setter.color_cb = setter;
    subject->setter_user_data = user_data;
    subject->has_setter = setter != NULL;
}

void lv_subject_set_pointer_setter(lv_subject_t * subject, lv_subject_pointer_setter_t setter,
                                   void * user_data)
{
    if(!set_setter_allowed(subject, LV_SUBJECT_TYPE_POINTER)) return;
    subject->setter.pointer_cb = setter;
    subject->setter_user_data = user_data;
    subject->has_setter = setter != NULL;
}

void lv_subject_set_string_setter(lv_subject_t * subject, lv_subject_string_setter_t setter,
                                  void * user_data)
{
    if(!set_setter_allowed(subject, LV_SUBJECT_TYPE_STRING)) return;
    subject->setter.string_cb = setter;
    subject->setter_user_data = user_data;
    subject->has_setter = setter != NULL;
}

bool lv_subject_is_writable(const lv_subject_t * subject)
{
    LV_CHECK_ARG(subject != NULL, return false);
    return !subject->has_mapper || subject->has_setter;
}

/* Drop the references a Subject's static declaration holds on other Subjects. */
static void static_deps_release(lv_subject_t * subject)
{
    for(uint32_t i = 0; i < subject->static_dep_cnt; i++) {
        lv_subject_t * dep = subject->static_deps[i];
        if(dep != NULL && dep->static_ref_cnt > 0) dep->static_ref_cnt--;
    }
    subject->static_deps = NULL;
    subject->static_dep_cnt = 0;
}

/* Is `dep` one of the Subjects `subject` declared? */
static bool static_deps_contain(const lv_subject_t * subject, const lv_subject_t * dep)
{
    for(uint32_t i = 0; i < subject->static_dep_cnt; i++) {
        if(subject->static_deps[i] == dep) return true;
    }
    return false;
}

void lv_subject_set_static_deps(lv_subject_t * subject, lv_subject_t * const * deps, uint32_t count)
{
    LV_CHECK_ARG(subject != NULL, return);
    LV_CHECK_ARG(deps != NULL || count == 0, return);

    static_deps_release(subject);
    if(deps == NULL || count == 0) return;

    subject->static_deps = deps;
    subject->static_dep_cnt = count;

    /* Each named Subject now has a reason to stay alive. */
    for(uint32_t i = 0; i < count; i++) {
        if(deps[i] != NULL) deps[i]->static_ref_cnt++;
    }
}

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

void lv_subject_transaction_begin(void)
{
    txn_enter();
}

lv_result_t lv_subject_transaction_commit(void)
{
    lv_global_t * global = LV_GLOBAL_DEFAULT();

    if(global->subject_txn_depth == 0) {
        /* A refusal already closed it and put every write back, so this is the normal
         * end of a transaction that failed, not a misuse. */
        if(global->subject_txn_aborted) return LV_RESULT_INVALID;

        LV_LOG_WARN("lv_subject_transaction_commit() with no transaction open");
        return LV_RESULT_INVALID;
    }
    txn_leave();
    return LV_RESULT_OK;
}

uint32_t lv_subject_get_changed_at(lv_subject_t * subject)
{
    LV_CHECK_ARG(subject != NULL, return 0);

    /* Reading a stamp is a read like any other: it wires a dependency, and it evaluates
     * a dirty Subject first. A lazily computed Subject's stamp is stale until its mapper
     * has run, so comparing without evaluating would give the wrong answer. */
    lv_subject_track_dependency(subject);
    subject_pull(subject);
    return subject->changed_at;
}

uint32_t lv_subject_get_transaction_id(void)
{
    return LV_GLOBAL_DEFAULT()->subject_txn_id;
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
    subject->type = (uint32_t)type;

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

lv_result_t lv_subject_delete(lv_subject_t * subject)
{
    if(!subject) {
        return LV_RESULT_INVALID;
    }
    if(!subject->in_list) {
        LV_LOG_WARN("Use lv_subject_deinit() for a Subject set up with lv_subject_init_...()");
        return LV_RESULT_INVALID;
    }
    /* The declared edges first. They cover every branch, so they catch the Subject that
     * happens to be unread right now but would be read after a condition flips. */
    if(subject->static_ref_cnt > 0) {
        LV_LOG_WARN("Subject is named in the static dependencies of %" LV_PRIu32 " other "
                    "Subject(s), so deleting it could leave a mapper or setter reaching freed "
                    "memory. Delete those first, or use lv_subject_delete_cascade().",
                    subject->static_ref_cnt);
        return LV_RESULT_INVALID;
    }

    uint32_t dependents = ref_list_count(&subject->dependents);
    if(dependents > 0) {
        LV_LOG_WARN("Subject is a dependency of %" LV_PRIu32 " other Subject(s). Delete those "
                    "first, or use lv_subject_delete_cascade().", dependents);
        return LV_RESULT_INVALID;
    }

    subject_destroy(subject);
    return LV_RESULT_OK;
}

void lv_subject_set_delete_cb(lv_subject_t * subject, lv_subject_delete_cb_t cb, void * user_data)
{
    LV_CHECK_ARG(subject != NULL, return);

    subject->delete_cb = cb;
    subject->delete_user_data = user_data;
}

void lv_subject_set_mapper_user_data_owned(lv_subject_t * subject)
{
    LV_CHECK_ARG(subject != NULL, return);

    if(!subject->has_mapper) {
        LV_LOG_WARN("Set the mapper and its user data first, then hand over ownership");
        return;
    }

    subject->owns_mapper_user_data = 1;
}

void * lv_subject_get_mapper_user_data(const lv_subject_t * subject)
{
    LV_CHECK_ARG(subject != NULL, return NULL);

    return subject->mapper_user_data;
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

void lv_subject_set_rounding(lv_subject_t * subject, lv_subject_rounding_t rounding)
{
    LV_CHECK_ARG(subject != NULL, return);

    subject->rounding = rounding;
}

#if LV_USE_FLOAT
/* Is the float a whole number, and which one is it nearest?
 *
 * The tolerance is scaled by magnitude rather than fixed: a float near 1e6 cannot hold a
 * fraction finer than about 0.06, so a fixed one would call a value broken that is as
 * whole as a float can be. The constant is FLT_EPSILON, spelled out rather than pulling
 * in <float.h>. */
static bool float_is_whole(float value, int32_t * nearest)
{
    int32_t n = lv_subject_float_to_int(value);
    if(nearest != NULL) *nearest = n;

    float mag = value < 0.0f ? -value : value;
    float tol = 4.0f * 1.19209290e-7f * mag;
    if(tol < 1e-6f) tol = 1e-6f;

    float diff = value - (float)n;
    if(diff < 0.0f) diff = -diff;
    return diff <= tol;
}

/* A float turned into an int, the way this Subject asks for. The only place that decides,
 * so a read and a write cannot disagree. `lossless` reports whether the fraction was
 * there to lose; passing NULL asks for the warning instead. */
static int32_t float_as_int(const lv_subject_t * subject, float value, bool * lossless)
{
    int32_t nearest;
    bool whole = float_is_whole(value, &nearest);
    if(lossless != NULL) *lossless = whole;

    if(!whole && lossless == NULL && subject->rounding == LV_SUBJECT_ROUND_EXACT) {
        LV_LOG_WARN("This Subject is set to LV_SUBJECT_ROUND_EXACT but its value is not "
                    "a whole number. Rounded to %" LV_PRId32 ". Use "
                    "lv_subject_get_int_checked() to be told instead of warned, or "
                    "choose LV_SUBJECT_ROUND_NEAREST or LV_SUBJECT_ROUND_TOWARD_ZERO.",
                    nearest);
    }

    if(subject->rounding == LV_SUBJECT_ROUND_TOWARD_ZERO) return (int32_t)value;
    return nearest;
}

/* An int turned into a float. Exact until the mantissa runs out. */
static bool int_fits_float(int32_t value)
{
    return value >= -LV_SUBJECT_INT_EXACT_IN_FLOAT && value <= LV_SUBJECT_INT_EXACT_IN_FLOAT;
}
#endif

void lv_subject_set_int(lv_subject_t * subject, int32_t value)
{
    LV_CHECK_ARG(subject != NULL, return);
    LV_CHECK_ARG(subject->type == LV_SUBJECT_TYPE_INT || subject->type == LV_SUBJECT_TYPE_FLOAT,
                 return);

#if LV_USE_FLOAT
    if(subject->type == LV_SUBJECT_TYPE_FLOAT) {
        /* Exact up to 2^24, and above it a float starts dropping low bits. Only
         * LV_SUBJECT_ROUND_EXACT treats that as a reason not to store the value. */
        if(subject->rounding == LV_SUBJECT_ROUND_EXACT && !int_fits_float(value)) {
            LV_LOG_WARN("%" LV_PRId32 " is too large for a float to hold exactly, and this "
                        "Subject is set to LV_SUBJECT_ROUND_EXACT, so the write is refused "
                        "and the previous value is kept.", value);
            return;
        }
        lv_subject_set_float(subject, (float)value);
        return;
    }
#endif

    if(!write_allowed()) return;

    if(subject->has_mapper) {
        lv_subject_value_t v = { .num = value };
        mapped_write(subject, v);
        return;
    }


    lv_subject_value_t v = { .num = value };
    subject_write(subject, v, NULL, false, NULL, false);
}

/*---------------------------------------------------------------
 * Peeking
 *
 * The same value, without the read counting as a dependency. A mapper that peeks decides
 * for itself what it depends on, with `lv_subject_track_dependency()`, instead of
 * depending on whatever its control flow happened to touch.
 *
 * The value is still brought up to date first: peeking a stale Subject would otherwise
 * answer with the previous evaluation's result.
 *--------------------------------------------------------------*/

int32_t lv_subject_peek_int(lv_subject_t * subject)
{
    LV_CHECK_ARG(subject != NULL, return 0);
    LV_CHECK_ARG(subject->type == LV_SUBJECT_TYPE_INT || subject->type == LV_SUBJECT_TYPE_FLOAT,
                 return 0);

    subject_pull(subject);
#if LV_USE_FLOAT
    if(subject->type == LV_SUBJECT_TYPE_FLOAT) return float_as_int(subject, subject->value.float_v, NULL);
#endif
    return subject->value.num;
}

#if LV_USE_FLOAT
float lv_subject_peek_float(lv_subject_t * subject)
{
    LV_CHECK_ARG(subject != NULL, return 0.0f);
    LV_CHECK_ARG(subject->type == LV_SUBJECT_TYPE_FLOAT || subject->type == LV_SUBJECT_TYPE_INT,
                 return 0.0f);

    subject_pull(subject);
    if(subject->type == LV_SUBJECT_TYPE_INT) return (float)subject->value.num;
    return subject->value.float_v;
}
#endif

lv_color_t lv_subject_peek_color(lv_subject_t * subject)
{
    lv_color_t black = lv_color_black();
    LV_CHECK_ARG(subject != NULL, return black);
    LV_CHECK_ARG(subject->type == LV_SUBJECT_TYPE_COLOR, return black);

    subject_pull(subject);
    return subject->value.color;
}

const char * lv_subject_peek_string(lv_subject_t * subject)
{
    LV_CHECK_ARG(subject != NULL, return NULL);
    LV_CHECK_ARG(subject->type == LV_SUBJECT_TYPE_STRING, return NULL);

    subject_pull(subject);
    return subject->value.pointer;
}

const void * lv_subject_peek_pointer(lv_subject_t * subject)
{
    LV_CHECK_ARG(subject != NULL, return NULL);
    LV_CHECK_ARG(subject->type == LV_SUBJECT_TYPE_POINTER, return NULL);

    subject_pull(subject);
    return subject->value.pointer;
}

int32_t lv_subject_get_int(lv_subject_t * subject)
{
    LV_CHECK_ARG(subject != NULL, return 0);
    LV_CHECK_ARG(subject->type == LV_SUBJECT_TYPE_INT || subject->type == LV_SUBJECT_TYPE_FLOAT,
                 return 0);

    /* The dependency is on the Subject, whichever type it was read as. */
    lv_subject_track_dependency(subject);
    subject_pull(subject);
#if LV_USE_FLOAT
    if(subject->type == LV_SUBJECT_TYPE_FLOAT) return float_as_int(subject, subject->value.float_v, NULL);
#endif
    return subject->value.num;
}




lv_result_t lv_subject_get_int_checked(lv_subject_t * subject, int32_t * value)
{
    LV_CHECK_ARG(subject != NULL, return LV_RESULT_INVALID);
    LV_CHECK_ARG(value != NULL, return LV_RESULT_INVALID);
    LV_CHECK_ARG(subject->type == LV_SUBJECT_TYPE_INT || subject->type == LV_SUBJECT_TYPE_FLOAT,
                 return LV_RESULT_INVALID);

    lv_subject_track_dependency(subject);
    subject_pull(subject);

#if LV_USE_FLOAT
    if(subject->type == LV_SUBJECT_TYPE_FLOAT) {
        bool lossless;
        *value = float_as_int(subject, subject->value.float_v, &lossless);
        return lossless ? LV_RESULT_OK : LV_RESULT_INVALID;
    }
#endif
    *value = subject->value.num;
    return LV_RESULT_OK;
}

#if LV_USE_FLOAT

lv_result_t lv_subject_get_float_checked(lv_subject_t * subject, float * value)
{
    LV_CHECK_ARG(subject != NULL, return LV_RESULT_INVALID);
    LV_CHECK_ARG(value != NULL, return LV_RESULT_INVALID);
    LV_CHECK_ARG(subject->type == LV_SUBJECT_TYPE_FLOAT || subject->type == LV_SUBJECT_TYPE_INT,
                 return LV_RESULT_INVALID);

    lv_subject_track_dependency(subject);
    subject_pull(subject);

    if(subject->type == LV_SUBJECT_TYPE_INT) {
        *value = (float)subject->value.num;
        return int_fits_float(subject->value.num) ? LV_RESULT_OK : LV_RESULT_INVALID;
    }
    *value = subject->value.float_v;
    return LV_RESULT_OK;
}

void lv_subject_init_float(lv_subject_t * subject, float value)
{
    LV_CHECK_ARG(subject != NULL, return);

    init_common(subject);
    init_float(subject, value);
}

void lv_subject_set_float(lv_subject_t * subject, float value)
{
    LV_CHECK_ARG(subject != NULL, return);
    LV_CHECK_ARG(subject->type == LV_SUBJECT_TYPE_FLOAT || subject->type == LV_SUBJECT_TYPE_INT,
                 return);

    /* An int Subject keeps only the whole part, by its own rounding rule. */
    if(subject->type == LV_SUBJECT_TYPE_INT) {
        bool lossless;
        int32_t as_int = float_as_int(subject, value, &lossless);
        if(!lossless && subject->rounding == LV_SUBJECT_ROUND_EXACT) {
            LV_LOG_WARN("This Subject is set to LV_SUBJECT_ROUND_EXACT and the value "
                        "written is not a whole number, so the write is refused and the "
                        "previous value is kept. It would have stored %" LV_PRId32 ".",
                        as_int);
            return;
        }
        lv_subject_set_int(subject, as_int);
        return;
    }

    if(!write_allowed()) return;

    if(subject->has_mapper) {
        lv_subject_value_t v = { .float_v = value };
        mapped_write(subject, v);
        return;
    }


    lv_subject_value_t written = { .float_v = value };
    subject_write(subject, written, NULL, false, NULL, false);
}

float lv_subject_get_float(lv_subject_t * subject)
{
    LV_CHECK_ARG(subject != NULL, return 0.0);
    LV_CHECK_ARG(subject->type == LV_SUBJECT_TYPE_FLOAT || subject->type == LV_SUBJECT_TYPE_INT,
                 return 0.0);

    /* The dependency is on the Subject, whichever type it was read as. */
    lv_subject_track_dependency(subject);
    subject_pull(subject);
    /* Exact, so there is nothing for the rounding rule to decide. */
    if(subject->type == LV_SUBJECT_TYPE_INT) return (float)subject->value.num;
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
    LV_CHECK_ARG(subject->type == LV_SUBJECT_TYPE_POINTER, return false);
    LV_CHECK_ARG(data != NULL || size == 0, return false);
    if(!write_allowed()) return false;

    bool handled_ok = false;
    if(copy_write_handled(subject, data, &handled_ok)) return handled_ok;

    if(!buf_reserve(subject, size, true)) return false;

    lv_subject_buf_t * b = subject->buf;

    /* The old bytes are still here, so proper change detection comes for free. */
    subject->copy_changed = (b->length != size) ||
                            (size > 0 && lv_memcmp(b->buf, data, size) != 0) ? 1U : 0U;

    if(size > 0) lv_memcpy(b->buf, data, size);
    b->length = size;

    subject->value.pointer = b->buf;
    lv_subject_value_t v = { .pointer = b->buf };
    subject_write(subject, v, NULL, false, NULL, false);
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
    LV_CHECK_ARG(subject->type == LV_SUBJECT_TYPE_STRING, return false);
    LV_CHECK_ARG(buf != NULL, return false);
    if(!write_allowed()) return false;

    bool handled_ok = false;
    if(copy_write_handled(subject, buf, &handled_ok)) return handled_ok;

    size_t needed = lv_strlen(buf) + 1;
    if(!buf_reserve(subject, needed, true)) return false;

    lv_subject_buf_t * b = subject->buf;
    subject->copy_changed = lv_strcmp((const char *)b->buf, buf) != 0 ? 1U : 0U;

    /* The new string is handed to the mapper as `input`; the buffer still holds the
     * previous value, so a mapper can compare the two. */
    lv_subject_value_t v = { .pointer = buf };
    subject_write(subject, v, buf, false, NULL, false);
    return true;
}

bool lv_subject_copy_string_trimmed(lv_subject_t * subject, const char * buf)
{
    LV_CHECK_ARG(subject != NULL, return false);
    LV_CHECK_ARG(subject->type == LV_SUBJECT_TYPE_STRING, return false);
    LV_CHECK_ARG(buf != NULL, return false);
    if(!write_allowed()) return false;

    bool handled_ok = false;
    if(copy_write_handled(subject, buf, &handled_ok)) return handled_ok;

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
    lv_subject_value_t v = { .pointer = buf };
    subject_write(subject, v, buf, false, NULL, false);
    return true;
}

bool lv_subject_snprintf(lv_subject_t * subject, const char * format, ...)
{
    LV_CHECK_ARG(subject != NULL, return false);
    LV_CHECK_ARG(subject->type == LV_SUBJECT_TYPE_STRING, return false);
    LV_CHECK_ARG(format != NULL, return false);
    if(!write_allowed()) return false;

    if(subject->has_mapper) {
        LV_LOG_WARN("lv_subject_snprintf() formats into the Subject's own buffer, which a "
                    "mapper owns. Format into a local and pass that to the setter instead.");
        return false;
    }

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

    lv_subject_value_t v = { .pointer = tmp };
    subject_write(subject, v, tmp, false, NULL, false);
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

    if(subject->has_mapper) {
        lv_subject_value_t v = { .pointer = ptr };
        mapped_write(subject, v);
        return;
    }

    if(owned) subject->value_free_cb = free_cb;

    lv_subject_value_t v = { .pointer = ptr };
    subject_write(subject, v, NULL, owned, NULL, false);
}

void lv_subject_set_pointer(lv_subject_t * subject, void * ptr)
{
    LV_CHECK_ARG(subject != NULL, return);
    LV_CHECK_ARG(subject->type == LV_SUBJECT_TYPE_POINTER, return);

    set_pointer_value(subject, ptr, false, NULL);
}

void lv_subject_set_pointer_owned(lv_subject_t * subject, void * ptr, lv_subject_value_free_cb_t free_cb)
{
    LV_CHECK_ARG(subject != NULL, return);
    LV_CHECK_ARG(subject->type == LV_SUBJECT_TYPE_POINTER, return);

    set_pointer_value(subject, ptr, true, free_cb);
}

void lv_subject_set_string(lv_subject_t * subject, const char * str)
{
    LV_CHECK_ARG(subject != NULL, return);
    LV_CHECK_ARG(subject->type == LV_SUBJECT_TYPE_STRING, return);
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
    LV_CHECK_ARG(subject->type == LV_SUBJECT_TYPE_STRING, return);
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
    LV_CHECK_ARG(subject->type == LV_SUBJECT_TYPE_COLOR, return);

    if(!write_allowed()) return;

    if(subject->has_mapper) {
        lv_subject_value_t v = { .color = color };
        mapped_write(subject, v);
        return;
    }


    lv_subject_value_t written = { .color = color };
    subject_write(subject, written, NULL, false, NULL, false);
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
    observer->obj = obj;
    subject->immediate_observer_cnt++;  /* LV_OBSERVER_MODE_IMMEDIATE is the default */
    /* subscribe to delete event of the object */
    if(obj != NULL) {
        lv_obj_add_event_cb(obj, unsubscribe_on_delete_cb, LV_EVENT_DELETE, observer);
    }

    /* Update Observer immediately. */
    observer->cb(observer, subject);

    return observer;
}



void lv_observer_delete(lv_observer_t * observer)
{
    if(observer == NULL) return;

    if(observer->obj) {
        lv_obj_remove_event_cb_with_user_data(observer->obj, unsubscribe_on_delete_cb, observer);
        lv_obj_remove_event_cb_with_user_data(observer->obj, NULL, observer->subject);
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
    LV_CHECK_ARG(true, return NULL);

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
    LV_CHECK_ARG(subject->type == LV_SUBJECT_TYPE_INT, return);

    lv_obj_add_event_cb(obj, subject_toggle_cb, trigger, subject);
}

void lv_obj_add_subject_set_int_event(lv_obj_t * obj, lv_subject_t * subject, lv_event_code_t trigger, int32_t value)
{
    LV_CHECK_ARG(obj != NULL, return);
    LV_CHECK_ARG(subject != NULL, return);
    LV_CHECK_ARG(subject->type == LV_SUBJECT_TYPE_INT, return);

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
    LV_CHECK_ARG(subject->type == LV_SUBJECT_TYPE_FLOAT, return);

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
    LV_CHECK_ARG(subject->type == LV_SUBJECT_TYPE_STRING, return);
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

    return observer->obj;
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
    if(!subject->in_list) {
        /* An application-owned Subject is not in the global list, so no drain can ever
         * find it. It is notified here instead. Such a Subject sits outside the graph
         * bookkeeping altogether, which is why `lv_subject_init_...()` is deprecated. */
        notify(subject);
        return;
    }

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

    /* Re-entrant on purpose. An Observer may write a Subject, and that write is its own
     * transaction: it has to evaluate and notify before the Observer's next statement,
     * or two writes in a row would look like one. The cap is there because a chain of
     * Observers writing each other has nothing else to stop it. */
    if(global->subject_flushing >= LV_SUBJECT_MAX_NOTIFY_DEPTH) {
        LV_LOG_WARN("Observers are writing Subjects more than %d deep. The chain is cut "
                    "here. An Observer that writes is starting a new transaction, so a "
                    "cycle between two of them never settles.",
                    (int)LV_SUBJECT_MAX_NOTIFY_DEPTH);
        return;
    }
    global->subject_flushing++;

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

    global->subject_flushing--;
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

/*---------------------------------------------------------------
 * Transactions
 *
 * A transaction defers *notification*, not evaluation. Writes take effect as they are
 * made, so a read inside a transaction is always honest and a chain of setters sees what
 * the previous one wrote. The Observers hear one settled story at the commit.
 *
 * Every public write wraps itself in one of these, so a bare write is a transaction of
 * its own and there is a single code path. Notification never happens inside a
 * transaction: the commit at depth 0 evaluates what is due and then notifies, so an
 * Observer always sees a settled graph, and a write an Observer makes is a new
 * transaction rather than a nested one.
 *--------------------------------------------------------------*/

static void txn_enter(void)
{
    lv_global_t * global = LV_GLOBAL_DEFAULT();
    /* A fresh top-level transaction gets a new id. Every Subject it changes stamps that
     * id, so Subjects changed together compare equal. */
    if(global->subject_txn_depth == 0) {
        global->subject_txn_id++;
        /* The flag describes the transaction now starting, not the one before it. */
        global->subject_txn_aborted = 0;
    }
    global->subject_txn_depth++;
}

static void txn_leave(void)
{
    lv_global_t * global = LV_GLOBAL_DEFAULT();
    if(global->subject_txn_depth == 0) return;

    global->subject_txn_depth--;
    if(global->subject_txn_depth > 0) return;   /* an inner commit notifies nothing */

    /* Committed, so the values the transaction replaced are finally unreachable and the
     * releases it held back can happen. */
    txn_write_t * w;
    LV_LL_READ(&txn_writes, w) {
        lv_subject_t * subject = w->subject;
        if(w->old_owned && (void *)subject->value.pointer != (void *)w->old_value.pointer) {
            void * old = (void *)w->old_value.pointer;
            if(old != NULL) {
                if(subject->value_free_cb) subject->value_free_cb(old);
                else lv_free(old);
            }
        }
        if(w->snapshot != NULL) lv_free(w->snapshot);
    }
    lv_ll_clear(&txn_writes);

    /* The graph has settled. Evaluate what is due and deliver the notifications that
     * were held back. */
    process_dirty(false);
}

static txn_write_t * txn_find(const lv_subject_t * subject)
{
    txn_write_t * w;
    LV_LL_READ(&txn_writes, w) {
        if(w->subject == subject) return w;
    }
    return NULL;
}

/* Remember what a Subject held before this transaction first changed it.
 *
 * Called just before every change, so the entry is the state a rollback goes back to.
 * Only the first change per Subject is recorded: later ones are already covered. */
static void txn_record_change(lv_subject_t * subject)
{
    lv_global_t * global = LV_GLOBAL_DEFAULT();
    if(global->subject_txn_depth == 0) return;   /* not inside anything to undo */
    if(global->subject_txn_aborting) return;     /* the undo must not record itself */
    if(txn_find(subject) != NULL) return;

    txn_write_t * w = lv_ll_ins_tail(&txn_writes);
    if(w == NULL) return;   /* out of memory: the rollback will be incomplete, not wrong */

    w->subject = subject;
    w->old_value = subject->value;
    w->old_version = subject->version;
    w->old_changed_at = subject->changed_at;
    w->direct = 0;
    w->direct_value = subject->value;
    w->old_owned = subject->owns_value;
    w->old_pending = subject->pending_notify;
    w->snapshot = NULL;
    w->snapshot_len = 0;
    w->restorable = 1;

    /* A copying Subject writes over its own buffer, so the only way back is a copy of
     * the bytes. Everything else is either a value or a pointer, and both are already in
     * `old_value`.
     *
     * Releasing an owned pointer is what a rollback genuinely cannot undo, so a write
     * inside a transaction does not release: `old_value` keeps it alive until the commit
     * decides which of the two to free. */
    if(subject->buf != NULL && subject->buf->buf != NULL) {
        size_t len = subject->buf->length;
        if(subject->type == LV_SUBJECT_TYPE_STRING) len++;   /* the terminator too */
        if(len > 0) {
            w->snapshot = lv_malloc(len);
            if(w->snapshot != NULL) {
                lv_memcpy(w->snapshot, subject->buf->buf, len);
                w->snapshot_len = len;
            }
        }
        /* The buffer *is* the value, so there is no separate pointer to put back. */
        w->restorable = 0;
    }
}

/* Has a chain of setters come back around to a Subject it already wrote, with a
 * different answer?
 *
 * Writing the same value again is how a chain settles, and change detection stops it on
 * its own. A *different* value means the inverses disagree and the propagation would not
 * terminate, so the transaction fails instead.
 *
 * Only checked while a setter is running. Outside one, writing the same Subject twice
 * with different values is ordinary application code, not a cycle. */
static bool txn_setter_write_ok(lv_subject_t * subject, lv_subject_value_t v)
{
    txn_write_t * w = txn_find(subject);
    if(w == NULL) return true;

    if(!w->direct) {
        w->direct = 1;
        w->direct_value = v;
        return true;
    }

    bool same;
    switch(subject->type) {
        case LV_SUBJECT_TYPE_INT:
            same = w->direct_value.num == v.num;
            break;
#if LV_USE_FLOAT
        case LV_SUBJECT_TYPE_FLOAT:
            same = w->direct_value.float_v == v.float_v;
            break;
#endif
        case LV_SUBJECT_TYPE_COLOR:
            same = lv_color_to_u32(w->direct_value.color) == lv_color_to_u32(v.color);
            break;
        default:
            same = true;
            break;
    }
    if(same) return true;

    LV_LOG_WARN("A chain of setters wrote the same Subject twice with different values. "
                "The inverses disagree, so the transaction is rolled back.");
    return false;
}

/* Put back everything the transaction changed. */
static void txn_abort(void)
{
    lv_global_t * global = LV_GLOBAL_DEFAULT();
    global->subject_txn_aborting = 1;

    /* Newest first, so a Subject changed more than once ends at its oldest state. */
    txn_write_t * w = lv_ll_get_tail(&txn_writes);
    while(w != NULL) {
        lv_subject_t * subject = w->subject;
        void * current = (void *)subject->value.pointer;

        /* The transaction is being undone, so anything it took ownership of goes with
         * it: the caller already handed it over and nothing will refer to it again. */
        if(subject->owns_value && current != NULL && current != (void *)w->old_value.pointer) {
            if(subject->value_free_cb) subject->value_free_cb(current);
            else lv_free(current);
        }

        if(w->restorable) subject->value = w->old_value;
        subject->owns_value = w->old_owned;
        subject->version = w->old_version;
        subject->changed_at = w->old_changed_at;

        /* A copying Subject's bytes are the value, so they are put back from the copy
         * taken before the first write. */
        if(w->snapshot != NULL && subject->buf != NULL && subject->buf->buf != NULL) {
            if(w->snapshot_len <= subject->buf->capacity) {
                lv_memcpy(subject->buf->buf, w->snapshot, w->snapshot_len);
                subject->buf->length = subject->type == LV_SUBJECT_TYPE_STRING
                                       ? w->snapshot_len - 1 : w->snapshot_len;
            }
            lv_free(w->snapshot);
            w->snapshot = NULL;
        }

        /* The notification the write queued goes back with the value. Nothing changed
         * after all, so an Observer must not be told that something did — that is what
         * made a rolled back transaction still report `x -> x` on the next flush. A
         * notification that was already owed before the transaction is left owed.
         *
         * Set here rather than through clear_pending(), which also clears `dirty` and
         * would undo the marking just below. */
        subject->pending_notify = w->old_pending;

        /* A computed Subject's value is reproducible, so it is simply marked stale and
         * recomputed on the next read rather than being restored. */
        if(subject->has_mapper) mark_dirty(subject);
        else list_reposition(subject);

        w = lv_ll_get_prev(&txn_writes, w);
    }

    lv_ll_clear(&txn_writes);
    /* Clearing the last pending notification can leave the flush timer with nothing to
     * do, and it only stops when something asks. */
    flush_timer_update();
    global->subject_txn_depth = 0;
    global->subject_txn_aborting = 0;
    /* Nothing is left to commit, and the caller has no other way to learn that. */
    global->subject_txn_aborted = 1;
}

/* Hand a write to the Subject's setter, which turns it into writes further up. */
static bool run_setter(lv_subject_t * subject, lv_subject_value_t value)
{
    if(!txn_setter_write_ok(subject, value)) return false;

    bool ok;
    switch(subject->type) {
        case LV_SUBJECT_TYPE_INT:
            ok = subject->setter.int_cb(subject, subject->setter_user_data, value.num);
            break;
#if LV_USE_FLOAT
        case LV_SUBJECT_TYPE_FLOAT:
            ok = subject->setter.float_cb(subject, subject->setter_user_data, value.float_v);
            break;
#endif
        case LV_SUBJECT_TYPE_COLOR:
            ok = subject->setter.color_cb(subject, subject->setter_user_data, value.color);
            break;
        case LV_SUBJECT_TYPE_POINTER:
            ok = subject->setter.pointer_cb(subject, subject->setter_user_data,
                                            (void *)value.pointer);
            break;
        case LV_SUBJECT_TYPE_STRING:
            ok = subject->setter.string_cb(subject, subject->setter_user_data,
                                           (const char *)value.pointer);
            break;
        default:
            ok = false;
            break;
    }
    return ok;
}

/* A copying write to a Subject whose mapper owns its value.
 *
 * Returns true when the caller should stop — the write has been handed to the Subject's
 * setter, or refused for want of one — and false when it should carry on and copy. */
static bool copy_write_handled(lv_subject_t * subject, const void * data, bool * ok)
{
    if(!subject->has_mapper) return false;

    lv_subject_value_t v = { .pointer = data };
    *ok = mapped_write(subject, v);
    return true;
}

/* The write path of a Subject that has a mapper.
 *
 * Its value belongs to the mapper, so the write only means something if the Subject has
 * a setter to turn it into writes on the Subjects the mapper reads. */
static bool mapped_write(lv_subject_t * subject, lv_subject_value_t value)
{
    if(!subject->has_setter) {
        LV_LOG_WARN("This Subject's value is computed by its mapper, so it cannot be written. "
                    "Give it a setter with lv_subject_set_..._setter() to make it writable, or "
                    "write the Subjects its mapper reads.");
        return false;
    }

    txn_enter();
    if(!run_setter(subject, value)) {
        txn_abort();
        return false;
    }
    txn_leave();
    return true;
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

    /* A first allocation hands back uninitialised bytes. Terminate them like
     * lv_subject_set_buffer() does, so change detection can read the buffer before
     * anything has been stored in it. */
    if(b->capacity == 0 && subject->type == LV_SUBJECT_TYPE_STRING) {
        ((char *)grown)[0] = '\0';
    }

    b->buf = grown;
    b->capacity = needed;
    /* The value is the buffer, so it moves with it. */
    subject->value.pointer = grown;
    return true;
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
static bool store_written(lv_subject_t * subject, lv_subject_value_t v, const void * str)
{
    switch(subject->type) {
        case LV_SUBJECT_TYPE_INT:
            if(subject->value.num == v.num) return false;
            subject->value.num = v.num;
            return true;
#if LV_USE_FLOAT
        case LV_SUBJECT_TYPE_FLOAT:
            if(subject->value.float_v == v.float_v) return false;
            subject->value.float_v = v.float_v;
            return true;
#endif
        case LV_SUBJECT_TYPE_COLOR:
            if(lv_color_to_u32(subject->value.color) == lv_color_to_u32(v.color)) return false;
            subject->value.color = v.color;
            return true;
        case LV_SUBJECT_TYPE_POINTER:
            if(subject->buf != NULL) {
                /* Copying: the bytes are already in the buffer, and the comparison
                 * against the previous ones was done while they were still there. */
                return subject->copy_changed;
            }
            /* Referring: documented to notify whether or not the pointer itself changed,
             * because the data behind an unchanged pointer may have changed. */
            subject->value.pointer = v.pointer;
            return true;
        case LV_SUBJECT_TYPE_STRING:
            if(subject->buf == NULL) {
                /* No buffer, so this Subject stores the pointer rather than copying. */
                subject->value.pointer = v.pointer;
                return true;
            }
            if(str == NULL) return false;
            if(!subject->copy_changed) return false;
            lv_strlcpy((char *)subject->buf->buf, str, buf_capacity(subject));
            subject->buf->length = lv_strlen((const char *)subject->buf->buf);
            return true;
        default:
            /* LV_SUBJECT_TYPE_NONE has no value; it only ever aggregates. */
            return true;
    }
}

/* Run the mapper with dependency tracking on. Returns whether it reported a change. */
static bool run_mapper(lv_subject_t * subject)
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

    bool changed = false;
    switch(subject->type) {
        case LV_SUBJECT_TYPE_INT: {
                int32_t value = subject->value.num;
                changed = subject->mapper.int_cb(subject, ud, &value);
                subject->value.num = value;
                break;
            }
#if LV_USE_FLOAT
        case LV_SUBJECT_TYPE_FLOAT: {
                float value = subject->value.float_v;
                changed = subject->mapper.float_cb(subject, ud, &value);
                subject->value.float_v = value;
                break;
            }
#endif
        case LV_SUBJECT_TYPE_POINTER: {
                const void * value = subject->value.pointer;
                changed = subject->mapper.pointer_cb(subject, ud, &value);
                subject->value.pointer = value;
                break;
            }
        case LV_SUBJECT_TYPE_COLOR: {
                lv_color_t value = subject->value.color;
                changed = subject->mapper.color_cb(subject, ud, &value);
                subject->value.color = value;
                break;
            }
        case LV_SUBJECT_TYPE_STRING:
            changed = subject->mapper.string_cb(subject, ud, (char *)subject->value.pointer,
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

/* Everything that happens once a Subject's value has been decided, whether it was
 * written or computed. */
static void publish(lv_subject_t * subject, bool changed)
{
    subject->dirty = 0;
    if(!changed) {
        list_reposition(subject);
        return;
    }

    /* A real change, so anything that read the old value is out of date. */
    subject->version++;
    subject->changed_at = LV_GLOBAL_DEFAULT()->subject_txn_id;

    /* Phase 1: everything downstream is now stale. Always synchronous, so a read of a
     * dependent can never return a stale value. */
    mark_dependents_dirty(subject);

    /* Phase 2: the Observers wait. Notification happens once the transaction has ended,
     * in `txn_leave()`, never from inside it — so an Observer always sees a settled graph,
     * and a write it makes is a transaction of its own rather than a nested one. */
    mark_pending_notify(subject);
}

/* Re-evaluate a Subject whose mapper owns its value. */
static void subject_evaluate(lv_subject_t * subject)
{
    if(!subject->has_mapper) {
        /* Nothing to re-derive from. A plain Subject can still be marked dirty, e.g. by
         * a rolled-back transaction, and simply has nothing to do about it. */
        subject->dirty = 0;
        list_reposition(subject);
        return;
    }

    void * outgoing = (void *)subject->value.pointer;
    bool outgoing_owned = subject->owns_value;

    txn_record_change(subject);
    bool changed = run_mapper(subject);

    if(subject->type == LV_SUBJECT_TYPE_POINTER || subject->type == LV_SUBJECT_TYPE_STRING) {
        /* A mapper cannot take ownership of anything: what it produced is borrowed unless
         * it handed back the value the Subject already owned. */
        if(subject->value.pointer != outgoing) subject->owns_value = 0;
        release_if_unreferenced(subject, outgoing, outgoing_owned);
    }

    publish(subject, changed);
}

/* The path every write to a plain Subject goes through.
 *
 * A Subject with a mapper never gets here: its value belongs to the mapper, so a write
 * is either refused or routed through its setter.
 *
 * `owned` says whether the Subject takes over the pointer being written, and
 * `superseded` is a pointer an earlier write left behind. Both mean nothing for the
 * value types. */
static void subject_write(lv_subject_t * subject, lv_subject_value_t v, const void * str,
                          bool owned, void * superseded, bool superseded_owned)
{
    txn_enter();

    /* Remember what is about to be replaced, so it can be released once nothing refers
     * to it any more. */
    void * outgoing = (void *)subject->value.pointer;
    bool outgoing_owned = subject->owns_value;

    txn_record_change(subject);
    bool changed = store_written(subject, v, str);

    if(subject->type == LV_SUBJECT_TYPE_POINTER || subject->type == LV_SUBJECT_TYPE_STRING) {
        /* A copying Subject never owns its value separately: the buffer is the value. */
        if(subject->buf != NULL) subject->owns_value = 0;
        else subject->owns_value = (subject->value.pointer == v.pointer && owned) ? 1U : 0U;

        /* Freeing cannot be undone, so while a transaction is open the old value stays
         * alive in the write set and the commit releases it. */
        if(txn_find(subject) == NULL) {
            release_if_unreferenced(subject, outgoing, outgoing_owned);
        }
        if(superseded != outgoing) release_if_unreferenced(subject, superseded, superseded_owned);
    }

    publish(subject, changed);
    txn_leave();
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
 * a write, which never reaches a Subject that has a mapper. */
static bool deps_are_unchanged(lv_subject_t * subject)
{
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

    subject_evaluate(subject);
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
 * `lv_subject_create_clamped()` mirrors a source bounded to a range.
 *
 * It keeps its configuration in the mapper's user data, which the Subject owns and
 * frees. That is what lets one mapper serve every instance.
 *
 * There used to be `lv_subject_create_min()` / `_max()` here, recording the extremum a
 * source had passed through. They were the library's only accumulating mappers, and an
 * accumulator's answer depends on how often it ran rather than on its inputs. Write one
 * as an eager Subject instead: see @ref lv_subject_set_mode.
 *--------------------------------------------------------------*/

typedef struct {
    lv_subject_t * source;
    lv_subject_compare_cb_t compare_cb;  /**< NULL means natural ordering */
    lv_subject_value_t min_value;
    lv_subject_value_t max_value;
    bool seeded;                         /**< Has a first value been stored? */
    bool is_clamp;                       /**< Marks the user data as a clamp's, for set_range */
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



#if LV_USE_FLOAT
#endif



/* Bound the source's value, ordering with the compare callback so the same code works
 * for a colour or a struct pointer as for an int. */
static bool clamp_step(lv_subject_t * subject, void * user_data, lv_subject_value_t * value)
{
    derived_dsc_t * dsc = user_data;

    /* The bounds live in the mapper's own state. `lv_subject_set_range()` changes them
     * and re-evaluates; the mapper only ever reads them. */
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

static bool clamp_int_mapper(lv_subject_t * subject, void * user_data, int32_t * value)
{
    lv_subject_value_t slot;
    lv_memzero(&slot, sizeof(slot));
    slot.num = *value;
    if(!clamp_step(subject, user_data, &slot)) return false;
    *value = slot.num;
    return true;
}

#if LV_USE_FLOAT
static bool clamp_float_mapper(lv_subject_t * subject, void * user_data, float * value)
{
    lv_subject_value_t slot;
    lv_memzero(&slot, sizeof(slot));
    slot.float_v = *value;
    if(!clamp_step(subject, user_data, &slot)) return false;
    *value = slot.float_v;
    return true;
}
#endif

static bool clamp_color_mapper(lv_subject_t * subject, void * user_data, lv_color_t * value)
{
    lv_subject_value_t slot;
    lv_memzero(&slot, sizeof(slot));
    slot.color = *value;
    if(!clamp_step(subject, user_data, &slot)) return false;
    *value = slot.color;
    return true;
}

static bool clamp_pointer_mapper(lv_subject_t * subject, void * user_data, const void ** value)
{
    lv_subject_value_t slot;
    lv_memzero(&slot, sizeof(slot));
    slot.pointer = *value;
    if(!clamp_step(subject, user_data, &slot)) return false;
    *value = slot.pointer;
    return true;
}

/* Create the derived Subject, attach the right typed mapper for the source's type, and
 * hand it ownership of `dsc`. */
static lv_subject_t * create_derived(lv_subject_t * source, derived_dsc_t * dsc)
{
    dsc->is_clamp = true;
    lv_subject_t * subject = lv_subject_create((lv_subject_type_t)source->type);
    if(subject == NULL) {
        lv_free(dsc);
        return NULL;
    }

    subject->owns_mapper_user_data = 1;

    /* A clamp is a pure mirror of its source, so it stays lazy: it is computed when
     * something reads it. */
    switch(source->type) {
        case LV_SUBJECT_TYPE_INT:
            lv_subject_set_int_mapper(subject, clamp_int_mapper, dsc);
            break;
#if LV_USE_FLOAT
        case LV_SUBJECT_TYPE_FLOAT:
            lv_subject_set_float_mapper(subject, clamp_float_mapper, dsc);
            break;
#endif
        case LV_SUBJECT_TYPE_COLOR:
            lv_subject_set_color_mapper(subject, clamp_color_mapper, dsc);
            break;
        case LV_SUBJECT_TYPE_STRING:
            /* A string Subject owns a buffer, so this would have to copy into it. Bound
             * a pointer Subject instead. */
            LV_LOG_WARN("lv_subject_create_clamped() does not support string subjects, "
                        "use a pointer subject");
            subject->owns_mapper_user_data = 0;
            lv_subject_delete(subject);
            lv_free(dsc);
            return NULL;
        default:
            lv_subject_set_pointer_mapper(subject, clamp_pointer_mapper, dsc);
            break;
    }

    return subject;
}

void lv_subject_set_range(lv_subject_t * subject, const lv_subject_range_t * range)
{
    LV_CHECK_ARG(subject != NULL, return);
    LV_CHECK_ARG(range != NULL, return);
    LV_CHECK_ARG(subject->has_mapper && subject->owns_mapper_user_data, return);

    derived_dsc_t * dsc = subject->mapper_user_data;
    LV_CHECK_ARG(dsc != NULL && dsc->is_clamp, return);

    /* Copied out, so the caller's struct may be transient. */
    dsc->min_value = range->min_value;
    dsc->max_value = range->max_value;

    /* The dependencies have not changed, so the version short-circuit would skip the
     * mapper. Commit directly, which always runs it. */
    txn_enter();
    subject_evaluate(subject);
    txn_leave();
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
    return create_derived(source, dsc);
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
    observer->obj = obj;
    observer->user_cb = set_cb;
    observer->mapper = mapper;
    observer->user_data = user_data;
    observer->style_selector = selector;
    observer->has_mapper = 1;
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
    subject->value.num = value;
}

#if LV_USE_FLOAT

static void init_float(lv_subject_t * subject, float value)
{
    LV_ASSERT(subject != NULL);
    subject->type = LV_SUBJECT_TYPE_FLOAT;
    subject->value.float_v = value;
}

#endif /*LV_USE_FLOAT*/

static void init_pointer(lv_subject_t * subject, void * value)
{
    LV_ASSERT(subject != NULL);
    subject->type = LV_SUBJECT_TYPE_POINTER;
    subject->value.pointer = value;
}

static void init_color(lv_subject_t * subject, lv_color_t color)
{
    LV_ASSERT(subject != NULL);
    subject->type = LV_SUBJECT_TYPE_COLOR;
    subject->value.color = color;
}



static void init_string(lv_subject_t * subject, char * buf, size_t size, const char * value)
{
    LV_ASSERT(subject != NULL);
    LV_ASSERT(buf != NULL || size == 0);

    subject->type = LV_SUBJECT_TYPE_STRING;

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

    /* First, while the Subject is still whole, so the callback can read it and reach
     * whatever it has to release. Cleared before the call so a callback that somehow
     * re-enters cannot run it twice. */
    if(subject->delete_cb) {
        lv_subject_delete_cb_t cb = subject->delete_cb;
        void * cb_user_data = subject->delete_user_data;
        subject->delete_cb = NULL;
        subject->delete_user_data = NULL;
        cb(subject, cb_user_data);
    }

    /* Both directions, so deleting a Subject in the middle of a graph leaves no
     * dangling edge in its dependencies or its dependents. */
    edges_teardown(subject);
    static_deps_release(subject);
    subject->has_mapper = 0;
    subject->dirty = 0;
    subject->pending_notify = 0;

    /* A copying Subject's storage goes with it, released by its own free_cb. */
    buf_release(subject);

    /* Whatever the Subject owns goes with it. */
    if(subject->owns_value) {
        void * stored = (void *)subject->value.pointer;
        subject->value.pointer = NULL;
        subject->owns_value = 0;

        if(stored != NULL) {
            if(subject->value_free_cb) subject->value_free_cb(stored);
            else lv_free(stored);
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
        lv_obj_add_flag(observer->obj, p->flag);
    }
    else {
        lv_obj_remove_flag(observer->obj, p->flag);
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
        lv_obj_add_state(observer->obj, p->flag);
    }
    else {
        lv_obj_remove_state(observer->obj, p->flag);
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
    lv_obj_t * obj = (lv_obj_t *)observer->obj;
    lv_obj_set_bool_t set_bool_cb = (lv_obj_set_bool_t)observer->user_cb;
    if(set_bool_cb == NULL) return;

    bool value;
    if(observer->has_mapper) {
        value = observer->out.num != 0;
        if(!observer->mapper.bool_cb(observer, subject->value, &value)) return;
        observer->out.num = value ? 1 : 0;
    }
    else {
        value = subject->value.num;
    }
    set_bool_cb(obj, value);
}

static void set_int_observer(lv_observer_t * observer, lv_subject_t * subject)
{
    lv_obj_t * obj = (lv_obj_t *)observer->obj;
    lv_obj_set_int_t set_int_cb = (lv_obj_set_int_t)observer->user_cb;
    if(set_int_cb == NULL) return;

    int32_t value;
    if(observer->has_mapper) {
        value = observer->out.num;
        if(!observer->mapper.int_cb(observer, subject->value, &value)) return;
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
    lv_obj_t * obj = (lv_obj_t *)observer->obj;
    lv_obj_set_float_t set_float_cb = (lv_obj_set_float_t)observer->user_cb;
    if(set_float_cb == NULL) return;

    float value;
    if(observer->has_mapper) {
        value = observer->out.float_v;
        if(!observer->mapper.float_cb(observer, subject->value, &value)) return;
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
    lv_obj_t * obj = (lv_obj_t *)observer->obj;
    lv_obj_set_string_t set_string_cb = (lv_obj_set_string_t)observer->user_cb;
    if(set_string_cb == NULL) return;

    const char * value;
    if(observer->has_mapper) {
        value = observer->out.pointer;
        if(!observer->mapper.string_cb(observer, subject->value, &value)) return;
        observer->out.pointer = value;  /* the mapper owns the storage, we keep the pointer */
    }
    else {
        value = subject->value.pointer;
    }
    set_string_cb(obj, value);
}


static void set_color_observer(lv_observer_t * observer, lv_subject_t * subject)
{
    lv_obj_t * obj = (lv_obj_t *)observer->obj;
    lv_obj_set_color_t set_color_cb = (lv_obj_set_color_t)observer->user_cb;
    if(set_color_cb == NULL) return;

    lv_color_t value;
    if(observer->has_mapper) {
        value = observer->out.color;
        if(!observer->mapper.color_cb(observer, subject->value, &value)) return;
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
        if(!observer->mapper.int_cb(observer, subject->value, &value)) return;
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
        if(!observer->mapper.color_cb(observer, subject->value, &value)) return;
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
        if(!observer->mapper.int_cb(observer, subject->value, &value)) return;
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
    lv_obj_t * obj = (lv_obj_t *)observer->obj;
    lv_obj_set_pointer_t set_pointer_cb = (lv_obj_set_pointer_t)observer->user_cb;
    if(set_pointer_cb == NULL) return;

    const void * value;
    if(observer->has_mapper) {
        value = observer->out.pointer;
        if(!observer->mapper.pointer_cb(observer, subject->value, &value)) return;
        observer->out.pointer = value;
    }
    else {
        value = subject->value.pointer;
    }
    set_pointer_cb(obj, value);
}



#endif /*LV_USE_OBSERVER*/
