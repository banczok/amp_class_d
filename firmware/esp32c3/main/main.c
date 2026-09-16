/* main.c - on-board ESP32-C3, top level.
 *
 * Two jobs, and only two:
 *
 *   1. Be the ESP-NOW endpoint for the battery knob, so a device that is
 *      asleep 99% of the time can change the volume in a few
 *      milliseconds and turn the amplifier on from cold.
 *   2. Cache the last amp state and the last now-playing blob, so a knob
 *      that has just woken up gets something to draw immediately instead
 *      of waiting for a round trip through the Pico and the Pi.
 *
 * It decides nothing.  Every key it receives goes straight to the Pico,
 * which owns the state.  If this chip dies, the amplifier loses its
 * remote knob and nothing else.
 *
 * Note on sleep: this module does not sleep.  It sits on +5VA and has to
 * hear an ESP-NOW frame that could arrive at any moment, which means the
 * receiver stays on - an unassociated or dozing station simply drops
 * them.  The deep-sleep cycle in this system belongs to the battery
 * knob, which is the thing that actually runs out of charge.
 */
#include "board.h"
#include "state.h"
#include "radio.h"
#include "picolink.h"
#include "proto.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "amp_c3";

#define HEARTBEAT_MS 30000

void app_main(void)
{
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        e = nvs_flash_init();
    }
    ESP_ERROR_CHECK(e);

    state_init();
    picolink_init();
    radio_init();

    /* One hello at boot to learn where the amplifier stands.  This does
     * wake a sleeping Pico, which is harmless - waking out of WFI is not
     * the same as switching the amplifier on. */
    picolink_send(MSG_ESP_HELLO, NULL, 0);

    int64_t last_hello = esp_timer_get_time() / 1000;
    int64_t last_beat  = last_hello;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(200));
        int64_t t = esp_timer_get_time() / 1000;

        /* Refresh the cached state periodically, but ONLY while the Pico
         * is awake.  Polling a sleeping Pico would wake it every few
         * seconds and throw away the whole point of the standby mode. */
        if (picolink_pico_awake() && t - last_hello > HELLO_INTERVAL_MS) {
            last_hello = t;
            picolink_send(MSG_ESP_HELLO, NULL, 0);
        }

        if (t - last_beat > HEARTBEAT_MS) {
            last_beat = t;
            amp_state_t a = state_amp();
            ESP_LOGI(TAG, "pico %s  wifi %s ch%u  knob %s  power %u vol %u%s",
                     picolink_pico_awake() ? "awake" : "asleep",
                     radio_wifi_up() ? "up" : "down",
                     radio_channel(),
                     radio_knob_paired() ? "paired" : "unpaired",
                     a.power, a.vol_code, a.muted ? " muted" : "");
        }
    }
}
