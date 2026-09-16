/* battery.c - cell voltage, and turning it into a number a person can use.
 *
 * Two things here are easy to get wrong and both are worth stating.
 *
 * ADC1, not ADC2.  ADC2 shares its SAR block with the WiFi radio; reads
 * return ESP_ERR_TIMEOUT while WiFi is up.  GPIO4 is ADC1_CH3, so this
 * works whether or not the cover is loading.
 *
 * Voltage is not charge.  A LiPo sits between about 3.7 V and 3.9 V for
 * most of its useful life and then falls off a cliff, so a linear map
 * from 3.3-4.2 V onto 0-100% reads 55% when the cell is nearly full and
 * 40% when it is nearly flat.  The table below is a piecewise fit to a
 * real discharge curve instead.
 */
#include "battery.h"
#include "board_knob.h"

#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_attr.h"
#include "esp_log.h"
#include <stdlib.h>

static const char *TAG = "battery";

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t         s_cali;
static bool                      s_cali_ok;

static uint16_t s_mv;
static uint8_t  s_percent;
static bool     s_valid;

/* Survives deep sleep, which is what makes the deadband below useful:
 * without it the displayed figure would be recomputed from scratch on
 * every wake and jitter by a couple of points each time. */
static RTC_DATA_ATTR uint8_t s_last_percent;
static RTC_DATA_ATTR bool    s_last_valid;

/* --------------------------------------------------------------- init */

void battery_init(void)
{
    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id  = VBAT_ADC_UNIT,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    if (adc_oneshot_new_unit(&unit_cfg, &s_adc) != ESP_OK) {
        ESP_LOGW(TAG, "no ADC unit");
        return;
    }

    adc_oneshot_chan_cfg_t chan_cfg = {
        .bitwidth = ADC_BITWIDTH_DEFAULT,
        .atten    = ADC_ATTEN_DB_12,      /* ~0-3.1 V at the pin */
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc, VBAT_ADC_CHANNEL, &chan_cfg));

    /* Curve fitting rather than line fitting: the S3 stores per-chip
     * calibration in eFuse and the correction is not linear across the
     * range.  If the eFuse is blank this fails and we fall back to the
     * raw counts, which is coarse but not wrong by much at 2 V. */
    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id  = VBAT_ADC_UNIT,
        .chan     = VBAT_ADC_CHANNEL,
        .atten    = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    s_cali_ok = adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_cali) == ESP_OK;
    if (!s_cali_ok) ESP_LOGW(TAG, "no ADC calibration in eFuse, readings are approximate");

    if (s_last_valid) {
        s_percent = s_last_percent;
        s_valid   = true;                 /* something to draw immediately */
    }
}

/* ------------------------------------------------------------- sample */

/* Piecewise LiPo discharge curve, open-circuit, at room temperature.
 * Millivolts against percent, descending. */
static const struct { uint16_t mv; uint8_t pct; } k_curve[] = {
    { 4200, 100 }, { 4100, 90 }, { 4000, 80 }, { 3930, 70 },
    { 3870,  60 }, { 3820, 50 }, { 3790, 40 }, { 3770, 30 },
    { 3740,  20 }, { 3680, 15 }, { 3600, 10 }, { 3450,  5 },
    { 3300,   0 },
};
#define CURVE_N ((int)(sizeof k_curve / sizeof k_curve[0]))

static uint8_t curve_percent(uint16_t mv)
{
    if (mv >= k_curve[0].mv)           return 100;
    if (mv <= k_curve[CURVE_N - 1].mv) return 0;

    for (int i = 1; i < CURVE_N; i++) {
        if (mv >= k_curve[i].mv) {
            int span_mv  = k_curve[i - 1].mv  - k_curve[i].mv;
            int span_pct = k_curve[i - 1].pct - k_curve[i].pct;
            int into     = mv - k_curve[i].mv;
            return (uint8_t)(k_curve[i].pct + (into * span_pct) / span_mv);
        }
    }
    return 0;
}

void battery_sample(void)
{
    if (!s_adc) return;

    int raw = 0;

    /* The first conversions after the mux moves to this channel sample a
     * partly charged hold capacitor, especially with a high-impedance
     * divider.  Throw them away. */
    for (int i = 0; i < VBAT_WARMUP_SAMPLES; i++)
        adc_oneshot_read(s_adc, VBAT_ADC_CHANNEL, &raw);

    int32_t acc = 0;
    int got = 0;
    for (int i = 0; i < VBAT_SAMPLES; i++) {
        if (adc_oneshot_read(s_adc, VBAT_ADC_CHANNEL, &raw) == ESP_OK) {
            acc += raw;
            got++;
        }
    }
    if (!got) { s_valid = false; return; }

    int avg = (int)(acc / got);

    int pin_mv = 0;
    if (s_cali_ok) {
        adc_cali_raw_to_voltage(s_cali, avg, &pin_mv);
    } else {
        /* 12-bit over roughly 3.1 V, uncorrected. */
        pin_mv = (avg * 3100) / 4095;
    }

    s_mv = (uint16_t)((pin_mv * 1000) / VBAT_DIVIDER_PERMILLE);

    uint8_t pct = curve_percent(s_mv);

    /* Deadband against the value carried through deep sleep, so the
     * indicator does not wander by a point or two every wake.  A real
     * change of three points or more gets through immediately. */
    if (s_last_valid && abs((int)pct - (int)s_last_percent) < 3)
        pct = s_last_percent;

    s_percent      = pct;
    s_valid        = true;
    s_last_percent = pct;
    s_last_valid   = true;

    ESP_LOGI(TAG, "%u mV at the cell, %u%%%s",
             s_mv, s_percent, s_cali_ok ? "" : " (uncalibrated)");
}

bool     battery_valid(void)   { return s_valid; }
uint16_t battery_mv(void)      { return s_mv; }
uint8_t  battery_percent(void) { return s_percent; }
bool     battery_low(void)     { return s_valid && s_percent <= 10; }
