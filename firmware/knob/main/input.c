/* input.c - quadrature decode and the push button. */
#include "input.h"
#include "board_knob.h"

#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_attr.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include <inttypes.h>

static const char *TAG = "input";

/* Gray-code transitions indexed by (prev << 2 | now). */
static const int8_t k_table[16] = {
     0, -1, +1,  0,
    +1,  0,  0, -1,
    -1,  0,  0, +1,
     0, +1, -1,  0
};

static volatile int      s_accum = 0;         /* quarter steps      */
static volatile uint8_t  s_prev  = 0;
static volatile bool     s_down  = false;
static volatile int64_t  s_down_us = 0;
static volatile bool     s_click_pending = false;
static volatile bool     s_hold_fired = false;
static volatile bool     s_hold_pending = false;

static int64_t now_us(void) { return esp_timer_get_time(); }

static void IRAM_ATTR enc_isr(void *arg)
{
    (void)arg;
    uint8_t now = (uint8_t)((gpio_get_level(PIN_ENC_A) ? 2 : 0) |
                            (gpio_get_level(PIN_ENC_B) ? 1 : 0));
    s_accum += k_table[(s_prev << 2) | now];
    s_prev = now;
}

static void IRAM_ATTR btn_isr(void *arg)
{
    (void)arg;
    bool down = gpio_get_level(PIN_BTN) == BTN_ACTIVE_LEVEL;
    if (down && !s_down) {
        s_down = true;
        s_down_us = esp_timer_get_time();
        s_hold_fired = false;
    } else if (!down && s_down) {
        s_down = false;
        /* A press that already became a hold is not also a click. */
        if (!s_hold_fired && (esp_timer_get_time() - s_down_us) > 30000)
            s_click_pending = true;
    }
}

void input_init(void)
{
    gpio_config_t enc = {
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
        .pin_bit_mask = BIT64(PIN_ENC_A) | BIT64(PIN_ENC_B),
    };
    ESP_ERROR_CHECK(gpio_config(&enc));

    gpio_config_t btn = {
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
        .pin_bit_mask = BIT64(PIN_BTN),
    };
    ESP_ERROR_CHECK(gpio_config(&btn));

    s_prev = (uint8_t)((gpio_get_level(PIN_ENC_A) ? 2 : 0) |
                       (gpio_get_level(PIN_ENC_B) ? 1 : 0));

    ESP_ERROR_CHECK(gpio_install_isr_service(ESP_INTR_FLAG_IRAM));
    ESP_ERROR_CHECK(gpio_isr_handler_add(PIN_ENC_A, enc_isr, NULL));
    ESP_ERROR_CHECK(gpio_isr_handler_add(PIN_ENC_B, enc_isr, NULL));
    ESP_ERROR_CHECK(gpio_isr_handler_add(PIN_BTN,   btn_isr, NULL));

    /* If we woke on the button it is already down and no edge is coming,
     * so seed the state rather than wait for a release we would then
     * read as a stray click. */
    if (gpio_get_level(PIN_BTN) == BTN_ACTIVE_LEVEL) {
        s_down = true;
        s_down_us = now_us();
        s_hold_fired = false;
    }
}

bool input_button_down(void) { return s_down; }

uint32_t input_hold_ms(void)
{
    if (!s_down) return 0;
    return (uint32_t)((now_us() - s_down_us) / 1000);
}

input_event_t input_poll(int *value)
{
    if (value) *value = 0;

    /* Hold is detected here, not in the ISR: the ring has to start
     * filling while the button is still down, and that is not work to
     * do in interrupt context. */
    if (s_down && !s_hold_fired && input_hold_ms() >= T_POWER_HOLD_MS) {
        s_hold_fired = true;
        s_hold_pending = true;
    }

    if (s_hold_pending)  { s_hold_pending = false;  return IN_HOLD; }
    if (s_click_pending) { s_click_pending = false; return IN_CLICK; }

    /* Four quarter-steps per detent. */
    int acc = s_accum;
    int detents = acc / 4;
    if (detents) {
        s_accum -= detents * 4;
        if (value) *value = detents;
        return IN_ROTATE;
    }
    return IN_NONE;
}

bool input_prepare_sleep(void)
{
    /* GPIO0, 5 and 6 are all RTC pads (0-21 on the S3), which is the
     * whole reason any of them can be a wake source: the RTC controller
     * watches them with the digital domain powered down.  Touch cannot
     * do this - this board leaves TP_INT unconnected, so the touch
     * controller can only be polled, and polling needs a CPU. */
    static const gpio_num_t k_cand[] = {
        PIN_BTN,
#if WAKE_ON_ROTATION
        PIN_ENC_A, PIN_ENC_B,
#endif
    };
    const int n_cand = (int)(sizeof k_cand / sizeof k_cand[0]);

    uint64_t mask = 0;
    for (int i = 0; i < n_cand; i++) {
        /* Arm only what is actually high right now.  Arming a line that
         * is already low wakes the chip the instant it sleeps - and a
         * detented encoder that rests with a contact closed would do
         * exactly that, forever. */
        if (gpio_get_level(k_cand[i]) != 0) mask |= BIT64(k_cand[i]);
    }

    if (!mask) {
        ESP_LOGW(TAG, "every wake pin is low, staying awake");
        return false;
    }

    gpio_isr_handler_remove(PIN_ENC_A);
    gpio_isr_handler_remove(PIN_ENC_B);
    gpio_isr_handler_remove(PIN_BTN);

    for (int i = 0; i < n_cand; i++) {
        gpio_num_t p = k_cand[i];
        if (!(mask & BIT64(p))) continue;
        /* The pull-up has to survive the sleep, and the ordinary GPIO
         * one does not - it has to be the RTC pad's. */
        rtc_gpio_init(p);
        rtc_gpio_set_direction(p, RTC_GPIO_MODE_INPUT_ONLY);
        rtc_gpio_pulldown_dis(p);
        rtc_gpio_pullup_en(p);
    }

    /* ...and RTC pull-ups only hold while their power domain is up.
     * This is what deep-sleep current on this device actually costs. */
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);

    ESP_ERROR_CHECK(esp_sleep_enable_ext1_wakeup(mask, ESP_EXT1_WAKEUP_ANY_LOW));
    ESP_LOGI(TAG, "wake armed, mask 0x%" PRIx64, mask);
    return true;
}
