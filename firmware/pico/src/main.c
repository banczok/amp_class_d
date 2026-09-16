/* main.c - preamp controller, top level.
 *
 * Core 0 runs everything that touches the outside world: encoder, IR,
 * both UART links, the relays and the power sequencer.  Core 1 does
 * nothing but the meter DSP, so a 1024-point FFT can never delay a
 * volume change.
 *
 * The one rule this file exists to enforce: the Pico is authoritative.
 * Volume, input, EQ and power all live here, in flash-backed settings.
 * The Pi is a display and a touch surface, the ESP32-C3 is a radio, and
 * the preamp keeps working with either of them unplugged.
 */
#include "board.h"
#include "audio.h"
#include "relays.h"
#include "encoder.h"
#include "irrx.h"
#include "meter.h"
#include "menu.h"
#include "power.h"
#include "settings.h"
#include "linkpi.h"
#include "linkesp.h"
#include "dispatch.h"
#include "pga2320.h"
#include "bd37033.h"
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "pico/flash.h"
#include <stdio.h>

#define METER_TX_INTERVAL_MS   21     /* ~47 Hz to the panel            */
#define STANDBY_IDLE_MS      2000     /* quiet this long -> go to sleep */
#define VOL_STEP_DETENT         2     /* 1 dB per click                 */
#define VOL_STEP_REMOTE         2

static uint32_t s_last_activity = 0;
static uint32_t s_last_meter_tx = 0;
static power_state_t s_prev_state = POWER_STANDBY;

static uint32_t now_ms(void) { return to_ms_since_boot(get_absolute_time()); }
static void     touched(void) { s_last_activity = now_ms(); }

/* Core 1 does the metering.  It has to register as a flash-lockout
 * victim first, otherwise settings_save() cannot safely erase a sector
 * while core 1 is executing from XIP. */
static void core1_entry(void)
{
    flash_safe_execute_core_init();
    meter_core1_main();
}

/* ---------------------------------------------------------- dispatch */

/* One place where a function id becomes an action, whatever produced it:
 * a learned IR key, the ESP-NOW knob, or the Pi.  When the menu is open
 * the navigation subset is redirected into it, so a remote can drive the
 * settings screens exactly like the encoder can. */
void dispatch_fn(fn_id_t fn, bool repeat)
{
    touched();

    if (menu_is_open()) {
        switch (fn) {
        case FN_UP:   case FN_VOL_UP:   menu_key(MK_UP);   return;
        case FN_DOWN: case FN_VOL_DOWN: menu_key(MK_DOWN); return;
        case FN_OK:                     menu_key(MK_OK);   return;
        case FN_BACK: case FN_MENU:     menu_key(MK_BACK); return;
        default: break;    /* power and mute still work from in here */
        }
    }

    switch (fn) {
    case FN_VOL_UP:   audio_volume_step(+VOL_STEP_REMOTE); break;
    case FN_VOL_DOWN: audio_volume_step(-VOL_STEP_REMOTE); break;

    case FN_MUTE:
        if (!repeat) audio_toggle_mute();
        break;

    case FN_POWER:
        if (!repeat) power_toggle();
        break;

    case FN_INPUT:
        if (!repeat)
            audio_set_input(audio_get()->input == IN_SEL_DIGITAL
                            ? IN_SEL_ANALOG : IN_SEL_DIGITAL);
        break;

    case FN_EQ_BYPASS:
        if (!repeat)
            audio_set_eq_path(audio_get()->eq_path == EQ_PATH_ACTIVE
                              ? EQ_PATH_BYPASS : EQ_PATH_ACTIVE);
        break;

    case FN_MENU:
        if (!repeat) menu_open();
        break;

    case FN_PRESET_1: case FN_PRESET_2: case FN_PRESET_3: {
        if (repeat) break;
        preset_t *p = &settings()->preset[fn - FN_PRESET_1];
        if (!p->used) break;
        for (int b = 0; b < 3; b++) {
            audio_set_band_shape((bd_band_t)b, p->tone_f0[b], p->tone_q[b]);
            audio_set_tone((bd_band_t)b, p->tone_gain[b]);
        }
        audio_set_loudness(p->loudness, p->loudness_f0);
        break;
    }

    /* ---- PLACEHOLDER: Raspberry Pi 4 -------------------------------- *
     * Transport control belongs to whatever is playing, which is moOde
     * on the Pi.  The Pico has no business knowing about tracks, so it
     * forwards the keypress and forgets about it.  When the Pi side
     * exists, MSG_EVENT kind 1 is what it should be listening for.     */
    case FN_PREV: case FN_NEXT: case FN_PLAY_PAUSE:
        linkpi_send_event(1, (uint8_t)fn, repeat ? 1 : 0);
        return;                                  /* no local state changed */

    default:
        return;
    }

    linkpi_send_state();
    linkesp_send_state();
}

/* A burst of detents from the knob.  Inside the menu it moves the
 * cursor, outside it moves the volume - same as the local encoder. */
void dispatch_volume_delta(int detents)
{
    touched();

    if (menu_is_open()) {
        for (int i = 0; i < detents;  i++) menu_key(MK_DOWN);
        for (int i = 0; i > detents;  i--) menu_key(MK_UP);
        return;
    }

    audio_volume_step(detents * VOL_STEP_DETENT);
    linkpi_send_state();
    linkesp_send_state();
}

/* ------------------------------------------------------------ inputs */

static void service_encoder(void)
{
    enc_event_t e;
    while ((e = encoder_poll()) != ENC_NONE) {
        touched();

        if (menu_is_open()) { menu_encoder(e); continue; }

        switch (e) {
        case ENC_CW:    audio_volume_step(+VOL_STEP_DETENT);
                        linkpi_send_state(); linkesp_send_state(); break;
        case ENC_CCW:   audio_volume_step(-VOL_STEP_DETENT);
                        linkpi_send_state(); linkesp_send_state(); break;
        case ENC_CLICK: audio_toggle_mute();
                        linkpi_send_state(); linkesp_send_state(); break;
        case ENC_LONG:  menu_open();                                  break;
        default: break;
        }
    }
}

static void service_ir(void)
{
    ir_key_t k;
    bool repeat;

    /* While learning, menu_tick() owns the receiver - it needs the raw
     * key, not the function it is not yet bound to. */
    if (menu_screen() == SCR_IR_WAIT) return;

    while (irrx_poll(&k, &repeat)) {
        fn_id_t fn = settings_lookup_ir(&k);
        if (fn == FN_NONE) {
            /* Unknown key.  Useful when a remote is half-learned, so
             * report it rather than dropping it silently. */
            linkpi_send_irlearn(FN_NONE, IRLEARN_UNKNOWN, k.proto, k.addr, k.cmd);
            continue;
        }
        dispatch_fn(fn, repeat);
    }
}

static void service_power_button(void)
{
    if (!power_button_pressed()) return;
    touched();

    /* The front panel button is the one control that always means power,
     * even with the menu open - it is the escape hatch. */
    if (menu_is_open()) menu_close();
    power_toggle();
    linkpi_send_state();
    linkesp_send_state();
}

static void service_meter(void)
{
    meter_result_t m;
    if (power_state() != POWER_ON) return;
    if (now_ms() - s_last_meter_tx < METER_TX_INTERVAL_MS) return;
    if (!meter_get(&m)) return;
    s_last_meter_tx = now_ms();
    linkpi_send_meter(&m);
}

/* Apply the stored startup state at the moment the rails are coming up,
 * so the first thing the PGA hears about is the user's chosen level and
 * not whatever was set before the last shutdown. */
static void service_state_change(void)
{
    power_state_t st = power_state();
    if (st == s_prev_state) return;

    if (s_prev_state == POWER_STANDBY && st == POWER_STARTING) {
        settings_t *cfg = settings();
        audio_set_input(cfg->last_input);
        audio_set_balance(cfg->balance);
        audio_set_volume(cfg->startup_vol);
    }

    s_prev_state = st;
    linkpi_send_state();
    linkesp_send_state();
}

/* -------------------------------------------------------------- main */

int main(void)
{
    stdio_init_all();          /* USB CDC console; both UARTs are taken */

    settings_load();
    relays_init();             /* coils off first - that is also reset  */
    power_init();
    encoder_init();
    irrx_init();
    linkpi_init();
    linkesp_init();
    meter_init();

    multicore_launch_core1(core1_entry);

    /* The audio chips are unpowered in standby, so this only sets up the
     * shadow state and the two GPIO mute controls.  power_tick() calls
     * pga_init()/bd_init() again once the rails are actually up. */
    audio_init();
    audio_hard_mute(true);

    {
        settings_t *cfg = settings();
        audio_set_input(cfg->last_input);
        audio_set_balance(cfg->balance);
        audio_set_volume(cfg->startup_vol);
    }

    linkpi_log("preamp_ctrl up");
    linkpi_send_state();
    linkesp_send_state();
    touched();

    for (;;) {
        service_power_button();
        service_encoder();
        service_ir();

        linkpi_poll();
        linkesp_poll();
        linkpi_pump_audio();

        menu_tick();
        power_tick();
        service_state_change();
        service_meter();

        /* Standby is the common case for a hi-fi: it spends most of its
         * life switched off but plugged in.  Once nothing has happened
         * for a couple of seconds, drop clk_sys to the crystal and sit
         * in WFI until the power button, the encoder or the ESP32
         * pulling UART RX low wakes us.  power_standby_sleep() returns
         * with the clocks and both links restored.
         *
         * Note the USB console is dead while this is asleep - at 12 MHz
         * the USB PLL is off.  Plugging in the debug cable and pressing
         * the encoder brings it straight back. */
        if (power_state() == POWER_STANDBY &&
            !menu_is_open() &&
            now_ms() - s_last_activity > STANDBY_IDLE_MS) {
            power_standby_sleep();
            touched();
        }

        tight_loop_contents();
    }
}
