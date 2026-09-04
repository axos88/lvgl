#include "../../lv_examples.h"
#if LV_USE_OBSERVER && LV_USE_LABEL && LV_BUILD_EXAMPLES

/*Coins dropped into a machine, one write per coin*/
static lv_subject_t * subject_coin;

/*Two totals fed by the same source, differing only in their mode*/
static lv_subject_t * subject_total_lazy;
static lv_subject_t * subject_total_eager;

static bool sum_mapper(lv_subject_t * subject, void * user_data, lv_subject_value_t input, int32_t * value);

/**
 * @title Why a summing mapper needs an eager subject
 * @brief The same accumulating mapper on a lazy and an eager subject, side by side.
 *
 * A mapper that *accumulates* — a running total, a counter, anything that adds to what
 * it already holds — depends on running once per write. A lazy subject does not promise
 * that. It records the last input and runs the mapper when someone reads it, so a run of
 * writes collapses into a single evaluation and every input but the last is never seen.
 *
 * Both subjects below use the identical `sum_mapper`. Five coins are dropped in: 10, 20,
 * 50, 10, 5, which add up to 95.
 *
 * - `subject_total_eager` is `LV_SUBJECT_MODE_EAGER`, so its mapper runs on every write
 *   and the total is **95**.
 * - `subject_total_lazy` is lazy, so its mapper runs once when the label reads it, sees
 *   only the last coin, and reports **5**.
 *
 * The lazy answer is not a bug in the mapper: it is the mapper breaking the contract.
 * A lazy mapper has to be **pure** — a function of its inputs, giving the same answer
 * however many times it runs. `*value += input` is not that, because the answer depends
 * on how often it ran.
 *
 * Two ways out. Declare the subject eager, as here. Or keep it lazy and make the mapper
 * pure by moving the accumulation to the source, so the mapper only ever reads a total
 * somebody else maintains.
 */
void lv_example_observer_13(void)
{
    static bool inited = false;

    if(!inited) {
        subject_coin = lv_subject_create(LV_SUBJECT_TYPE_INT);

        /*Lazy, the default: the mapper runs when the value is read*/
        subject_total_lazy = lv_subject_create(LV_SUBJECT_TYPE_INT);
        lv_subject_set_int_mapper(subject_total_lazy, sum_mapper, NULL);

        /*Eager: the mapper runs inside every lv_subject_set_int() on the coin subject*/
        subject_total_eager = lv_subject_create(LV_SUBJECT_TYPE_INT);
        lv_subject_set_int_mapper(subject_total_eager, sum_mapper, NULL);
        lv_subject_set_mode(subject_total_eager, LV_SUBJECT_MODE_EAGER);

        inited = true;
    }

    lv_obj_t * screen = lv_screen_active();
    lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(screen, 8, 0);
    lv_obj_set_style_pad_all(screen, 12, 0);

    lv_obj_t * label_lazy = lv_label_create(screen);
    lv_obj_t * label_eager = lv_label_create(screen);

    /* 💡 Five coins, one write each. Only the eager total counts them all. */
    static const int32_t coins[] = { 10, 20, 50, 10, 5 };
    for(uint32_t i = 0; i < sizeof(coins) / sizeof(coins[0]); i++) {
        lv_subject_set_int(subject_coin, coins[i]);
    }

    /*Reading the lazy one is what finally runs its mapper, once, on the last coin*/
    lv_label_set_text_fmt(label_lazy, "lazy total:  %" LV_PRId32 "  (wrong)",
                          lv_subject_get_int(subject_total_lazy));
    lv_label_set_text_fmt(label_eager, "eager total: %" LV_PRId32 "  (correct)",
                          lv_subject_get_int(subject_total_eager));

    LV_LOG_USER("five coins of 10+20+50+10+5 = 95");
    LV_LOG_USER("lazy  total: %" LV_PRId32 "  <- saw only the last coin",
                lv_subject_get_int(subject_total_lazy));
    LV_LOG_USER("eager total: %" LV_PRId32 "  <- ran once per coin",
                lv_subject_get_int(subject_total_eager));
}

/*Accumulates, so it is NOT pure: the answer depends on how many times it ran. Safe on an
 *eager subject, wrong on a lazy one.*/
static bool sum_mapper(lv_subject_t * subject, void * user_data, lv_subject_value_t input, int32_t * value)
{
    LV_UNUSED(subject);
    LV_UNUSED(user_data);
    LV_UNUSED(input);

    /*Reading the coin subject is what makes this depend on it*/
    *value += lv_subject_get_int(subject_coin);
    return true;
}

#endif
