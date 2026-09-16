/* power.c - power sequencing and the low-power standby.
 *
 * Sequencing follows the design doc:
 *
 *   on  : mains relay -> Pi boots -> rails settle -> 2.5 s -> unmute
 *   off : mute -> 200 ms -> shutdown request -> Pi halts -> mains off
 *
 * Both mutes are asserted before anything moves.  That matters because
 * K3 energises the amplifier at the same moment it energises T1, so for
 * a few hundred milliseconds the amp is live while the preamp rails are
 * still coming up and C61/C62 are charging toward VREF.  The BD37033
 * MUTE pin is the only control downstream of those caps, so it is the
 * one that has to be held.
 *
 * Standby: the Pico and the ESP32-C3 stay powered from +5VA, everything
 * else is dead.  We drop clk_sys to the crystal, shut the ADC and its
 * DMA down, park the peripherals and sit in WFI.  Three things wake us:
 * the power button, the encoder, and the ESP32 pulling UART RX low.
 */
#include "power.h"
#include "board.h"
#include "audio.h"
#include "relays.h"
#include "meter.h"
#include "encoder.h"
#include "settings.h"
#include "linkpi.h"
#include "linkesp.h"
#include "pga2320.h"
#include "bd37033.h"
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/clocks.h"
#include "hardware/uart.h"
#include "hardware/irq.h"

static power_state_t s_state = POWER_STANDBY;
static uint32_t      s_t0    = 0;
static int           s_step  = 0;
static bool          s_pi_up = false;

static bool     s_btn_down  = false;
static uint32_t s_btn_t0    = 0;
static bool     s_btn_click = false;

static uint32_t now_ms(void) { return to_ms_since_boot(get_absolute_time()); }

void power_init(void)
{
    gpio_init(PIN_PWR_BTN);
    gpio_set_dir(PIN_PWR_BTN, GPIO_IN);
    gpio_disable_pulls(PIN_PWR_BTN);

    gpio_init(PIN_SHUTDOWN_REQ);
    gpio_put(PIN_SHUTDOWN_REQ, 0);
    gpio_set_dir(PIN_SHUTDOWN_REQ, GPIO_OUT);

    gpio_init(PIN_HALTED);
    gpio_set_dir(PIN_HALTED, GPIO_IN);
    gpio_disable_pulls(PIN_HALTED);

    gpio_init(PIN_LED_1); gpio_set_dir(PIN_LED_1, GPIO_OUT); gpio_put(PIN_LED_1, 0);
    gpio_init(PIN_LED_2); gpio_set_dir(PIN_LED_2, GPIO_OUT); gpio_put(PIN_LED_2, 0);

    s_state = POWER_STANDBY;
}

power_state_t power_state(void) { return s_state; }
void power_note_pi_up(void)     { s_pi_up = true; }

void power_request(power_state_t want)
{
    if (want == POWER_ON && s_state == POWER_STANDBY) {
        s_state = POWER_STARTING; s_step = 0; s_t0 = now_ms();
    } else if (want == POWER_STANDBY && s_state == POWER_ON) {
        s_state = POWER_STOPPING; s_step = 0; s_t0 = now_ms();
    }
}

void power_toggle(void)
{
    power_request(s_state == POWER_ON ? POWER_STANDBY : POWER_ON);
}

bool power_button_pressed(void)
{
    bool down = !gpio_get(PIN_PWR_BTN);
    if (down && !s_btn_down) {
        s_btn_down = true;
        s_btn_t0 = now_ms();
    } else if (!down && s_btn_down) {
        s_btn_down = false;
        if (now_ms() - s_btn_t0 > 30) s_btn_click = true;
    }
    if (s_btn_click) { s_btn_click = false; return true; }
    return false;
}

void power_tick(void)
{
    uint32_t dt = now_ms() - s_t0;

    if (s_state == POWER_STARTING) {
        if (s_step == 0) {
            audio_hard_mute(true);
            relay_mains(true);
            gpio_put(PIN_LED_1, 1);
            s_step = 1; s_t0 = now_ms();
        } else if (s_step == 1) {
            if (dt >= T_MAINS_TO_SECONDARY_MS) {
                relay_secondary(true);
                s_step = 2; s_t0 = now_ms();
            }
        } else if (s_step == 2) {
            if (dt >= T_RAILS_SETTLE_MS) {
                /* Rails are up: bring both chips to a known state.  The
                 * BD37033 powers up with its front faders MUTED and a
                 * volume default inside its prohibited range, so this
                 * step is not optional. */
                pga_init();
                bd_init();
                audio_hard_mute(true);
                audio_apply();
                meter_start();
                s_step = 3; s_t0 = now_ms();
            }
        } else if (s_step == 3) {
            if (dt >= T_UNMUTE_DELAY_MS) {
                audio_hard_mute(false);
                s_state = POWER_ON;
                gpio_put(PIN_LED_2, 1);
                linkpi_send_state();
                linkesp_send_state();
            }
        }
    } else if (s_state == POWER_STOPPING) {
        if (s_step == 0) {
            audio_hard_mute(true);
            gpio_put(PIN_LED_2, 0);
            s_step = 1; s_t0 = now_ms();
        } else if (s_step == 1) {
            if (dt >= T_MUTE_BEFORE_SHUTDOWN_MS) {
                gpio_put(PIN_SHUTDOWN_REQ, 1);
                s_step = 2; s_t0 = now_ms();
            }
        } else if (s_step == 2) {
            /* Wait for the Pi to report it has halted, but never hang on
             * it: a wedged Pi must not keep the amplifier powered. */
            if (gpio_get(PIN_HALTED) || dt >= T_PI_HALT_TIMEOUT_MS) {
                meter_stop();
                relay_secondary(false);
                s_step = 3; s_t0 = now_ms();
            }
        } else if (s_step == 3) {
            if (dt >= T_SECONDARY_TO_MAINS_MS) {
                relay_mains(false);
                gpio_put(PIN_SHUTDOWN_REQ, 0);
                gpio_put(PIN_LED_1, 0);
                s_pi_up = false;
                s_state = POWER_STANDBY;
                settings()->last_input = audio_get()->input;
                settings_save();
            }
        }
    }
}

static uint32_t s_saved_sys_khz = 0;

static void enter_low_power(void)
{
    meter_stop();

    /* The Pi is unpowered in standby, so drop its UART entirely rather
     * than try to keep 921600 alive on a 12 MHz peripheral clock. */
    uart_deinit(UART_PI);

    linkesp_arm_wake(true);
    encoder_arm_wake(true);
    gpio_set_irq_enabled(PIN_PWR_BTN, GPIO_IRQ_EDGE_FALL, true);
    irq_set_enabled(IO_IRQ_BANK0, true);
    (void)encoder_wake_pending();          /* clear anything stale */

    s_saved_sys_khz = clock_get_hz(clk_sys) / 1000;

    clock_configure(clk_peri, 0,
                    CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_XOSC_CLKSRC,
                    12 * MHZ, 12 * MHZ);
    set_sys_clock_khz(12000, true);
}

static void exit_low_power(void)
{
    set_sys_clock_khz(s_saved_sys_khz ? s_saved_sys_khz : 150000, true);
    clock_configure(clk_peri, 0,
                    CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLK_SYS,
                    clock_get_hz(clk_sys), clock_get_hz(clk_sys));

    gpio_set_irq_enabled(PIN_PWR_BTN, GPIO_IRQ_EDGE_FALL, false);
    linkesp_arm_wake(false);

    linkpi_init();
    linkesp_init();
    /* Deliberately no meter_start() here: waking from standby lands
     * back in standby, where the analogue rails are still off.
     * power_tick() starts the meter once they come up. */
}

void power_standby_sleep(void)
{
    if (s_state != POWER_STANDBY) return;

    enter_low_power();
    /* Any enabled bank-0 edge wakes the core out of WFI; encoder.c owns
     * the callback and flags it.  The three sources armed above are the
     * power button, the encoder and the ESP32 pulling UART RX low. */
    while (!encoder_wake_pending() && s_state == POWER_STANDBY) {
        __wfi();
    }
    exit_low_power();

    /* Whatever woke us, give the ESP a moment to finish its preamble
     * train and send the real frame: the byte that woke the core was
     * lost while the UART was down. */
    sleep_ms(20);
}
