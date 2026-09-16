/* net.c - WiFi station, brought up late and on purpose.
 *
 * The order of operations on a wake matters more here than anything
 * else in this firmware:
 *
 *   1. radio on, park on the cached channel      (~10 ms)
 *   2. ESP-NOW hello, amp answers with state     (~5 ms)
 *   3. draw
 *   4. only now, associate and fetch the cover   (~1-2 s)
 *
 * Doing it the obvious way round - associate, then talk - would make
 * every wake feel like a second and a half, for a device whose whole job
 * is to respond to a hand reaching for it.
 */
#include "net.h"
#include "board_knob.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "nvs.h"
#include <string.h>

static const char *TAG = "net";

static volatile bool s_up = false;
static bool          s_started = false;

static uint8_t channel_load(void)
{
    nvs_handle_t h;
    uint8_t ch = CONFIG_KNOB_FALLBACK_CHANNEL;
    if (nvs_open("knob", NVS_READONLY, &h) == ESP_OK) {
        uint8_t v = 0;
        if (nvs_get_u8(h, "chan", &v) == ESP_OK && v >= 1 && v <= 13) ch = v;
        nvs_close(h);
    }
    return ch;
}

static void channel_store(uint8_t ch)
{
    nvs_handle_t h;
    if (nvs_open("knob", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "chan", ch);
    nvs_commit(h);
    nvs_close(h);
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)data;

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_up = false;
        /* No retry storm.  If the cover does not arrive this wake, it
         * arrives the next one, and the knob still works meanwhile. */
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_up = true;
        uint8_t ch; wifi_second_chan_t sec;
        if (esp_wifi_get_channel(&ch, &sec) == ESP_OK) {
            channel_store(ch);
            ESP_LOGI(TAG, "associated on channel %u", ch);
        }
    }
}

void net_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL, NULL));

    ESP_ERROR_CHECK(esp_wifi_start());
    s_started = true;

    /* ESP-NOW transmits fine without power save, and the receive window
     * for the amp's reply is only a few milliseconds wide. */
    esp_wifi_set_ps(WIFI_PS_NONE);
}

void net_use_cached_channel(void)
{
    if (!s_started) return;
    uint8_t ch = channel_load();
    esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
}

void net_connect(void)
{
    if (!s_started || s_up) return;
    if (strlen(CONFIG_KNOB_WIFI_SSID) == 0) return;

    wifi_config_t wc = { 0 };
    strncpy((char *)wc.sta.ssid,     CONFIG_KNOB_WIFI_SSID, sizeof wc.sta.ssid - 1);
    strncpy((char *)wc.sta.password, CONFIG_KNOB_WIFI_PASS, sizeof wc.sta.password - 1);
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    esp_wifi_connect();
}

bool net_is_up(void) { return s_up; }

void net_stop(void)
{
    if (!s_started) return;
    esp_wifi_disconnect();
    s_up = false;
}
