/* encoder.c - quadrature encoder plus its push button.
 *
 * The board already does the debouncing: each line has a 10k pull-up, a
 * 1k series resistor and 100n to ground, so tau is about 1 ms and the
 * edges arrive clean.  That means a plain state-table decoder sampled in
 * the GPIO interrupt is enough - no software filtering needed.
 *
 * All three lines are active low.  No internal pulls are enabled: the
 * externals do the job, and RP2350-E9 makes internal pull-downs
 * unreliable anyway (an input driven high then released can latch at
 * ~2.1 V).  Every input on this board has an external pull-up, which is
 * what keeps that erratum out of the design.
 */
#include "encoder.h"
#include "board.h"
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/sync.h"

/* Gray-code transition table indexed by (prev<<2 | now). */
static const int8_t k_table[16] = {
     0, -1, +1,  0,
    +1,  0,  0, -1,
    -1,  0,  0, +1,
     0, +1, -1,  0
};

static volatile int      s_accum = 0;      /* quarter-steps */
static volatile uint8_t  s_prev  = 0;
static volatile bool     s_btn_down = false;
static volatile uint32_t s_btn_t0 = 0;
static volatile bool     s_long_fired = false;
static volatile bool     s_click_pending = false;
static volatile bool     s_long_pending  = false;
static volatile bool     s_any_edge      = false;

static void gpio_cb(uint gpio, uint32_t events)
{
    /* This is the only GPIO callback registered on this core, so it
     * also sees the power button and the ESP RX line.  Note the edge
     * for the standby sleep, then decode the ones we own. */
    s_any_edge = true;

    if (gpio == PIN_ENC_A || gpio == PIN_ENC_B) {
        uint8_t now = (uint8_t)((gpio_get(PIN_ENC_A) ? 2 : 0) |
                                (gpio_get(PIN_ENC_B) ? 1 : 0));
        s_accum += k_table[(s_prev << 2) | now];
        s_prev = now;
    } else if (gpio == PIN_ENC_SW) {
        bool down = !gpio_get(PIN_ENC_SW);      /* active low */
        if (down && !s_btn_down) {
            s_btn_down = true;
            s_btn_t0 = to_ms_since_boot(get_absolute_time());
            s_long_fired = false;
        } else if (!down && s_btn_down) {
            s_btn_down = false;
            if (!s_long_fired) s_click_pending = true;
        }
    }
}

void encoder_init(void)
{
    const uint pins[3] = { PIN_ENC_A, PIN_ENC_B, PIN_ENC_SW };
    for (int i = 0; i < 3; i++) {
        gpio_init(pins[i]);
        gpio_set_dir(pins[i], GPIO_IN);
        gpio_disable_pulls(pins[i]);
    }
    s_prev = (uint8_t)((gpio_get(PIN_ENC_A) ? 2 : 0) |
                       (gpio_get(PIN_ENC_B) ? 1 : 0));

    gpio_set_irq_enabled_with_callback(PIN_ENC_A,
        GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL, true, gpio_cb);
    gpio_set_irq_enabled(PIN_ENC_B,
        GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL, true);
    gpio_set_irq_enabled(PIN_ENC_SW,
        GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL, true);
}

enc_event_t encoder_poll(void)
{
    /* Long press is detected here rather than in the ISR so the menu can
     * open on the way down, the way a user expects, without doing work
     * in interrupt context. */
    if (s_btn_down && !s_long_fired) {
        uint32_t held = to_ms_since_boot(get_absolute_time()) - s_btn_t0;
        if (held >= T_ENC_LONGPRESS_MS) {
            s_long_fired = true;
            s_long_pending = true;
        }
    }

    uint32_t save = save_and_disable_interrupts();
    bool click = s_click_pending; s_click_pending = false;
    bool lng   = s_long_pending;  s_long_pending  = false;
    int  acc   = s_accum;
    restore_interrupts(save);

    if (lng)   return ENC_LONG;
    if (click) return ENC_CLICK;

    /* Four quarter-steps per detent on the common 20-detent encoders. */
    if (acc >= 4)  { save = save_and_disable_interrupts(); s_accum -= 4; restore_interrupts(save); return ENC_CW;  }
    if (acc <= -4) { save = save_and_disable_interrupts(); s_accum += 4; restore_interrupts(save); return ENC_CCW; }
    return ENC_NONE;
}

int encoder_take_delta(void)
{
    uint32_t save = save_and_disable_interrupts();
    int d = s_accum / 4;
    s_accum -= d * 4;
    restore_interrupts(save);
    return d;
}

bool encoder_button_down(void) { return s_btn_down; }

bool encoder_wake_pending(void)
{
    uint32_t save = save_and_disable_interrupts();
    bool p = s_any_edge;
    s_any_edge = false;
    restore_interrupts(save);
    return p;
}

void encoder_arm_wake(bool en)
{
    /* In standby these are the wake sources.  Falling edge only: all
     * three idle high through their pull-ups. */
    gpio_set_irq_enabled(PIN_ENC_A,  GPIO_IRQ_EDGE_FALL, en);
    gpio_set_irq_enabled(PIN_ENC_B,  GPIO_IRQ_EDGE_FALL, en);
    gpio_set_irq_enabled(PIN_ENC_SW, GPIO_IRQ_EDGE_FALL, en);
}
