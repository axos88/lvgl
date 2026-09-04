/**
 * @file lv_observer.h
 *
 * Subjects are observable values. A Subject may carry a mapper, which owns its value
 * and can derive it from other Subjects; reading a Subject from inside a mapper is what
 * makes it a dependency, so the dependency graph builds itself.
 *
 * The reactive model here was inspired by Angular signals. No knowledge of those is
 * needed to use this API, and the rest of the documentation does not refer to them.
 */

#ifndef LV_OBSERVER_H
#define LV_OBSERVER_H

#ifdef __cplusplus
extern "C" {
#endif

/*********************
 *      INCLUDES
 *********************/

#include "lv_obj.h"
#include "lv_ext_data.h"
#include "../misc/lv_ll.h"

#if LV_USE_OBSERVER

/*********************
 *      DEFINES
 *********************/

/**********************
 *      TYPEDEFS
 **********************/

/**
 * Values for lv_subject_t's `type` field
 */
typedef enum {
    LV_SUBJECT_TYPE_INVALID =   0,   /**< indicates Subject not initialized yet */
    LV_SUBJECT_TYPE_NONE =      1,   /**< no value of its own; only aggregates other
                                      *   Subjects through its mapper */
    LV_SUBJECT_TYPE_INT =       2,   /**< an int32_t */
    LV_SUBJECT_TYPE_FLOAT =     3,   /**< a float, requires `LV_USE_FLOAT 1` */
    LV_SUBJECT_TYPE_POINTER =   4,   /**< a void pointer */
    LV_SUBJECT_TYPE_COLOR   =   5,   /**< an lv_color_t */
    /* 6 was LV_SUBJECT_TYPE_GROUP, removed. Use LV_SUBJECT_TYPE_NONE with a mapper. */
    LV_SUBJECT_TYPE_STRING  =   7,   /**< a char pointer */
} lv_subject_type_t;

/**
 * A common type to handle all the various observable types in the same way
 */
typedef union {
    int32_t num;           /**< Integer number (opacity, enums, booleans or "normal" numbers) */
    const void * pointer;  /**< Constant pointer  (string buffer, format string, font, cone text, etc.) */
    lv_color_t color;      /**< Color */
#if LV_USE_FLOAT
    float float_v;         /**< Floating point value*/
#endif
} lv_subject_value_t;

/**
 * When a Subject with a mapper re-evaluates it.
 */
typedef enum {
    /** Only mark the Subject dirty when a dependency changes. The mapper runs when the
     * value is read, when an eager dependent pulls it, or at the next
     * `lv_subject_flush()`. A lazy Subject's mapper must be pure. */
    LV_SUBJECT_MODE_LAZY  = 0,
    /** Re-evaluate and notify inside the `lv_subject_set_...()` call that dirtied it.
     * An eager Subject's mapper may have side effects. */
    LV_SUBJECT_MODE_EAGER = 1,
} lv_subject_mode_t;

/**
 * When an Observer is notified.
 */
typedef enum {
    /** Notified inside the `lv_subject_set_...()` call that changed the value. Makes
     * the observed Subject act eager. This is the default, and it is how Observers
     * behaved before mappers existed. */
    LV_OBSERVER_MODE_IMMEDIATE = 0,
    /** Notified once per `lv_subject_flush()`, i.e. once per frame, no matter how many
     * times the value changed in between. Does not make the Subject eager. This is the
     * equivalent of a scheduled effect: use it when the Observer only needs the latest
     * value, e.g. one that just refreshes a Label. */
    LV_OBSERVER_MODE_BATCHED   = 1,
} lv_observer_mode_t;

/**
 * Mapper of a Subject whose value is an integer.
 *
 * The mapper owns the Subject's value: it is the only thing that writes it, and it
 * decides what counts as a change.
 *
 * @param subject     pointer to Subject being evaluated
 * @param user_data   the pointer given to `lv_subject_set_int_mapper()`. Use it to
 *                    reach the other Subjects and state the mapper needs, the way a
 *                    closure captures variables.
 * @param input       the value most recently written with a `lv_subject_set_...()`
 *                    call. Read the member matching the Subject's **input type**, which
 *                    need not be the type of its value: see
 *                    @ref lv_subject_create_mapped. On a re-evaluation caused by a
 *                    dependency change it is still that last written value, so a mapper
 *                    can re-derive from the original input.
 * @param value       in/out pointer to the Subject's stored value. It holds the value
 *                    from the previous evaluation on entry. Save it to a local first if
 *                    you need to compare against it.
 * @return            `true` if the mapper changed `*value`, `false` otherwise
 *
 * @note Every `lv_subject_get_...()` call made from a mapper registers the Subject it
 *       read as a dependency of `subject`.
 * @note A pointer input is **retained**: a re-evaluation gets the same pointer the last
 *       write carried. The caller answers for it, and has to keep it valid until a new
 *       input is written, or transfer ownership with `lv_subject_set_pointer_owned()`.
 * @note A string input is the exception: `input.pointer` is the written string on a
 *       write and NULL on a re-evaluation. `lv_subject_snprintf()` formats into a
 *       temporary it frees before returning, so keeping that pointer would dangle, and
 *       it is not needed anyway: a string mapper re-derives from `buf`, which still
 *       holds the last value. This concerns the input only; a string Subject's *value*
 *       is retained in its buffer.
 * @warning A mapper must not write any Subject. Doing so is ignored and logs a warning.
 */
typedef bool (*lv_subject_int_mapper_t)(lv_subject_t * subject, void * user_data, lv_subject_value_t input,
                                        int32_t * value);

#if LV_USE_FLOAT
/**
 * Mapper of a Subject whose value is a float. See @ref lv_subject_int_mapper_t.
 */
typedef bool (*lv_subject_float_mapper_t)(lv_subject_t * subject, void * user_data, lv_subject_value_t input,
                                          float * value);
#endif

/**
 * Mapper of a Subject whose value is a pointer. See @ref lv_subject_int_mapper_t.
 */
typedef bool (*lv_subject_pointer_mapper_t)(lv_subject_t * subject, void * user_data, lv_subject_value_t input,
                                            const void ** value);

/**
 * Mapper of a Subject whose value is a color. See @ref lv_subject_int_mapper_t.
 */
typedef bool (*lv_subject_color_mapper_t)(lv_subject_t * subject, void * user_data, lv_subject_value_t input,
                                          lv_color_t * value);

/**
 * Mapper of a Subject whose value is a string. See @ref lv_subject_int_mapper_t.
 *
 * A string Subject owns its buffer, so the mapper is handed the buffer instead of a
 * value. `buf` holds the previous string on entry, so comparing against it is how the
 * mapper decides whether anything changed.
 *
 * @param subject     pointer to Subject being evaluated
 * @param user_data   the pointer given to `lv_subject_set_string_mapper()`
 * @param input       the most recently written value, as for @ref lv_subject_int_mapper_t
 * @param buf         the Subject's string buffer, holding the previous value on entry
 * @param size        size of `buf`
 * @return            `true` if the mapper changed the contents of `buf`
 */
typedef bool (*lv_subject_string_mapper_t)(lv_subject_t * subject, void * user_data, lv_subject_value_t input,
                                           char * buf, size_t size);

/**
 * Mapper of an `LV_SUBJECT_TYPE_NONE` Subject, which has no value of its own and
 * exists only to aggregate other Subjects. Read the Subjects you care about and return
 * `true` to notify.
 * @param subject     pointer to Subject being evaluated
 * @param user_data   the pointer given to `lv_subject_set_none_mapper()`
 * @return            `true` to notify this Subject's Observers
 */
typedef bool (*lv_subject_none_mapper_t)(lv_subject_t * subject, void * user_data);

/**
 * Orders two Subject values. Used by the `lv_subject_create_min()`,
 * `lv_subject_create_max()` and `lv_subject_create_clamped()` helpers, which cannot
 * know how to order a pointer.
 * @param subject   pointer to the Subject being evaluated
 * @param a         one value
 * @param b         the other value
 * @return          negative if `a` orders before `b`, 0 if equivalent, positive if
 *                  `a` orders after `b`
 * @note            `lv_subject_compare_int()`, `lv_subject_compare_float()` and
 *                  `lv_subject_compare_color()` are ready to use.
 */
typedef int (*lv_subject_compare_cb_t)(lv_subject_t * subject, lv_subject_value_t a, lv_subject_value_t b);

/**
 * The bounds of a clamped Subject, as accepted by its `lv_subject_set_pointer()`.
 * See @ref lv_subject_create_clamped.
 */
typedef struct {
    lv_subject_value_t min_value;   /**< Lower bound */
    lv_subject_value_t max_value;   /**< Upper bound */
} lv_subject_range_t;

/**
 * A Subject's mapper, selected by the type of the Subject's **value**.
 */
typedef union {
    lv_subject_int_mapper_t int_cb;          /**< Value is an int32_t */
#if LV_USE_FLOAT
    lv_subject_float_mapper_t float_cb;      /**< Value is a float */
#endif
    lv_subject_pointer_mapper_t pointer_cb;  /**< Value is a pointer */
    lv_subject_color_mapper_t color_cb;      /**< Value is a color */
    lv_subject_string_mapper_t string_cb;    /**< Value is a string */
    lv_subject_none_mapper_t none_cb;        /**< LV_SUBJECT_TYPE_NONE */
} lv_subject_mapper_t;

/**
 * Grows the buffer of a Subject that copies its value.
 * @param buf    the current buffer @nullable NULL on the first allocation
 * @param size   the number of bytes needed
 * @return       the buffer, possibly moved, or NULL if it could not be grown. The old
 *               buffer must be left untouched when NULL is returned.
 */
typedef void * (*lv_subject_realloc_cb_t)(void * buf, size_t size);

/**
 * Releases a value a Subject owns.
 * @param value   the value to release
 */
typedef void (*lv_subject_value_free_cb_t)(void * value);

/**
 * The buffer of a Subject that copies its value, as set up by
 * @ref lv_subject_set_buffer. A Subject only has one if it copies.
 */
typedef struct {
    void * buf;                             /**< The storage; also the Subject's value */
    size_t capacity;                        /**< Bytes allocated */
    size_t length;                          /**< Bytes in use. For a string this excludes
                                             *   the terminating zero. */
    lv_subject_realloc_cb_t realloc_cb;     /**< Grows `buf`. @nullable NULL means the
                                             *   buffer is fixed and cannot grow. */
    lv_subject_value_free_cb_t free_cb;     /**< Releases `buf` when it is replaced or the
                                             *   Subject is deleted. @nullable */
} lv_subject_buf_t;



/**
 * The Subject (an observable value)
 */
struct _lv_subject_t {
#if LV_USE_EXT_DATA
    lv_ext_data_t ext_data;
#endif
    lv_ll_t subs_ll;                     /**< Subscribers */
    lv_subject_value_t value;            /**< Current value, written only by the mapper */
    lv_subject_value_t last_input;       /**< Value most recently passed to `lv_subject_set_...()`,
                                          * handed to the mapper on a re-evaluation. Unused for
                                          * `LV_SUBJECT_TYPE_STRING`, which does not retain it. */
    lv_subject_mapper_t mapper;          /**< Maps the input to the stored value */
    void * mapper_user_data;             /**< Passed to `mapper`, the mapper's captured state */
    lv_subject_value_free_cb_t value_free_cb; /**< Releases an owned pointer value */
    lv_subject_buf_t * buf;              /**< Set when the Subject copies its value @nullable */
    void * user_data;                    /**< Additional parameter, can be used freely by user */

    lv_ll_t deps;                        /**< Subjects this one read during its last evaluation */
    lv_ll_t dependents;                  /**< Subjects that read this one */
    uint16_t immediate_observer_cnt;     /**< Observers with LV_OBSERVER_MODE_IMMEDIATE */
    uint32_t version;                    /**< Bumped on every actual value change. A dependent
                                          * compares it against the version it last read to
                                          * decide whether its mapper has to run at all. */

    uint32_t type                 :  4;  /**< Type of the *value*, i.e. what Observers see.
                                          * One of the LV_SUBJECT_TYPE_... values. */
    uint32_t input_type           :  4;  /**< Type accepted by the `lv_subject_set_...()`
                                          * functions. Equal to `type` unless the Subject was
                                          * made with `lv_subject_create_mapped()`. */
    uint32_t input_pending        :  1;  /**< A write was recorded but its mapper has not run
                                          * yet, so the pending work is not only a stale
                                          * dependency and the mapper must not be skipped. */
    uint32_t copy_changed         :  1;  /**< Did the last copy into `buf` change the bytes?
                                          * Computed by the copying setters, which have the
                                          * old bytes to hand, and used for change detection. */
    uint32_t notify_restart_query :  1;  /**< If an Observer was deleted during notification,
                                          * start notifying from the beginning. */
    uint32_t mode                 :  1;  /**< One of the LV_SUBJECT_MODE_... values */
    uint32_t dirty                :  1;  /**< A dependency changed, so the mapper has to run again */
    uint32_t pending_notify       :  1;  /**< Value is up to date but the Observers have not
                                          * been told yet. */
    uint32_t evaluating           :  1;  /**< Re-entrancy guard, used to detect dependency cycles */
    uint32_t has_mapper           :  1;  /**< Is `mapper` set? */
    uint32_t in_list              :  1;  /**< Is the Subject in LVGL's global Subject list?
                                          * False for a Subject set up with a deprecated
                                          * `lv_subject_init_...()`, which the application owns. */
    uint32_t owns_mapper_user_data :  1; /**< `mapper_user_data` was allocated by one of the
                                          * `lv_subject_create_...()` helpers and is freed
                                          * with the Subject. */
    uint32_t owns_value           :  1;  /**< The Subject owns the data its stored pointer
                                          * value refers to. */
    uint32_t owns_input           :  1;  /**< The Subject owns the data its retained input
                                          * pointer refers to. */
};

/**
  * Callback called to notify Observer that Subject's value has changed
  * @param observer     pointer to Observer
  * @param subject      pointer to Subject being observed
  */
typedef void (*lv_observer_cb_t)(lv_observer_t * observer, lv_subject_t * subject);

/**
 * Mapper of an Observer that pushes an integer out.
 * Runs when the Observer is notified, before the value reaches its target, so a
 * Subject of any type can drive an integer target.
 * @param observer  pointer to Observer
 * @param out       pointer to the Observer's output slot, holding the last pushed value
 * @return          `true` if the mapper changed `*out`. When `false` the target is
 *                  not updated at all.
 * @note            Read the observed value with `lv_observer_get_subject()` and the
 *                  matching `lv_subject_get_...()`. Unlike a Subject's mapper, an
 *                  Observer's mapper does not register dependencies: an Observer is
 *                  not a node in the dependency graph and runs only when its own
 *                  Subject notifies.
 */
typedef bool (*lv_observer_int_mapper_t)(lv_observer_t * observer, int32_t * out);

/**
 * Mapper of an Observer that pushes a boolean out. See @ref lv_observer_int_mapper_t.
 */
typedef bool (*lv_observer_bool_mapper_t)(lv_observer_t * observer, bool * out);

#if LV_USE_FLOAT
/**
 * Mapper of an Observer that pushes a float out. See @ref lv_observer_int_mapper_t.
 */
typedef bool (*lv_observer_float_mapper_t)(lv_observer_t * observer, float * out);
#endif

/**
 * Mapper of an Observer that pushes a string out. See @ref lv_observer_int_mapper_t.
 * @note `*out` has to point to storage the mapper itself owns, e.g. a static buffer
 *       or one reached through `lv_observer_get_user_data()`. The Observer only keeps
 *       the pointer.
 */
typedef bool (*lv_observer_string_mapper_t)(lv_observer_t * observer, const char ** out);

/**
 * Mapper of an Observer that pushes a color out. See @ref lv_observer_int_mapper_t.
 */
typedef bool (*lv_observer_color_mapper_t)(lv_observer_t * observer, lv_color_t * out);

/**
 * Mapper of an Observer that pushes a pointer out. See @ref lv_observer_int_mapper_t.
 */
typedef bool (*lv_observer_pointer_mapper_t)(lv_observer_t * observer, const void ** out);

/**
 * An Observer's mapper, selected by the type the Observer pushes out.
 */
typedef union {
    lv_observer_int_mapper_t int_cb;          /**< Pushes an int32_t */
    lv_observer_bool_mapper_t bool_cb;        /**< Pushes a bool */
#if LV_USE_FLOAT
    lv_observer_float_mapper_t float_cb;      /**< Pushes a float */
#endif
    lv_observer_string_mapper_t string_cb;    /**< Pushes a const char * */
    lv_observer_color_mapper_t color_cb;      /**< Pushes an lv_color_t */
    lv_observer_pointer_mapper_t pointer_cb;  /**< Pushes a const void * */
} lv_observer_mapper_t;


/**
 * Generic callback called to set a boolean value on a Widget.
 * @param obj       pointer to Widget
 * @param value     new value
 */
typedef void (*lv_obj_set_bool_t)(lv_obj_t * obj, bool value);

/**
 * Generic callback called to set an int value on a Widget.
 * @param obj       pointer to Widget
 * @param value     new value
 */
typedef void (*lv_obj_set_int_t)(lv_obj_t * obj, int32_t value);

/**
 * Generic callback called to set a float value on a Widget.
 * @param obj       pointer to Widget
 * @param value     new value
 */
typedef void (*lv_obj_set_float_t)(lv_obj_t * obj, float value);

/**
 * Generic callback called to set a string value on a Widget.
 * @param obj       pointer to Widget
 * @param value     new value
 */
typedef void (*lv_obj_set_string_t)(lv_obj_t * obj, const char * value);

/**
 * Generic callback called to set a color value on a Widget.
 * @param obj       pointer to Widget
 * @param value     new value
 */
typedef void (*lv_obj_set_color_t)(lv_obj_t * obj, lv_color_t value);

/**
 * A style setter that takes an integer, e.g. `lv_obj_set_style_pad_all`. 58 of LVGL's
 * style setters have this shape.
 * @param obj       pointer to Widget
 * @param value     new value
 * @param selector  part and state, as passed to any style setter
 */
typedef void (*lv_obj_set_style_int_t)(lv_obj_t * obj, int32_t value, lv_style_selector_t selector);

/**
 * A style setter that takes a color, e.g. `lv_obj_set_style_bg_color`.
 * @param obj       pointer to Widget
 * @param value     new value
 * @param selector  part and state
 */
typedef void (*lv_obj_set_style_color_t)(lv_obj_t * obj, lv_color_t value, lv_style_selector_t selector);

/**
 * A style setter that takes an opacity, e.g. `lv_obj_set_style_bg_opa`.
 * @param obj       pointer to Widget
 * @param value     new value
 * @param selector  part and state
 */
typedef void (*lv_obj_set_style_opa_t)(lv_obj_t * obj, lv_opa_t value, lv_style_selector_t selector);

/**
 * Generic callback called to set a pointer value on a Widget.
 * @param obj       pointer to Widget
 * @param value     new value
 */
typedef void (*lv_obj_set_pointer_t)(lv_obj_t * obj, const void * value);


/**********************
 * GLOBAL PROTOTYPES
 **********************/

/**
 * Create a subject of type `type`
 * @param type   the type of the subject. See @ref lv_subject_type_t
 */
lv_subject_t * lv_subject_create(lv_subject_type_t type);

/**
 * Create a Subject that is written as one type and observed as another.
 *
 * The `lv_subject_set_...()` function matching `input_type` is the one that writes it,
 * and the `lv_subject_get_...()` function matching `value_type` is the one that reads
 * it. The mapper converts, and it is the mapper for `value_type`.
 *
 * This collapses what would otherwise be two Subjects — a source and a derived one —
 * into a single one, for the common case where the raw form is of no interest to
 * anybody:
 *
 * ```c
 * // Set with a temperature, observed as a level
 * lv_subject_t * level = lv_subject_create_mapped(LV_SUBJECT_TYPE_FLOAT, LV_SUBJECT_TYPE_INT);
 * lv_subject_set_int_mapper(level, level_mapper, NULL);
 *
 * static bool level_mapper(lv_subject_t * s, void * ud, lv_subject_value_t input, int32_t * value)
 * {
 *     int32_t next = input.float_v > 80.0f ? LEVEL_CRITICAL :
 *                    input.float_v > 60.0f ? LEVEL_HIGH : LEVEL_NORMAL;
 *     if(next == *value) return false;
 *     *value = next;
 *     return true;
 * }
 *
 * lv_subject_set_float(level, 72.5f);      // written as a float
 * lv_subject_get_int(level);               // read as LEVEL_HIGH
 * ```
 *
 * @param input_type    the type the `lv_subject_set_...()` functions accept
 * @param value_type    the type of the stored value, i.e. what Observers see
 * @return              the new Subject, or NULL on failure
 * @note                Such a Subject needs a mapper; without one there is nothing to
 *                      convert with, and a write logs a warning and is ignored.
 * @note                Pass the same type twice to get exactly what
 *                      `lv_subject_create()` gives you.
 */
lv_subject_t * lv_subject_create_mapped(lv_subject_type_t input_type, lv_subject_type_t value_type);

/**
 * Get the type a Subject's `lv_subject_set_...()` functions accept.
 * @param subject   pointer to Subject
 * @return          the input type, equal to the value type for an ordinary Subject
 */
lv_subject_type_t lv_subject_get_input_type(const lv_subject_t * subject);

/**
 * Delete a Subject.
 *
 * Refused if the Subject is a dependency of another Subject, i.e. if some other
 * Subject's mapper reads it. Deleting it would leave that mapper without an input.
 * Delete the dependents first, or use `lv_subject_delete_cascade()`.
 *
 * @param subject   the subject to delete @nullable
 * @note            Subjects still alive when `lv_deinit()` is called are deleted
 *                  automatically, regardless of their dependencies.
 */
void lv_subject_delete(lv_subject_t * subject);

/**
 * Delete a Subject together with everything that depends on it.
 *
 * Deletes the transitive closure of dependents first, deepest first, then the Subject
 * itself, the way `ON DELETE CASCADE` does. Use it when a whole derived chain goes away
 * with its source.
 *
 * @param subject   the subject to delete @nullable
 * @warning         Every Subject that derives from `subject`, directly or indirectly,
 *                  is deleted too. Pointers to them become invalid.
 */
void lv_subject_delete_cascade(lv_subject_t * subject);

/**
 * Initialize an integer-type Subject.
 * @param subject   pointer to Subject
 * @param value     initial value
 * @deprecated      The subject init API is deprecated, use `lv_subject_create` instead
 */
LV_DEPRECATED("The subject init API is deprecated,use `lv_subject_create` instead")
void lv_subject_init_int(lv_subject_t * subject, int32_t value);

/**
 * Set value of an integer Subject and notify Observers.
 * @param subject   pointer to Subject
 * @param value     new value
 */
void lv_subject_set_int(lv_subject_t * subject, int32_t value);

/**
 * Get current value of an integer Subject.
 * @param subject   pointer to Subject
 * @return          current value
 */
int32_t lv_subject_get_int(lv_subject_t * subject);




#if LV_USE_FLOAT

/**
 * Initialize an float-type Subject.
 * @param subject   pointer to Subject
 * @param value     initial value
 * @deprecated      The subject init API is deprecated, use `lv_subject_create` instead
 */
LV_DEPRECATED("The subject init API is deprecated,use `lv_subject_create` instead")
void lv_subject_init_float(lv_subject_t * subject, float value);

/**
 * Set value of an float Subject and notify Observers.
 * @param subject   pointer to Subject
 * @param value     new value
 */
void lv_subject_set_float(lv_subject_t * subject, float value);

/**
 * Get current value of an float Subject.
 * @param subject   pointer to Subject
 * @return          current value
 */
float lv_subject_get_float(lv_subject_t * subject);



#endif /*LV_USE_FLOAT*/

/**
 * Initialize a string-type Subject.
 * @param subject   pointer to Subject
 * @param buf       pointer to buffer to store string
 * @param size      size of the buffer
 * @param value     initial value of string, e.g. "hello"
 * @note            A string Subject stores its own copy of the string, not just the pointer.
 * @deprecated      The subject init API is deprecated, use `lv_subject_create` instead
 */
LV_DEPRECATED("The subject init API is deprecated,use `lv_subject_create` instead")
void lv_subject_init_string(lv_subject_t * subject, char * buf, size_t size, const char * value);

/**
 * Give a Subject a buffer, so that it *copies* its value instead of referring to it.
 *
 * A Subject that copies has no lifetime question: the data written is copied in, so it
 * only has to be valid for the duration of the call. It also gets change detection for
 * free, because the old bytes are still there to compare against.
 *
 * The buffer can be fixed or growable:
 *
 * - Pass `buf` and `size` with `realloc_cb` NULL for a **fixed** buffer, e.g. a static
 *   array. A write that does not fit is refused.
 * - Pass `realloc_cb` for a **growable** buffer. `buf` may be NULL and `size` 0 to start
 *   with nothing. A write that does not fit grows the buffer, and is refused if the
 *   callback returns NULL.
 *
 * @param subject       pointer to a Subject of type `LV_SUBJECT_TYPE_STRING` or
 *                      `LV_SUBJECT_TYPE_POINTER`
 * @param buf           the initial buffer @nullable
 * @param size          size of `buf` in bytes
 * @param realloc_cb    grows the buffer. @nullable NULL means it cannot grow.
 * @param free_cb       releases the buffer when it is replaced or the Subject is deleted.
 *                      @nullable NULL means it is not released, which is what a static
 *                      buffer wants.
 *
 * @note Calling this again releases the previous buffer with its own `free_cb`.
 * @note A Subject either copies or refers. Setting a buffer makes
 *       `lv_subject_set_pointer()`, `lv_subject_set_pointer_owned()`,
 *       `lv_subject_set_string()` and `lv_subject_set_string_owned()` refuse, and
 *       `lv_subject_copy_pointer()` / `lv_subject_copy_string()` require one.
 */
void lv_subject_set_buffer(lv_subject_t * subject, void * buf, size_t size,
                           lv_subject_realloc_cb_t realloc_cb, lv_subject_value_free_cb_t free_cb);

/**
 * Give a string Subject a fixed buffer. A convenience for
 * `lv_subject_set_buffer(subject, buf, size, NULL, NULL)`.
 * @param subject   pointer to a Subject of type `LV_SUBJECT_TYPE_STRING`
 * @param buf       pointer to the buffer, which has to out-live the Subject
 * @param size      size of the buffer
 */
void lv_subject_set_string_buffer_static(lv_subject_t * subject, char * buf, size_t size);

/**
 * Copy data into a pointer Subject's buffer and notify Observers if the bytes changed.
 *
 * This is the third way a pointer Subject can hold its value, next to borrowing it and
 * owning it, and the only one with no lifetime question: `data` only has to be valid for
 * this call. It also detects change properly, because the previous bytes are compared
 * against the new ones — so a driver that republishes a mutated buffer notifies only
 * when something actually moved.
 *
 * @param subject   pointer to a Subject of type `LV_SUBJECT_TYPE_POINTER` that has a
 *                  buffer set with `lv_subject_set_buffer()`
 * @param data      the bytes to copy @nullable when `size` is 0
 * @param size      number of bytes
 * @return          `true` on success; `false` if the Subject has no buffer, or the buffer
 *                  is fixed and too small, or growing it failed. On failure the Subject
 *                  keeps its previous value and no Observer is notified.
 * @note            Read the length back with `lv_subject_get_pointer_size()`.
 */
bool lv_subject_copy_pointer(lv_subject_t * subject, const void * data, size_t size);

/**
 * Get the number of bytes a pointer Subject's last `lv_subject_copy_pointer()` stored.
 * @param subject   pointer to a Subject that copies its value
 * @return          the length in bytes, or 0 if the Subject does not copy
 */
size_t lv_subject_get_pointer_size(const lv_subject_t * subject);

/**
 * Copy a string into a Subject's buffer and notify Observers if it changed.
 * @param subject   pointer to Subject that has a buffer set with `lv_subject_set_buffer()`
 * @param buf       new string
 * @return          `true` on success; `false` if the Subject has no buffer, or the buffer
 *                  is fixed and too small, or growing it failed. On failure the Subject
 *                  keeps its previous value.
 */
bool lv_subject_copy_string(lv_subject_t * subject, const char * buf);

/**
 * Format a new string into a Subject's buffer and notify Observers if it changed.
 * A growable buffer is grown to fit; a fixed one refuses a string that does not fit.
 * @param subject   pointer to Subject that has a buffer set with `lv_subject_set_buffer()`
 * @param format    format string
 * @return          `true` on success, `false` if the string could not be stored
 */
bool lv_subject_snprintf(lv_subject_t * subject, const char * format, ...) LV_FORMAT_ATTRIBUTE(2, 3);

/**
 * Copy a string into a Subject's buffer, storing as much as fits, and notify Observers
 * if the stored text changed.
 *
 * The difference from `lv_subject_copy_string()` is what happens when the string does
 * not fit: that one refuses the write, this one trims. **Prefer this one whenever a
 * shortened label is better than no update at all**, which is most of the time for text
 * on screen. Reserve `lv_subject_copy_string()` for the cases where a truncated value
 * would be wrong rather than merely ugly.
 *
 * A growable buffer is still grown first, so trimming only happens when the buffer is
 * fixed and too small, or when growing it failed.
 *
 * @param subject   pointer to Subject that has a buffer set with `lv_subject_set_buffer()`
 * @param buf       new string
 * @return          `true` if the string was stored, trimmed or whole; `false` only if
 *                  the Subject has no buffer at all
 * @note            Trimming is silent. Compare `lv_strlen()` of the value against your
 *                  input if you need to know it happened.
 * @note            There is no pointer equivalent, because half a struct is not a
 *                  shorter struct. `lv_subject_copy_pointer()` always refuses a write
 *                  that does not fit.
 */
bool lv_subject_copy_string_trimmed(lv_subject_t * subject, const char * buf);

/**
 * Get current value of a string Subject.
 * @param subject   pointer to Subject
 * @return          pointer to buffer containing current value
 */
const char * lv_subject_get_string(lv_subject_t * subject);


/**
 * Initialize a pointer-type Subject.
 * @param subject   pointer to Subject
 * @param value     initial value @nullable
 * @deprecated      The subject init API is deprecated, use `lv_subject_create` instead
 */
LV_DEPRECATED("The subject init API is deprecated,use `lv_subject_create` instead")
void lv_subject_init_pointer(lv_subject_t * subject, void * value);

/**
 * Get current value of a pointer Subject.
 * @param subject   pointer to Subject
 * @return          current value
 */
const void * lv_subject_get_pointer(lv_subject_t * subject);


/**
 * Initialize a color-type Subject.
 * @param subject   pointer to Subject
 * @param color     initial value
 * @deprecated      The subject init API is deprecated, use `lv_subject_create` instead
 */
LV_DEPRECATED("The subject init API is deprecated,use `lv_subject_create` instead")
void lv_subject_init_color(lv_subject_t * subject, lv_color_t color);

/**
 * Set value of a color Subject and notify Observers if it changed.
 * @param subject   pointer to Subject
 * @param color     new value
 */
void lv_subject_set_color(lv_subject_t * subject, lv_color_t color);

/**
 * Get current value of a color Subject.
 * @param subject   pointer to Subject
 * @return          current value
 */
lv_color_t lv_subject_get_color(lv_subject_t * subject);




/**
 * Remove all Observers from a Subject and free allocated memory, and delete
 * any associated Widget-Binding events.  This leaves `subject` "disconnected" from
 * all Observers and all associated Widget events established through Widget Binding.
 * @param subject   pointer to Subject @nullable
 * @note            This can safely be called regardless of whether any Observers
 *                  added with `lv_subject_add_observer_obj()` or bound to a Widget Property
 *                  with one of the `..._bind_...()` functions.
 * @deprecated      The subject init API is deprecated, use `lv_subject_create`/`lv_subject_delete` instead
 */
LV_DEPRECATED("The subject init API is deprecated, use `lv_subject_create`/`lv_subject_delete` instead")
void lv_subject_deinit(lv_subject_t * subject);


/**
 * Set the mapper of an integer Subject.
 *
 * The mapper is the only thing that writes the Subject's value slot. It runs in two
 * situations, and its own code decides which role it plays:
 *
 * - `lv_subject_set_int()` seeds the slot with the written value and then runs the
 *   mapper. A mapper that reads the slot therefore acts as a write transform, e.g.
 *   a clamp.
 * - A dependency changed, so the Subject is re-evaluated. The mapper then runs on
 *   whatever the slot already holds. A mapper that overwrites the slot from the
 *   Subjects it reads therefore acts as a derived value.
 *
 * Dependencies are wired automatically: every `lv_subject_get_...()` call made from
 * inside the mapper registers the Subject it read as a dependency. The dependency
 * list is rebuilt on every evaluation, so a mapper may read different Subjects on
 * different runs.
 *
 * @param subject   pointer to an integer Subject
 * @param mapper    the mapper, or NULL to remove the current one
 * @note            Writing to a Subject whose mapper ignores the slot has no effect,
 *                  because the mapper overwrites the value that was just written.
 * @note            A `LV_SUBJECT_MODE_LAZY` Subject's mapper must be pure. Only a
 *                  `LV_SUBJECT_MODE_EAGER` Subject's mapper may have side effects.
 */
void lv_subject_set_int_mapper(lv_subject_t * subject, lv_subject_int_mapper_t mapper, void * user_data);

#if LV_USE_FLOAT
/**
 * Set the mapper of a float Subject. See @ref lv_subject_set_int_mapper.
 * @param subject   pointer to a float Subject
 * @param mapper    the mapper, or NULL to remove the current one
 * @param user_data passed to the mapper on every call; the mapper's captured state @nullable
 */
void lv_subject_set_float_mapper(lv_subject_t * subject, lv_subject_float_mapper_t mapper, void * user_data);
#endif

/**
 * Set the mapper of a pointer Subject. See @ref lv_subject_set_int_mapper.
 * @param subject   pointer to a pointer Subject
 * @param mapper    the mapper, or NULL to remove the current one
 * @param user_data passed to the mapper on every call; the mapper's captured state @nullable
 */
void lv_subject_set_pointer_mapper(lv_subject_t * subject, lv_subject_pointer_mapper_t mapper, void * user_data);

/**
 * Set the mapper of a color Subject. See @ref lv_subject_set_int_mapper.
 * @param subject   pointer to a color Subject
 * @param mapper    the mapper, or NULL to remove the current one
 * @param user_data passed to the mapper on every call; the mapper's captured state @nullable
 */
void lv_subject_set_color_mapper(lv_subject_t * subject, lv_subject_color_mapper_t mapper, void * user_data);

/**
 * Set the mapper of a string Subject. See @ref lv_subject_set_int_mapper.
 * The mapper receives the Subject's buffer instead of a value.
 * @param subject   pointer to a string Subject
 * @param mapper    the mapper, or NULL to remove the current one
 * @note            The string buffer has to be set with
 *                  `lv_subject_set_string_buffer_static()` before the mapper can run.
 */
void lv_subject_set_string_mapper(lv_subject_t * subject, lv_subject_string_mapper_t mapper, void * user_data);

/**
 * Set the mapper of an `LV_SUBJECT_TYPE_NONE` Subject, which has no value of its own
 * and only aggregates other Subjects. This is how a Subject that reacts to several
 * others is built now that the Group type is gone.
 * See @ref lv_subject_set_int_mapper.
 * @param subject   pointer to a Subject of type `LV_SUBJECT_TYPE_NONE`
 * @param mapper    the mapper, or NULL to remove the current one
 * @note            Such a Subject is normally declared `LV_SUBJECT_MODE_EAGER`, so its
 *                  Observers are notified as soon as any aggregated Subject changes.
 */
void lv_subject_set_none_mapper(lv_subject_t * subject, lv_subject_none_mapper_t mapper, void * user_data);

/**
 * Set when a Subject with a mapper re-evaluates.
 * @param subject   pointer to Subject
 * @param mode      `LV_SUBJECT_MODE_LAZY` (the default) or `LV_SUBJECT_MODE_EAGER`
 * @note            An Observer with `LV_OBSERVER_MODE_IMMEDIATE` makes a lazy Subject
 *                  act as an eager one for as long as it is subscribed. That changes
 *                  when the mapper runs, not what it is allowed to do: a lazy
 *                  Subject's mapper must stay pure.
 */
void lv_subject_set_mode(lv_subject_t * subject, lv_subject_mode_t mode);

/**
 * Get the declared mode of a Subject.
 * @param subject   pointer to Subject
 * @return          the mode set with `lv_subject_set_mode()`, ignoring any
 *                  promotion by eager Observers
 */
lv_subject_mode_t lv_subject_get_mode(const lv_subject_t * subject);

/**
 * Tell whether a Subject currently evaluates eagerly, i.e. whether it is declared
 * `LV_SUBJECT_MODE_EAGER` or has at least one `LV_OBSERVER_MODE_IMMEDIATE` Observer.
 * @param subject   pointer to Subject
 * @return          `true` if the Subject re-evaluates inside `lv_subject_set_...()`
 */
bool lv_subject_is_eager(const lv_subject_t * subject);

/**
 * Tell whether a Subject is waiting to be re-evaluated because a dependency changed.
 * @param subject   pointer to Subject
 * @return          `true` if the Subject is dirty
 */
bool lv_subject_is_dirty(const lv_subject_t * subject);

/**
 * Set a Subject's user data. A mapper reaches it with `lv_subject_get_user_data()`,
 * which is what makes one mapper function reusable across several Subjects.
 * @param subject   pointer to Subject
 * @param user_data pointer to user-owned data @nullable
 */
void lv_subject_set_user_data(lv_subject_t * subject, void * user_data);

/**
 * Get a Subject's user data.
 * @param subject   pointer to Subject
 * @return          the pointer set with `lv_subject_set_user_data()`
 */
void * lv_subject_get_user_data(const lv_subject_t * subject);

/**
 * Get the number of Subjects the given Subject read during its last evaluation.
 * @param subject   pointer to Subject
 * @return          number of dependencies
 */
uint32_t lv_subject_get_dependency_count(const lv_subject_t * subject);

/**
 * Get one of the Subjects the given Subject read during its last evaluation.
 * @param subject   pointer to Subject
 * @param index     index of the dependency
 * @return          the dependency, or NULL if `index` is out of bounds
 */
lv_subject_t * lv_subject_get_dependency(const lv_subject_t * subject, uint32_t index);

/**
 * Do the deferred work: evaluate every Subject that still needs its mapper run, and
 * notify every Observer that is still waiting.
 *
 * Two kinds of work end up here:
 * - a lazy Subject that a dependency change left dirty, and
 * - a Subject whose value changed while it was not evaluating eagerly, so its
 *   Observers have not been told yet.
 *
 * This is what makes `LV_OBSERVER_MODE_BATCHED` mean "once per frame": however many
 * times a value changed since the last flush, its batched Observers are notified once.
 *
 * LVGL calls this once per `lv_timer_handler()` pass, so an application normally does
 * not have to. Call it directly to force the pending work to happen at a chosen moment,
 * e.g. in a unit test that does not run a frame.
 *
 * @note A dirty Subject with no Observers at all is left alone. It is evaluated when
 *       something reads it or when an eager dependent pulls it.
 */
void lv_subject_flush(void);

/**
 * Set the value of a pointer Subject, keeping ownership of the data.
 *
 * The Subject only borrows the pointer, so **the data has to stay valid until a new
 * value is written**, or until the Subject is deleted. Both the stored value and the
 * retained input refer to it, and a mapper is handed the retained input again on every
 * re-evaluation, so "until a new value is written" is longer than it may look.
 *
 * Use `lv_subject_set_pointer_owned()` instead to hand that responsibility over.
 *
 * @param subject   pointer to a Subject of type `LV_SUBJECT_TYPE_POINTER`
 * @param ptr       new value @nullable
 * @note            Observers are notified whether or not the pointer itself changed,
 *                  because the data behind an unchanged pointer may have changed. A
 *                  mapper can narrow that down; see the data-binding documentation.
 */
void lv_subject_set_pointer(lv_subject_t * subject, void * ptr);

/**
 * Set the value of a pointer Subject and hand it ownership of the data.
 *
 * The Subject releases the data once neither its stored value nor its retained input
 * refers to it any more, and releases whatever it still owns when it is deleted. So the
 * caller can allocate and forget:
 *
 * ```c
 * lv_subject_set_pointer_owned(frame, decode(), lv_free);
 * lv_subject_set_pointer_owned(frame, decode(), lv_free);   // releases the first
 * lv_subject_delete(frame);                                 // releases the second
 * ```
 *
 * @param subject   pointer to a Subject of type `LV_SUBJECT_TYPE_POINTER`
 * @param ptr       new value, whose ownership passes to the Subject @nullable
 * @param free_cb   releases the data. @nullable NULL means `lv_free()`.
 *
 * @note Republishing the **same** pointer never releases it, so a driver that mutates
 *       its buffer in place and publishes it again is safe.
 * @note The release happens **after** the mapper has run, so a mapper may still read the
 *       outgoing value, and a mapper that keeps it keeps it alive.
 * @note Ownership follows what the write handed in. If a mapper stores some *other*
 *       pointer, the Subject treats that one as borrowed and never releases it; a mapper
 *       that publishes its own allocation is responsible for it.
 */
void lv_subject_set_pointer_owned(lv_subject_t * subject, void * ptr, lv_subject_value_free_cb_t free_cb);

/**
 * Set a string Subject's value to a string it borrows, without copying.
 *
 * No buffer is needed, and nothing is copied, so **the string has to stay valid until a
 * new value is written**. Use it for a literal or a long-lived buffer.
 *
 * @param subject   pointer to a Subject of type `LV_SUBJECT_TYPE_STRING`
 * @param str       new value @nullable
 * @note            This is the pointer form of a string Subject. The other form is to
 *                  give the Subject a buffer with `lv_subject_set_string_buffer_static()`
 *                  and copy into it with `lv_subject_copy_string()`, which has no
 *                  lifetime question at all. A Subject uses one form or the other.
 */
void lv_subject_set_string(lv_subject_t * subject, const char * str);

/**
 * Set a string Subject's value to a string whose ownership passes to the Subject.
 *
 * Nothing is copied. The Subject releases the string once it no longer refers to it, and
 * when it is deleted, exactly as `lv_subject_set_pointer_owned()` does.
 *
 * @param subject   pointer to a Subject of type `LV_SUBJECT_TYPE_STRING`
 * @param str       new value, whose ownership passes to the Subject @nullable
 * @param free_cb   releases the string. @nullable NULL means `lv_free()`.
 */
void lv_subject_set_string_owned(lv_subject_t * subject, char * str, lv_subject_value_free_cb_t free_cb);

/**
 * Tell whether a Subject currently owns the data its stored pointer value refers to.
 * @param subject   pointer to Subject
 * @return          `true` if the Subject releases its stored value
 */
bool lv_subject_is_value_owned(const lv_subject_t * subject);

/**
 * Create a Subject that holds the smallest value another Subject has taken so far.
 *
 * The returned Subject has the same type as `source` and depends on it, so it updates
 * whenever `source` changes and never goes back up. Use it for a running minimum, e.g.
 * the lowest temperature seen since boot.
 *
 * @param source        the Subject to watch
 * @param compare_cb    how to order two values. @nullable Pass NULL to use the natural
 *                      ordering, which is available for `LV_SUBJECT_TYPE_INT`,
 *                      `LV_SUBJECT_TYPE_FLOAT` and `LV_SUBJECT_TYPE_COLOR`. A pointer
 *                      or string Subject has no natural ordering, so it needs one.
 * @return              the new Subject, or NULL on failure
 * @note                The returned Subject is `LV_SUBJECT_MODE_EAGER`, so it records
 *                      every value `source` passes through rather than only the ones
 *                      somebody happened to read. A lazy running extremum would miss
 *                      values, which is why this is not configurable.
 * @note                The helper owns the returned Subject's user data. Do not call
 *                      `lv_subject_set_user_data()` on it.
 */
lv_subject_t * lv_subject_create_min(lv_subject_t * source, lv_subject_compare_cb_t compare_cb);

/**
 * Create a Subject that holds the largest value another Subject has taken so far.
 * See @ref lv_subject_create_min.
 * @param source        the Subject to watch
 * @param compare_cb    how to order two values @nullable
 * @return              the new Subject, or NULL on failure
 */
lv_subject_t * lv_subject_create_max(lv_subject_t * source, lv_subject_compare_cb_t compare_cb);

/**
 * Create a Subject that mirrors another one, bounded to a range.
 *
 * The returned Subject has the same value type as `source` and depends on it, so it
 * reports `source`'s value pulled inside the bounds whenever `source` changes.
 *
 * **The bounds can be changed at run time.** The returned Subject's *input* type is
 * `LV_SUBJECT_TYPE_POINTER` and it accepts an @ref lv_subject_range_t, so writing a new
 * range re-clamps immediately:
 *
 * ```c
 * lv_subject_value_t lo = { .num = 1 };
 * lv_subject_value_t hi = { .num = 25 };
 * lv_subject_t * bounded = lv_subject_create_clamped(reading, lo, hi, NULL);
 *
 * lv_subject_set_int(reading, 30);              // bounded reports 25
 *
 * lv_subject_range_t wider = { .min_value = { .num = 1 }, .max_value = { .num = 100 } };
 * lv_subject_set_pointer(bounded, &wider);      // bounded reports 30
 *
 * lv_subject_range_t higher = { .min_value = { .num = 50 }, .max_value = { .num = 100 } };
 * lv_subject_set_pointer(bounded, &higher);     // bounded reports 50
 * ```
 *
 * Each of those writes notifies, because the clamped value really changed. Widening the
 * range restores the reading it had clamped away, because the bound is re-applied to
 * `source`'s current value rather than to what was stored.
 *
 * The range is a retained pointer input, so **it has to stay valid until a new range is
 * written**: `source` changing re-runs the mapper against it. A static is the normal way
 * to hold one.
 *
 * @param source        the Subject to follow
 * @param min_value     initial lower bound, read through the member matching `source`'s type
 * @param max_value     initial upper bound
 * @param compare_cb    how to order two values. @nullable Pass NULL to use the natural
 *                      ordering, available for `LV_SUBJECT_TYPE_INT`,
 *                      `LV_SUBJECT_TYPE_FLOAT` and `LV_SUBJECT_TYPE_COLOR`. Any other
 *                      type needs one.
 * @return              the new Subject, or NULL on failure
 * @note                This bounds a *separate* Subject and leaves `source` alone. To
 *                      bound a Subject in place, so that a two-way Widget binding
 *                      round-trips through clamped values, give that Subject a clamping
 *                      mapper instead.
 */
lv_subject_t * lv_subject_create_clamped(lv_subject_t * source, lv_subject_value_t min_value,
                                         lv_subject_value_t max_value, lv_subject_compare_cb_t compare_cb);

/**
 * Ready-made `lv_subject_compare_cb_t` for an integer Subject.
 * @param subject   the Subject being evaluated
 * @param a         one value
 * @param b         the other value
 * @return          negative, 0 or positive as `a.num` orders against `b.num`
 */
int lv_subject_compare_int(lv_subject_t * subject, lv_subject_value_t a, lv_subject_value_t b);

#if LV_USE_FLOAT
/**
 * Ready-made `lv_subject_compare_cb_t` for a float Subject.
 * @param subject   the Subject being evaluated
 * @param a         one value
 * @param b         the other value
 * @return          negative, 0 or positive as `a.float_v` orders against `b.float_v`
 */
int lv_subject_compare_float(lv_subject_t * subject, lv_subject_value_t a, lv_subject_value_t b);
#endif

/**
 * Ready-made `lv_subject_compare_cb_t` for a color Subject, ordering by the packed
 * 32-bit value.
 * @param subject   the Subject being evaluated
 * @param a         one value
 * @param b         the other value
 * @return          negative, 0 or positive
 */
int lv_subject_compare_color(lv_subject_t * subject, lv_subject_value_t a, lv_subject_value_t b);

/**
 * Clamp an integer between two bounds. A convenience for writing a clamping mapper.
 * @param value     the value to bound
 * @param min_value lower bound
 * @param max_value upper bound
 * @return          `value` pulled inside `[min_value, max_value]`
 */
int32_t lv_subject_clamp_int(int32_t value, int32_t min_value, int32_t max_value);

#if LV_USE_FLOAT
/**
 * Clamp a float between two bounds. A convenience for writing a clamping mapper.
 * @param value     the value to bound
 * @param min_value lower bound
 * @param max_value upper bound
 * @return          `value` pulled inside `[min_value, max_value]`
 */
float lv_subject_clamp_float(float value, float min_value, float max_value);
#endif

/**
 * Add Observer to Subject. When Subject's value changes `observer_cb` will be called.
 * @param subject       pointer to Subject
 * @param observer_cb   notification callback
 * @param user_data     optional user data @nullable
 * @return              pointer to newly-created Observer
 */
lv_observer_t * lv_subject_add_observer(lv_subject_t * subject, lv_observer_cb_t observer_cb, void * user_data);

/**
 * Add Observer to Subject for a Widget.
 * When the Widget is deleted, Observer will be unsubscribed from Subject automatically.
 * @param subject       pointer to Subject
 * @param observer_cb   notification callback
 * @param obj           pointer to Widget. @nullable When NULL the Observer is not
 *                      bound to a Widget.
 * @param user_data     optional user data @nullable
 * @return              pointer to newly-created Observer
 * @note                Do not call `lv_observer_delete()` on Observers created this way.
 *                      Only clean up such Observers by either:
 *                      - deleting the Widget, or
 *                      - calling `lv_subject_deinit()` to gracefully de-couple and
 *                        remove all Observers.
 */
lv_observer_t * lv_subject_add_observer_obj(lv_subject_t * subject, lv_observer_cb_t observer_cb, lv_obj_t * obj,
                                            void * user_data);

/**
 * Add an Observer to a Subject and also save a target pointer.
 * @param subject       pointer to Subject
 * @param observer_cb   notification callback
 * @param target        any pointer @nullable
 * @param user_data     optional user data @nullable
 * @return              pointer to newly-created Observer
 */
lv_observer_t * lv_subject_add_observer_with_target(lv_subject_t * subject, lv_observer_cb_t observer_cb,
                                                    void * target, void * user_data);

/**
 * Remove Observer from its Subject.
 * @param observer      pointer to Observer @nullable
 */
void lv_observer_delete(lv_observer_t * observer);

/**
 * Remove Observers associated with Widget `obj` from specified `subject` or all Subjects.
 * @param obj       pointer to Widget whose Observers should be removed
 * @param subject   Subject to remove Widget from. @nullable When NULL the Widget is
 *                  removed from all Subjects.
 * @note            This function can be used e.g. when a Widget's Subject(s) needs to
 *                  be replaced by other Subject(s)
 */
void lv_obj_remove_from_subject(lv_obj_t * obj, lv_subject_t * subject);

/**
 * Get target of an Observer.
 * @param observer      pointer to Observer
 * @return              pointer to saved target
 */
void * lv_observer_get_target(lv_observer_t * observer);

/**
 * Get target Widget of Observer.
 * This is the same as `lv_observer_get_target()`, except it returns `target`
 * as an `lv_obj_t *`.
 * @param observer      pointer to Observer
 * @return              pointer to saved Widget target
 */
lv_obj_t * lv_observer_get_target_obj(lv_observer_t * observer);

/**
 * Get Observer's user data.
 * @param observer      pointer to Observer
 * @return              void pointer to saved user data
*/
void * lv_observer_get_user_data(const lv_observer_t * observer);

#if LV_USE_EXT_DATA

/**
 * @brief Attaches external user data to an integer Subject with lifecycle management
 *
 * Associates arbitrary user-defined data with an LVGL observer and registers a destructor
 * callback that will be automatically invoked when the observer is deleted. This enables:
 * - Safe resource cleanup through the destructor mechanism
 * - Contextual data storage for observer callbacks
 * - Proper memory management for observer-related resources
 *
 * @param subject    pointer to Subject
 * @param data       User-defined data pointer to associate @nullable
 * @param free_cb    Cleanup function called when: @nullable
 *                   - Observer is explicitly deleted
 *                   - Observed object is deleted
 *                   - New data replaces current association
 *                   NULL indicates no cleanup required
 */
void lv_subject_set_external_data(lv_subject_t * subject, void * data, void (* free_cb)(void * data));
#endif

/**
 * Set Observer's user data.
 * @param observer      pointer to Observer
 * @param user_data     pointer to user-owned data (may be NULL) @nullable
 */
void lv_observer_set_user_data(lv_observer_t * observer, void * user_data);

/**
 * Get the Subject an Observer is subscribed to.
 * This is what an Observer's mapper uses to read the observed value.
 * @param observer      pointer to Observer
 * @return              the observed Subject
 */
lv_subject_t * lv_observer_get_subject(const lv_observer_t * observer);

/**
 * Set when an Observer is notified.
 * @param observer      pointer to Observer
 * @param mode          `LV_OBSERVER_MODE_IMMEDIATE` (the default) or
 *                      `LV_OBSERVER_MODE_BATCHED`
 * @note                Switching the last immediate Observer of a Subject to batched
 *                      lets that Subject fall back to its own declared mode.
 */
void lv_observer_set_mode(lv_observer_t * observer, lv_observer_mode_t mode);

/**
 * Get an Observer's mode.
 * @param observer      pointer to Observer
 * @return              the Observer's mode
 */
lv_observer_mode_t lv_observer_get_mode(const lv_observer_t * observer);

/**
 * Notify all Observers of Subject.
 * @param subject       pointer to Subject
 */
void lv_subject_notify(lv_subject_t * subject);

/**
 * Add an event handler to increment (or decrement) the value of a subject on a trigger.
 * @param obj       pointer to a widget
 * @param subject   pointer to a subject to change
 * @param trigger   the trigger on which the subject should be changed
 * @param step      value to add on trigger
 *                  if the minimum value is reached, the maximum value will be set on rollover.
 */
lv_subject_increment_dsc_t * lv_obj_add_subject_increment_event(lv_obj_t * obj, lv_subject_t * subject,
                                                                lv_event_code_t trigger, int32_t step);

/**
 * Set the minimum subject value to set by the event
 * @param obj           pointer to the Widget to which the event is attached
 * @param dsc           pointer to the descriptor returned by `lv_obj_add_subject_increment_event()`
 * @param min_value     the minimum value to set
 */
void lv_obj_set_subject_increment_event_min_value(lv_obj_t * obj, lv_subject_increment_dsc_t * dsc, int32_t min_value);

/**
 * Set the maximum subject value to set by the event
 * @param obj           pointer to the Widget to which the event is attached
 * @param dsc           pointer to the descriptor returned by `lv_obj_add_subject_increment_event()`
 * @param max_value     the maximum value to set
 */
void lv_obj_set_subject_increment_event_max_value(lv_obj_t * obj, lv_subject_increment_dsc_t * dsc, int32_t max_value);

/**
 * Set what to do when the min/max value is crossed.
 * @param obj           pointer to the Widget to which the event is attached
 * @param dsc           pointer to the descriptor returned by `lv_obj_add_subject_increment_event()`
 * @param rollover      false: stop at the min/max value; true: jump to the other end
 * @note                the subject's mapper, if it has one, is applied on top of this range
 */
void lv_obj_set_subject_increment_event_rollover(lv_obj_t * obj, lv_subject_increment_dsc_t * dsc, bool rollover);

/**
* Toggle the value of an integer subject on an event. If it was != 0 it will be 0.
* If it was 0, it will be 1.
* @param obj       pointer to a widget
* @param subject   pointer to a subject to toggle
* @param trigger   the trigger on which the subject should be changed
*/
void lv_obj_add_subject_toggle_event(lv_obj_t * obj, lv_subject_t * subject, lv_event_code_t trigger);

/**
 * Set the value of an integer subject.
 * @param obj       pointer to a widget
 * @param subject   pointer to a subject to change
 * @param trigger   the trigger on which the subject should be changed
 * @param value     the value to set
 */
void lv_obj_add_subject_set_int_event(lv_obj_t * obj, lv_subject_t * subject, lv_event_code_t trigger, int32_t value);


#if LV_USE_FLOAT
/**
 * Set the value of a float subject.
 * @param obj       pointer to a widget
 * @param subject   pointer to a subject to change
 * @param trigger   the trigger on which the subject should be changed
 * @param value     the value to set
 */
void lv_obj_add_subject_set_float_event(lv_obj_t * obj, lv_subject_t * subject, lv_event_code_t trigger, float value);
#endif

/**
 * Set the value of a string subject.
 * @param obj       pointer to a widget
 * @param subject   pointer to a subject to change
 * @param trigger   the trigger on which the subject should be changed
 * @param value     the value to set
 */
void lv_obj_add_subject_set_string_event(lv_obj_t * obj, lv_subject_t * subject, lv_event_code_t trigger,
                                         const char * value);

/**
 * Bind a boolean value to a Widget: `set_bool_cb` is called with the Subject's
 * value (as a `bool`) on subscribing and whenever it changes. A dedicated per-flag
 * setter such as `lv_obj_set_hidden` can be passed directly.
 * @param obj           pointer to Widget
 * @param subject       pointer to an integer Subject
 * @param set_bool_cb   callback that applies the boolean value to the Widget
 * @return              pointer to newly-created Observer
 */
lv_observer_t * lv_obj_bind_bool(lv_obj_t * obj, lv_subject_t * subject, lv_obj_set_bool_t set_bool_cb);

/**
 * Bind an integer value to a Widget: `set_int_cb` is called with the Subject's
 * value on subscribing and whenever it changes.
 * @param obj           pointer to Widget
 * @param subject       pointer to an integer Subject
 * @param set_int_cb    callback that applies the integer value to the Widget
 * @return              pointer to newly-created Observer
 */
lv_observer_t * lv_obj_bind_int(lv_obj_t * obj, lv_subject_t * subject, lv_obj_set_int_t set_int_cb);

#if LV_USE_FLOAT
/**
 * Bind a float value to a Widget: `set_float_cb` is called with the Subject's
 * value on subscribing and whenever it changes.
 * @param obj           pointer to Widget
 * @param subject       pointer to a float Subject
 * @param set_float_cb  callback that applies the float value to the Widget
 * @return              pointer to newly-created Observer
 */
lv_observer_t * lv_obj_bind_float(lv_obj_t * obj, lv_subject_t * subject, lv_obj_set_float_t set_float_cb);
#endif

/**
 * Bind a string value to a Widget: `set_string_cb` is called with the Subject's
 * value on subscribing and whenever it changes.
 * @param obj           pointer to Widget
 * @param subject       pointer to a string Subject
 * @param set_string_cb callback that applies the string value to the Widget
 * @return              pointer to newly-created Observer
 */
lv_observer_t * lv_obj_bind_string(lv_obj_t * obj, lv_subject_t * subject, lv_obj_set_string_t set_string_cb);

/**
 * Bind a color value to a Widget: `set_color_cb` is called with the Subject's
 * value on subscribing and whenever it changes.
 * @param obj           pointer to Widget
 * @param subject       pointer to a color Subject
 * @param set_color_cb  callback that applies the color value to the Widget
 * @return              pointer to newly-created Observer
 */
lv_observer_t * lv_obj_bind_color(lv_obj_t * obj, lv_subject_t * subject, lv_obj_set_color_t set_color_cb);

/**
 * Bind a pointer value to a Widget: `set_pointer_cb` is called with the Subject's
 * value on subscribing and whenever it changes.
 * @param obj            pointer to Widget
 * @param subject        pointer to a pointer Subject
 * @param set_pointer_cb callback that applies the pointer value to the Widget
 * @return               pointer to newly-created Observer
 */
lv_observer_t * lv_obj_bind_pointer(lv_obj_t * obj, lv_subject_t * subject, lv_obj_set_pointer_t set_pointer_cb);

/**
 * Bind a Subject of any type to a Widget through a mapper that produces a boolean.
 *
 * The plain `lv_obj_bind_bool()` requires the Subject to be an integer one. This
 * variant drops that requirement: `mapper` reads the Subject however it likes and
 * produces the boolean to apply, so e.g. a string Subject can drive
 * `lv_obj_set_hidden`. When `mapper` returns `false` the Widget is not touched.
 *
 * @param obj           pointer to Widget
 * @param subject       pointer to a Subject of any type
 * @param set_bool_cb   callback that applies the boolean value to the Widget
 * @param mapper        maps the Subject's value to the boolean to apply
 * @param user_data     passed to the mapper on every call @nullable
 * @return              pointer to newly-created Observer
 * @note                The mapper has to be given here rather than set afterwards,
 *                      because binding notifies the Observer immediately.
 */
/**
 * Bind an integer Subject to a style property.
 *
 * A style setter takes a selector as well as a value, so it does not fit
 * `lv_obj_bind_int()`. This passes the selector along:
 *
 * ```c
 * lv_obj_bind_style_int(box, subject_pad, lv_obj_set_style_pad_all, 0);
 * lv_obj_bind_style_int(box, subject_w, lv_obj_set_style_width, LV_PART_INDICATOR);
 * ```
 *
 * @param obj       pointer to Widget
 * @param subject   pointer to an integer Subject
 * @param setter    the style setter to call
 * @param selector  part and state, as passed to any style setter
 * @return          pointer to newly-created Observer
 */
lv_observer_t * lv_obj_bind_style_int(lv_obj_t * obj, lv_subject_t * subject, lv_obj_set_style_int_t setter,
                                      lv_style_selector_t selector);

/**
 * Bind a color Subject to a style property. See @ref lv_obj_bind_style_int.
 * @param obj       pointer to Widget
 * @param subject   pointer to a color Subject
 * @param setter    the style setter to call
 * @param selector  part and state
 * @return          pointer to newly-created Observer
 */
lv_observer_t * lv_obj_bind_style_color(lv_obj_t * obj, lv_subject_t * subject, lv_obj_set_style_color_t setter,
                                        lv_style_selector_t selector);

/**
 * Bind an integer Subject to an opacity style property, clamped to 0..255.
 * See @ref lv_obj_bind_style_int.
 * @param obj       pointer to Widget
 * @param subject   pointer to an integer Subject
 * @param setter    the style setter to call
 * @param selector  part and state
 * @return          pointer to newly-created Observer
 */
lv_observer_t * lv_obj_bind_style_opa(lv_obj_t * obj, lv_subject_t * subject, lv_obj_set_style_opa_t setter,
                                      lv_style_selector_t selector);

/**
 * Bind a Subject of any type to a style property through a mapper that produces an
 * integer. See @ref lv_obj_bind_style_int and @ref lv_obj_bind_bool_mapped.
 * @param obj       pointer to Widget
 * @param subject   pointer to a Subject of any type
 * @param setter    the style setter to call
 * @param selector  part and state
 * @param mapper    maps the Subject's value to the integer to apply
 * @param user_data passed to the mapper on every call @nullable
 * @return          pointer to newly-created Observer
 */
lv_observer_t * lv_obj_bind_style_int_mapped(lv_obj_t * obj, lv_subject_t * subject,
                                             lv_obj_set_style_int_t setter, lv_style_selector_t selector,
                                             lv_observer_int_mapper_t mapper, void * user_data);

/**
 * Bind a Subject of any type to a style property through a mapper that produces a color.
 * See @ref lv_obj_bind_style_int_mapped.
 * @param obj       pointer to Widget
 * @param subject   pointer to a Subject of any type
 * @param setter    the style setter to call
 * @param selector  part and state
 * @param mapper    maps the Subject's value to the color to apply
 * @param user_data passed to the mapper on every call @nullable
 * @return          pointer to newly-created Observer
 */
lv_observer_t * lv_obj_bind_style_color_mapped(lv_obj_t * obj, lv_subject_t * subject,
                                               lv_obj_set_style_color_t setter, lv_style_selector_t selector,
                                               lv_observer_color_mapper_t mapper, void * user_data);

/**
 * Bind a Subject of any type to an opacity style property through a mapper. The mapper's
 * integer output is bounded to 0..255. See @ref lv_obj_bind_style_int_mapped.
 * @param obj       pointer to Widget
 * @param subject   pointer to a Subject of any type
 * @param setter    the style setter to call
 * @param selector  part and state
 * @param mapper    maps the Subject's value to the opacity to apply
 * @param user_data passed to the mapper on every call @nullable
 * @return          pointer to newly-created Observer
 */
lv_observer_t * lv_obj_bind_style_opa_mapped(lv_obj_t * obj, lv_subject_t * subject,
                                             lv_obj_set_style_opa_t setter, lv_style_selector_t selector,
                                             lv_observer_int_mapper_t mapper, void * user_data);

lv_observer_t * lv_obj_bind_bool_mapped(lv_obj_t * obj, lv_subject_t * subject, lv_obj_set_bool_t set_bool_cb,
                                        lv_observer_bool_mapper_t mapper, void * user_data);

/**
 * Bind a Subject of any type to a Widget through a mapper that produces an integer.
 * See @ref lv_obj_bind_bool_mapped.
 * @param obj           pointer to Widget
 * @param subject       pointer to a Subject of any type
 * @param set_int_cb    callback that applies the integer value to the Widget
 * @param mapper        maps the Subject's value to the integer to apply
 * @return              pointer to newly-created Observer
 */
lv_observer_t * lv_obj_bind_int_mapped(lv_obj_t * obj, lv_subject_t * subject, lv_obj_set_int_t set_int_cb,
                                       lv_observer_int_mapper_t mapper, void * user_data);

#if LV_USE_FLOAT
/**
 * Bind a Subject of any type to a Widget through a mapper that produces a float.
 * See @ref lv_obj_bind_bool_mapped.
 * @param obj           pointer to Widget
 * @param subject       pointer to a Subject of any type
 * @param set_float_cb  callback that applies the float value to the Widget
 * @param mapper        maps the Subject's value to the float to apply
 * @param user_data     passed to the mapper on every call @nullable
 * @return              pointer to newly-created Observer
 */
lv_observer_t * lv_obj_bind_float_mapped(lv_obj_t * obj, lv_subject_t * subject, lv_obj_set_float_t set_float_cb,
                                         lv_observer_float_mapper_t mapper, void * user_data);
#endif

/**
 * Bind a Subject of any type to a Widget through a mapper that produces a string.
 * This is what lets an integer Subject drive `lv_label_set_text` directly.
 * See @ref lv_obj_bind_bool_mapped.
 * @param obj           pointer to Widget
 * @param subject       pointer to a Subject of any type
 * @param set_string_cb callback that applies the string to the Widget
 * @param mapper        maps the Subject's value to the string to apply
 * @param user_data     passed to the mapper on every call @nullable
 * @return              pointer to newly-created Observer
 * @note                The mapper owns the string storage; the Observer only keeps
 *                      the pointer the mapper hands it.
 */
lv_observer_t * lv_obj_bind_string_mapped(lv_obj_t * obj, lv_subject_t * subject, lv_obj_set_string_t set_string_cb,
                                          lv_observer_string_mapper_t mapper, void * user_data);

/**
 * Bind a Subject of any type to a Widget through a mapper that produces a color.
 * See @ref lv_obj_bind_bool_mapped.
 * @param obj           pointer to Widget
 * @param subject       pointer to a Subject of any type
 * @param set_color_cb  callback that applies the color to the Widget
 * @param mapper        maps the Subject's value to the color to apply
 * @param user_data     passed to the mapper on every call @nullable
 * @return              pointer to newly-created Observer
 */
lv_observer_t * lv_obj_bind_color_mapped(lv_obj_t * obj, lv_subject_t * subject, lv_obj_set_color_t set_color_cb,
                                         lv_observer_color_mapper_t mapper, void * user_data);

/**
 * Bind a Subject of any type to a Widget through a mapper that produces a pointer.
 * See @ref lv_obj_bind_bool_mapped.
 * @param obj            pointer to Widget
 * @param subject        pointer to a Subject of any type
 * @param set_pointer_cb callback that applies the pointer to the Widget
 * @param mapper         maps the Subject's value to the pointer to apply
 * @return               pointer to newly-created Observer
 */
lv_observer_t * lv_obj_bind_pointer_mapped(lv_obj_t * obj, lv_subject_t * subject, lv_obj_set_pointer_t set_pointer_cb,
                                           lv_observer_pointer_mapper_t mapper, void * user_data);

/**
 * Set Widget's flag(s) if an integer Subject's value is equal to a reference value, clear flag otherwise.
 * @param obj           pointer to Widget
 * @param subject       pointer to Subject
 * @param flag          flag(s) (can be bit-wise OR-ed) to set or clear (e.g. `LV_OBJ_FLAG_HIDDEN`)
 * @param ref_value     reference value to compare Subject's value with
 * @return              pointer to newly-created Observer
 * @deprecated Use `lv_obj_bind_bool()` or `lv_subject_add_observer_obj()` instead.
 */
LV_DEPRECATED("Use `lv_obj_bind_bool()` or `lv_subject_add_observer_obj()` instead.")
lv_observer_t * lv_obj_bind_flag_if_eq(lv_obj_t * obj, lv_subject_t * subject, lv_obj_flag_t flag, int32_t ref_value);

/**
 * Set Widget's flag(s) if an integer Subject's value is not equal to a reference value, clear flag otherwise.
 * @param obj           pointer to Widget
 * @param subject       pointer to Subject
 * @param flag          flag(s) (can be bit-wise OR-ed) to set or clear (e.g. `LV_OBJ_FLAG_HIDDEN`)
 * @param ref_value     reference value to compare Subject's value with
 * @return              pointer to newly-created Observer
 * @deprecated Use `lv_obj_bind_bool()` or `lv_subject_add_observer_obj()` instead.
 */
LV_DEPRECATED("Use `lv_obj_bind_bool()` or `lv_subject_add_observer_obj()` instead.")
lv_observer_t * lv_obj_bind_flag_if_not_eq(lv_obj_t * obj, lv_subject_t * subject, lv_obj_flag_t flag,
                                           int32_t ref_value);

/**
 * Set Widget's flag(s) if an integer Subject's value is greater than a reference value, clear flag otherwise.
 * @param obj           pointer to Widget
 * @param subject       pointer to Subject
 * @param flag          flag(s) (can be bit-wise OR-ed) to set or clear (e.g. `LV_OBJ_FLAG_HIDDEN`)
 * @param ref_value     reference value to compare Subject's value with
 * @return              pointer to newly-created Observer
 * @deprecated Use `lv_obj_bind_bool()` or `lv_subject_add_observer_obj()` instead.
 */
LV_DEPRECATED("Use `lv_obj_bind_bool()` or `lv_subject_add_observer_obj()` instead.")
lv_observer_t * lv_obj_bind_flag_if_gt(lv_obj_t * obj, lv_subject_t * subject, lv_obj_flag_t flag, int32_t ref_value);

/**
 * Set Widget's flag(s) if an integer Subject's value is greater than or equal to a reference value, clear flag otherwise.
 * @param obj           pointer to Widget
 * @param subject       pointer to Subject
 * @param flag          flag(s) (can be bit-wise OR-ed) to set or clear (e.g. `LV_OBJ_FLAG_HIDDEN`)
 * @param ref_value     reference value to compare Subject's value with
 * @return              pointer to newly-created Observer
 * @deprecated Use `lv_obj_bind_bool()` or `lv_subject_add_observer_obj()` instead.
 */
LV_DEPRECATED("Use `lv_obj_bind_bool()` or `lv_subject_add_observer_obj()` instead.")
lv_observer_t * lv_obj_bind_flag_if_ge(lv_obj_t * obj, lv_subject_t * subject, lv_obj_flag_t flag, int32_t ref_value);

/**
 * Set Widget's flag(s) if an integer Subject's value is less than a reference value, clear flag otherwise.
 * @param obj           pointer to Widget
 * @param subject       pointer to Subject
 * @param flag          flag(s) (can be bit-wise OR-ed) to set or clear (e.g. `LV_OBJ_FLAG_HIDDEN`)
 * @param ref_value     reference value to compare Subject's value with
 * @return              pointer to newly-created Observer
 * @deprecated Use `lv_obj_bind_bool()` or `lv_subject_add_observer_obj()` instead.
 */
LV_DEPRECATED("Use `lv_obj_bind_bool()` or `lv_subject_add_observer_obj()` instead.")
lv_observer_t * lv_obj_bind_flag_if_lt(lv_obj_t * obj, lv_subject_t * subject, lv_obj_flag_t flag, int32_t ref_value);

/**
 * Set Widget's flag(s) if an integer Subject's value is less than or equal to a reference value, clear flag otherwise.
 * @param obj           pointer to Widget
 * @param subject       pointer to Subject
 * @param flag          flag(s) (can be bit-wise OR-ed) to set or clear (e.g. `LV_OBJ_FLAG_HIDDEN`)
 * @param ref_value     reference value to compare Subject's value with
 * @return              pointer to newly-created Observer
 * @deprecated Use `lv_obj_bind_bool()` or `lv_subject_add_observer_obj()` instead.
 */
LV_DEPRECATED("Use `lv_obj_bind_bool()` or `lv_subject_add_observer_obj()` instead.")
lv_observer_t * lv_obj_bind_flag_if_le(lv_obj_t * obj, lv_subject_t * subject, lv_obj_flag_t flag, int32_t ref_value);


/**
 * Set Widget's state(s) if an integer Subject's value is equal to a reference value, clear flag otherwise.
 * @param obj           pointer to Widget
 * @param subject       pointer to Subject
 * @param state         state(s) (can be bit-wise OR-ed) to set or clear (e.g. `LV_STATE_CHECKED`)
 * @param ref_value     reference value to compare Subject's value with
 * @return              pointer to newly-created Observer
 * @deprecated Use `lv_obj_bind_bool()` or `lv_subject_add_observer_obj()` instead.
 */
LV_DEPRECATED("Use `lv_obj_bind_bool()` or `lv_subject_add_observer_obj()` instead.")
lv_observer_t * lv_obj_bind_state_if_eq(lv_obj_t * obj, lv_subject_t * subject, lv_state_t state, int32_t ref_value);

/**
 * Set a Widget's state(s) if an integer Subject's value is not equal to a reference value, clear flag otherwise
 * @param obj           pointer to Widget
 * @param subject       pointer to Subject
 * @param state         state(s) (can be bit-wise OR-ed) to set or clear (e.g. `LV_STATE_CHECKED`)
 * @param ref_value     reference value to compare Subject's value with
 * @return              pointer to newly-created Observer
 * @deprecated Use `lv_obj_bind_bool()` or `lv_subject_add_observer_obj()` instead.
 */
LV_DEPRECATED("Use `lv_obj_bind_bool()` or `lv_subject_add_observer_obj()` instead.")
lv_observer_t * lv_obj_bind_state_if_not_eq(lv_obj_t * obj, lv_subject_t * subject, lv_state_t state,
                                            int32_t ref_value);

/**
 * Set Widget's state(s) if an integer Subject's value is greater than a reference value, clear flag otherwise.
 * @param obj           pointer to Widget
 * @param subject       pointer to Subject
 * @param state         state(s) (can be bit-wise OR-ed) to set or clear (e.g. `LV_STATE_CHECKED`)
 * @param ref_value     reference value to compare Subject's value with
 * @return              pointer to newly-created Observer
 * @deprecated Use `lv_obj_bind_bool()` or `lv_subject_add_observer_obj()` instead.
 */
LV_DEPRECATED("Use `lv_obj_bind_bool()` or `lv_subject_add_observer_obj()` instead.")
lv_observer_t * lv_obj_bind_state_if_gt(lv_obj_t * obj, lv_subject_t * subject, lv_state_t state, int32_t ref_value);

/**
 * Set Widget's state(s) if an integer Subject's value is greater than or equal to a reference value, clear flag otherwise.
 * @param obj           pointer to Widget
 * @param subject       pointer to Subject
 * @param state         state(s) (can be bit-wise OR-ed) to set or clear (e.g. `LV_STATE_CHECKED`)
 * @param ref_value     reference value to compare Subject's value with
 * @return              pointer to newly-created Observer
 * @deprecated Use `lv_obj_bind_bool()` or `lv_subject_add_observer_obj()` instead.
 */
LV_DEPRECATED("Use `lv_obj_bind_bool()` or `lv_subject_add_observer_obj()` instead.")
lv_observer_t * lv_obj_bind_state_if_ge(lv_obj_t * obj, lv_subject_t * subject, lv_state_t state, int32_t ref_value);

/**
 * Set Widget's state(s) if an integer Subject's value is less than a reference value, clear flag otherwise.
 * @param obj           pointer to Widget
 * @param subject       pointer to Subject
 * @param state         state(s) (can be bit-wise OR-ed) to set or clear (e.g. `LV_STATE_CHECKED`)
 * @param ref_value     reference value to compare Subject's value with
 * @return              pointer to newly-created Observer
 * @deprecated Use `lv_obj_bind_bool()` or `lv_subject_add_observer_obj()` instead.
 */
LV_DEPRECATED("Use `lv_obj_bind_bool()` or `lv_subject_add_observer_obj()` instead.")
lv_observer_t * lv_obj_bind_state_if_lt(lv_obj_t * obj, lv_subject_t * subject, lv_state_t state, int32_t ref_value);

/**
 * Set Widget's state(s) if an integer Subject's value is less than or equal to a reference value, clear flag otherwise.
 * @param obj           pointer to Widget
 * @param subject       pointer to Subject
 * @param state         state(s) (can be bit-wise OR-ed) to set or clear (e.g. `LV_STATE_CHECKED`)
 * @param ref_value     reference value to compare Subject's value with
 * @return              pointer to newly-created Observer
 * @deprecated Use `lv_obj_bind_bool()` or `lv_subject_add_observer_obj()` instead.
 */
LV_DEPRECATED("Use `lv_obj_bind_bool()` or `lv_subject_add_observer_obj()` instead.")
lv_observer_t * lv_obj_bind_state_if_le(lv_obj_t * obj, lv_subject_t * subject, lv_state_t state, int32_t ref_value);

/**
 * Set an integer Subject to 1 when a Widget is checked and set it 0 when unchecked, and
 * clear Widget's checked state when Subject's value changes to 0 and set it when non-zero.
 * @param obj       pointer to Widget
 * @param subject   pointer to a Subject
 * @return          pointer to newly-created Observer
 * @note            Ensure Widget's `LV_OBJ_FLAG_CHECKABLE` flag is set.
 */
lv_observer_t * lv_obj_bind_checked(lv_obj_t * obj, lv_subject_t * subject);


/**********************
 *      MACROS
 **********************/

/**
 * Define an Observer callback that forwards a Subject's value straight to a Widget
 * setter, passing any extra arguments the setter needs.
 *
 * `lv_obj_bind_int()` and friends already forward to a setter shaped
 * `void (lv_obj_t *, value)`. Plenty of LVGL setters take more than that — an animation
 * flag, a style selector — and these macros bridge the difference without writing the
 * callback out by hand. They expand to a `static` function, so use them at file scope:
 *
 * ```c
 * LV_SUBJECT_FORWARD_INT(set_slider, lv_slider_set_value, LV_ANIM_OFF)
 * LV_SUBJECT_FORWARD_COLOR(set_border, lv_obj_set_style_border_color, LV_PART_MAIN)
 *
 * void my_ui(void)
 * {
 *     lv_subject_add_observer_obj(subject_value, set_slider, slider, NULL);
 *     lv_subject_add_observer_obj(subject_color, set_border, box, NULL);
 * }
 * ```
 *
 * The result is type-checked by the compiler and costs nothing at run time: the
 * generated function calls the setter directly.
 *
 * @param name      name for the generated Observer callback
 * @param setter    the Widget setter to call
 * @param ...       the extra arguments to pass after the value. At least one is
 *                  required; with none, use `lv_obj_bind_int()` instead.
 */
#define LV_SUBJECT_FORWARD_INT(name, setter, ...)                                    \
    static void name(lv_observer_t * observer, lv_subject_t * subject)               \
    {                                                                                \
        setter(lv_observer_get_target_obj(observer), lv_subject_get_int(subject),     \
               __VA_ARGS__);                                                         \
    }

/**
 * Like @ref LV_SUBJECT_FORWARD_INT, for a Subject whose value is a string.
 * @param name      name for the generated Observer callback
 * @param setter    the Widget setter to call
 * @param ...       extra arguments passed after the value
 */
#define LV_SUBJECT_FORWARD_STRING(name, setter, ...)                                 \
    static void name(lv_observer_t * observer, lv_subject_t * subject)               \
    {                                                                                \
        setter(lv_observer_get_target_obj(observer), lv_subject_get_string(subject),  \
               __VA_ARGS__);                                                         \
    }

/**
 * Like @ref LV_SUBJECT_FORWARD_INT, for a Subject whose value is a color.
 * @param name      name for the generated Observer callback
 * @param setter    the Widget setter to call
 * @param ...       extra arguments passed after the value
 */
#define LV_SUBJECT_FORWARD_COLOR(name, setter, ...)                                  \
    static void name(lv_observer_t * observer, lv_subject_t * subject)               \
    {                                                                                \
        setter(lv_observer_get_target_obj(observer), lv_subject_get_color(subject),   \
               __VA_ARGS__);                                                         \
    }

/**
 * Like @ref LV_SUBJECT_FORWARD_INT, for a Subject whose value is a pointer.
 * @param name      name for the generated Observer callback
 * @param setter    the Widget setter to call
 * @param ...       extra arguments passed after the value
 */
#define LV_SUBJECT_FORWARD_POINTER(name, setter, ...)                                \
    static void name(lv_observer_t * observer, lv_subject_t * subject)               \
    {                                                                                \
        setter(lv_observer_get_target_obj(observer), lv_subject_get_pointer(subject), \
               __VA_ARGS__);                                                         \
    }

#if LV_USE_FLOAT
/**
 * Like @ref LV_SUBJECT_FORWARD_INT, for a Subject whose value is a float.
 * @param name      name for the generated Observer callback
 * @param setter    the Widget setter to call
 * @param ...       extra arguments passed after the value
 */
#define LV_SUBJECT_FORWARD_FLOAT(name, setter, ...)                                  \
    static void name(lv_observer_t * observer, lv_subject_t * subject)               \
    {                                                                                \
        setter(lv_observer_get_target_obj(observer), lv_subject_get_float(subject),   \
               __VA_ARGS__);                                                         \
    }
#endif


#endif /*LV_USE_OBSERVER*/

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif /*LV_OBSERVER_H*/
