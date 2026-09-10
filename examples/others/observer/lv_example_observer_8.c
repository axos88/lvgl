#include "../../lv_examples.h"
#if LV_USE_OBSERVER && LV_USE_LABEL && LV_BUILD_EXAMPLES

/*The raw reading from the sensor, in ADC counts*/
static lv_subject_t * subject_adc;
/*The same reading as a temperature in kelvin, derived from the one above*/
static lv_subject_t * subject_kelvin;

static bool adc_to_kelvin_mapper(lv_subject_t * subject, void * user_data, int32_t * value);
static bool kelvin_to_celsius_text(lv_observer_t * observer, lv_subject_value_t input, const char ** out);
static bool kelvin_to_fahrenheit_text(lv_observer_t * observer, lv_subject_value_t input, const char ** out);

/**
 * @title Mapping a raw sensor value once, then formatting it per subscriber
 * @brief An ADC reading becomes kelvin in one place; two subscribers render C and F.
 *
 * The conversion from ADC counts to kelvin lives in exactly one mapper, on
 * `subject_kelvin`. Nothing else in the program needs to know the sensor's scale.
 *
 * The two labels then subscribe with *observer* mappers, which convert kelvin to
 * Celsius and to Fahrenheit on the way out and hand the label a finished string. So
 * neither label needs a subject of its own, and neither knows how the sensor works.
 *
 * The important property is that every subscriber sees the same kelvin value. If each
 * label had converted from raw ADC counts itself, the two could drift apart the moment
 * one of them was updated and the other was not, or the moment the sensor's scale
 * changed in one place but not the other.
 */
void lv_example_observer_8(void)
{
    static bool inited = false;

    if(!inited) {
        subject_adc = lv_subject_create(LV_SUBJECT_TYPE_INT);

        /*Derived: reading `subject_adc` from the mapper is what makes it a dependency,
         *so `subject_kelvin` re-derives itself whenever a new reading arrives.*/
        subject_kelvin = lv_subject_create(LV_SUBJECT_TYPE_INT);
        lv_subject_set_int_mapper(subject_kelvin, adc_to_kelvin_mapper, NULL);

        inited = true;
    }

    lv_obj_t * screen = lv_screen_active();
    lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(screen, 12, 0);
    lv_obj_set_style_pad_all(screen, 16, 0);

    lv_obj_t * label_k = lv_label_create(screen);
    lv_label_bind_text(label_k, subject_kelvin, "%d K");

    /* 💡 Both labels read the same kelvin subject and only differ in their mapper. */
    lv_obj_t * label_c = lv_label_create(screen);
    lv_obj_bind_string_mapped(label_c, subject_kelvin, lv_label_set_text, kelvin_to_celsius_text, NULL);

    lv_obj_t * label_f = lv_label_create(screen);
    lv_obj_bind_string_mapped(label_f, subject_kelvin, lv_label_set_text, kelvin_to_fahrenheit_text, NULL);

    /*Feed a reading in, as the sensor driver would*/
    lv_subject_set_int(subject_adc, 2048);
}

/*The sensor's scale lives here and nowhere else. 0..4095 counts span 250..350 K.*/
static bool adc_to_kelvin_mapper(lv_subject_t * subject, void * user_data, int32_t * value)
{
    LV_UNUSED(subject);
    LV_UNUSED(user_data);

    int32_t counts = lv_subject_get_int(subject_adc);
    int32_t kelvin = 250 + (counts * 100) / 4095;

    if(kelvin == *value) return false;   /*no change, so nobody is notified*/
    *value = kelvin;
    return true;
}

static bool kelvin_to_celsius_text(lv_observer_t * observer, lv_subject_value_t input, const char ** out)
{
    LV_UNUSED(observer);
    static char buf[32];
    int32_t kelvin = input.num;
    int32_t celsius = kelvin - 273;

    char next[32];
    lv_snprintf(next, sizeof(next), "%" LV_PRId32 " C", celsius);
    if(lv_strcmp(next, buf) == 0) return false;   /*unchanged, leave the label alone*/

    lv_strlcpy(buf, next, sizeof(buf));
    *out = buf;
    LV_LOG_USER("temperature is %" LV_PRId32 " C", celsius);
    return true;
}

static bool kelvin_to_fahrenheit_text(lv_observer_t * observer, lv_subject_value_t input, const char ** out)
{
    LV_UNUSED(observer);
    static char buf[32];
    int32_t kelvin = input.num;
    int32_t fahrenheit = ((kelvin - 273) * 9) / 5 + 32;

    char next[32];
    lv_snprintf(next, sizeof(next), "%" LV_PRId32 " F", fahrenheit);
    if(lv_strcmp(next, buf) == 0) return false;

    lv_strlcpy(buf, next, sizeof(buf));
    *out = buf;
    LV_LOG_USER("temperature is %" LV_PRId32 " F", fahrenheit);
    return true;
}

#endif
