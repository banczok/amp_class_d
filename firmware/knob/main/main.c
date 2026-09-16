/* main.c - battery knob remote, top level.
 *
 * The device spends nearly all its life in deep sleep.  Everything here
 * is arranged around making the wake feel immediate:
 *
 *   radio on, cached channel      ~10 ms
 *   ESP-NOW hello, amp answers    ~5 ms
 *   panel init and first frame    ~250 ms
 *   backlight up
 *   ... and only then WiFi and the cover art, ~1-2 s, in the background
 *
 * If the amplifier does not answer within T_LINK_WAIT_MS - it is
 * unplugged, out of range, or the cached channel is wrong - none of that
 * happens.  The screen says so at low brightness, keeps asking, and goes
 * back to sleep in four seconds.  Lighting up a full panel of controls
 * that cannot control anything is worse than saying nothing.
 *
 * Associating first and talking second would make every wake feel like a
 * second and a half, on a device whose entire job is to answer a hand
 * reaching for it.
 *
 * Wake source is the encoder press on GPIO0 and nothing else.  Touch
 * cannot wake it: this board does not bring the touch interrupt out, so
 * the controller can only be polled, which needs the CPU running.  That
 * turned out to suit the intended behaviour anyway - a knob in a pocket
 * or under a cat should not power up an amplifier.
 */
#include "board_knob.h"
#include "display.h"
#include "input.h"
#include "link.h"
#include "net.h"
#include "art.h"
#include "battery.h"
#include "ui.h"

#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_now.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "knob";

/* Pico power_state_t values, as they arrive over the wire. */
#define AMP_STANDBY  0
#define AMP_STARTING 1
#define AMP_ON       2
#define AMP_STOPPING 3

static int64_t s_last_activity;
static bool    s_wake_press;       /* the press that woke us is not a click */
static bool    s_hold_shown;

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

static void note_activity(void) { s_last_activity = now_ms(); }

/* ------------------------------------------------------------- sleep */

static void go_to_sleep(void)
{
    /* Arm first, sleep second.  If nothing can be armed - something is
     * being held - we must not have already torn the screen down. */
    if (!input_prepare_sleep()) {
        note_activity();
        return;
    }

    ESP_LOGI(TAG, "sleeping");

    display_sleep();
    vTaskDelay(pdMS_TO_TICKS(T_BL_FADE_MS));

    net_stop();
    esp_now_deinit();
    esp_wifi_stop();

    esp_deep_sleep_start();
    /* not reached */
}

/* -------------------------------------------------------------- mode */

static ui_mode_t mode_for_state(void)
{
    /* Never heard from the amplifier, or stopped hearing from it: there
     * is no volume to show, no track, and no control that would do
     * anything.  Say so rather than draw a dead panel. */
    if (!link_ever_heard() || !link_amp_reachable())
        return UI_NOLINK;

    amp_state_t  a  = link_amp();
    nowplaying_t np = link_nowplaying();

    if (!a.valid || a.power == AMP_STANDBY || a.power == AMP_STOPPING)
        return UI_PROMPT;

    return (np.valid && np.title[0]) ? UI_PLAYING : UI_IDLE;
}

/* ------------------------------------------------------------- input */

static void handle_input(void)
{
    int v = 0;
    input_event_t e;

    while ((e = input_poll(&v)) != IN_NONE) {
        switch (e) {
        case IN_ROTATE:
            note_activity();
            link_send_volume(v);
            ui_flash_volume();
            break;

        case IN_CLICK:
            if (s_wake_press) {
                /* This is the release of the press that woke us.  It has
                 * already done its job. */
                s_wake_press = false;
                note_activity();
                break;
            }
            note_activity();
            /* Same as the amplifier's own encoder: a short press mutes.
             * Power is the 2 s hold, and only the hold. */
            link_send_key(FN_MUTE, false);
            break;

        case IN_HOLD:
            /* Fires from the wake press too, which is the point: press
             * and keep holding, and the amplifier comes on in one
             * gesture without a second press. */
            s_wake_press = false;
            note_activity();
            link_send_key(FN_POWER, false);
            ui_set_hold_progress(0);
            s_hold_shown = false;
            break;

        default:
            break;
        }
    }

    /* The ring fills while the button is down, so the 2 s hold is a
     * visible thing rather than a guess. */
    uint32_t held = input_hold_ms();
    if (held > 150) {
        ui_set_hold_progress(held);
        s_hold_shown = true;
        note_activity();
    } else if (s_hold_shown && !input_button_down()) {
        ui_set_hold_progress(0);
        s_hold_shown = false;
    }
}

/* --------------------------------------------------------------- main */

void app_main(void)
{
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        e = nvs_flash_init();
    }
    ESP_ERROR_CHECK(e);

    /* Before anything else draws current.  The radio is off, the panel
     * is off and the backlight is off, so this is the quietest the cell
     * will be all wake - and therefore the only reading worth trusting.
     * Costs about a millisecond. */
    battery_init();
    battery_sample();

    /* Radio next, and on the channel we ended on last time, so the
     * hello below lands on the amplifier rather than into a channel it
     * is not listening on. */
    net_init();
    net_use_cached_channel();
    link_init();
    link_hello();

    input_init();
    s_wake_press = input_button_down();

    ESP_ERROR_CHECK(display_init());
    art_init();
    ui_create();
    ui_set_activity_cb(note_activity);

    /* Give the amplifier a proper chance to answer before deciding what
     * to draw.  It replies to a hello in about 5 ms when it is listening,
     * so this is not about its speed - it is about the cached channel
     * being stale, which costs a retry somewhere else. */
    for (int waited = 0; waited < T_LINK_WAIT_MS && !link_ever_heard();
         waited += 20) {
        if (waited && (waited % T_LINK_RETRY_MS) < 20) link_hello();
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    ui_set_link(link_ever_heard());
    ui_set_mode(mode_for_state());
    ui_tick();

    vTaskDelay(pdMS_TO_TICKS(60));      /* let the first frame land */
    /* A wake that found nothing is a wake the user got nothing out of.
     * Dimmer and brief, rather than a full-brightness screen saying no. */
    display_backlight(ui_mode() == UI_NOLINK ? BL_NOLINK_PERCENT
                                             : BL_ACTIVE_PERCENT);

    note_activity();
    ui_mode_t last_mode = ui_mode();
    uint8_t   last_art  = 0;
    int64_t   next_hello = now_ms();

    for (;;) {
        handle_input();

        ui_mode_t want = mode_for_state();
        if (want != last_mode) {
            bool was_nolink = (last_mode == UI_NOLINK);
            last_mode = want;
            ui_set_mode(want);
            note_activity();            /* the amp changed under us    */
            /* It turned up after all - come up to full brightness and
             * give the user the time they would have had. */
            if (was_nolink && want != UI_NOLINK)
                display_backlight(BL_ACTIVE_PERCENT);
        }
        ui_set_link(link_amp_reachable());

        /* Keep the link demonstrably alive.  The amplifier sends state
         * when it changes rather than on a schedule, so without asking
         * now and then a quiet minute is indistinguishable from an
         * unplugged amplifier.  Faster while there is nothing, because
         * then it is a retry rather than a keepalive - and a
         * press-and-hold to power on is still sent either way, since the
         * failure may be one way and the amplifier coming up is exactly
         * what we are waiting for. */
        if (now_ms() >= next_hello) {
            next_hello = now_ms() + (want == UI_NOLINK ? T_LINK_RETRY_MS
                                                       : T_LINK_KEEPALIVE_MS);
            link_hello();
        }

        if (want == UI_PLAYING) {
            /* Cover art is fetched only when there is something to show
             * it on.  Nothing else on this device needs WiFi, so this
             * call is what brings the association up at all - and it must
             * not happen on the no-link screen, where a second and a half
             * of associating would buy nothing. */
            uint8_t id = link_nowplaying().art_id;
            if (id != last_art) {
                last_art = id;
                art_request(id);
            }
        }

        ui_tick();

        int64_t idle = now_ms() - s_last_activity;
        int64_t limit = (want == UI_NOLINK) ? T_NOLINK_TIMEOUT_MS
                      : (want == UI_PROMPT) ? T_PROMPT_TIMEOUT_MS
                                            : T_IDLE_TIMEOUT_MS;
        if (idle > limit && !input_button_down()) go_to_sleep();

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
