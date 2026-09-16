/* radio.c - ESP-NOW endpoint for the battery knob.
 *
 * Why ESP-NOW and not plain WiFi for control: the knob is battery
 * powered and spends its life in deep sleep.  A WiFi association costs
 * roughly a second and a couple of hundred millijoules; an ESP-NOW frame
 * costs a few milliseconds.  Turning the volume has to feel instant, so
 * control goes over ESP-NOW.  Album art does not, so that goes over
 * WiFi, on the knob, in its own time.
 *
 * The catch is that both have to happen on the same radio channel.  So
 * this side joins the house WiFi like any other station and lets ESP-NOW
 * ride on the STA interface - peers are registered with channel 0, which
 * means "whatever channel we are on now", so an AP channel change fixes
 * itself.  The C3 is mains powered from +5VA, so an always-associated
 * station costs us nothing.
 *
 * If there is no SSID configured, or the AP is down, we fall back to a
 * fixed channel so a knob that also falls back can still reach us.  The
 * amplifier must remain controllable when the network is not.
 */
#include "radio.h"
#include "board.h"
#include "state.h"
#include "picolink.h"
#include "proto.h"

#include "esp_wifi.h"
#include "esp_now.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

#ifdef CONFIG_AMP_ESPNOW_ENCRYPT
#define AMP_ENCRYPT 1
#else
#define AMP_ENCRYPT 0
#endif

static const char *TAG = "radio";

static const uint8_t BCAST[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

static uint8_t  s_knob[6];
static bool     s_knob_known = false;
static int64_t  s_knob_seen_ms = 0;
static uint8_t  s_channel = CONFIG_AMP_ESPNOW_CHANNEL;
static bool     s_wifi_up = false;

#define KNOB_AWAKE_MS 10000

/* Older than this and the cached state is worth disturbing the Pico
 * for.  Anything newer, answer from cache and leave it asleep. */
#define STATE_STALE_MS 5000

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

uint8_t radio_channel(void)  { return s_channel; }
bool radio_wifi_up(void)     { return s_wifi_up; }
bool radio_knob_paired(void) { return s_knob_known; }

/* --------------------------------------------------------------- nvs */

static void knob_load(void)
{
    nvs_handle_t h;
    if (nvs_open("amp", NVS_READONLY, &h) != ESP_OK) return;
    size_t n = sizeof s_knob;
    if (nvs_get_blob(h, "knob", s_knob, &n) == ESP_OK && n == sizeof s_knob)
        s_knob_known = true;
    nvs_close(h);
}

static void knob_store(void)
{
    nvs_handle_t h;
    if (nvs_open("amp", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, "knob", s_knob, sizeof s_knob);
    nvs_commit(h);
    nvs_close(h);
}

void radio_forget_knob(void)
{
    if (s_knob_known) esp_now_del_peer(s_knob);
    s_knob_known = false;
    nvs_handle_t h;
    if (nvs_open("amp", NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, "knob");
        nvs_commit(h);
        nvs_close(h);
    }
}

/* ------------------------------------------------------------- peers */

static void add_peer(const uint8_t *mac, bool encrypt)
{
    if (esp_now_is_peer_exist(mac)) return;

    esp_now_peer_info_t p = { 0 };
    memcpy(p.peer_addr, mac, 6);
    p.channel = 0;                 /* 0 = current channel, follows the AP */
    p.ifidx   = WIFI_IF_STA;
    p.encrypt = encrypt;
#if AMP_ENCRYPT
    if (encrypt) memcpy(p.lmk, CONFIG_AMP_ESPNOW_LMK, ESP_NOW_KEY_LEN);
#endif
    esp_err_t e = esp_now_add_peer(&p);
    if (e != ESP_OK) ESP_LOGW(TAG, "add_peer: %s", esp_err_to_name(e));
}

static void pair(const uint8_t *mac)
{
    if (s_knob_known && memcmp(s_knob, mac, 6) == 0) return;
    if (s_knob_known) esp_now_del_peer(s_knob);
    memcpy(s_knob, mac, 6);
    s_knob_known = true;
    add_peer(s_knob, AMP_ENCRYPT);
    knob_store();
    ESP_LOGI(TAG, "paired knob %02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

/* -------------------------------------------------------------- send */

static void send_to_knob(uint8_t type, const uint8_t *p, uint8_t n)
{
    if (!s_knob_known) return;
    uint8_t f[PROTO_MAX_PAYLOAD + 4];
    size_t k = proto_build(f, type, p, n);
    esp_now_send(s_knob, f, k);
}

static void send_state_now(void)
{
    amp_state_t a = state_amp();
    uint8_t p[6] = { a.power, a.vol_code, a.muted, a.input, a.eq_path, a.max_vol };
    send_to_knob(MSG_ESP_STATE, p, sizeof p);
}

static void send_nowplaying_now(void)
{
    uint8_t np[NOWPLAYING_MAX];
    int n = state_nowplaying(np, sizeof np);
    if (n > 0) send_to_knob(MSG_ESP_NOW, np, (uint8_t)n);
}

/* A knob that is asleep cannot hear us, and an unacknowledged unicast
 * just burns airtime, so only push while we believe it is listening. */
static bool knob_awake(void)
{
    return s_knob_known && (now_ms() - s_knob_seen_ms) < KNOB_AWAKE_MS;
}

void radio_push_state(void)      { if (knob_awake()) send_state_now(); }
void radio_push_nowplaying(void) { if (knob_awake()) send_nowplaying_now(); }

/* -------------------------------------------------------------- recv */

static void on_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    proto_rx_t rx;
    proto_rx_init(&rx);

    for (int i = 0; i < len; i++) {
        if (!proto_rx_byte(&rx, data[i])) continue;

        s_knob_seen_ms = now_ms();

        switch (rx.type) {
        case MSG_ESP_HELLO:
            /* The knob announces itself on every wake.  Answer with
             * everything it needs to draw a first frame. */
            pair(info->src_addr);
            send_state_now();
            send_nowplaying_now();
            /* The knob says hello every second and a half while it is
             * awake, to prove the link is alive.  Forwarding all of those
             * would wake a sleeping Pico five times a session for nothing,
             * so only ask when what we hold is actually old. */
            if (state_amp_age_ms() > STATE_STALE_MS)
                picolink_send(MSG_ESP_HELLO, NULL, 0);
            break;

        case MSG_ESP_REMOTE:
            if (!s_knob_known) pair(info->src_addr);
            /* Straight through.  Deciding what a key means is the Pico's
             * job, not ours - it owns the state. */
            picolink_send(MSG_ESP_REMOTE, rx.buf, rx.len);
            send_state_now();
            break;

        default:
            break;
        }
    }
}

/* -------------------------------------------------------------- wifi */

static esp_timer_handle_t s_retry;

static void retry_cb(void *arg)
{
    (void)arg;
    esp_wifi_connect();
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)data;

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (strlen(CONFIG_AMP_WIFI_SSID) > 0) esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_wifi_up = false;
        /* Retry off a timer rather than sleeping here: this runs in the
         * event task, and blocking it stalls every other WiFi event. */
        if (strlen(CONFIG_AMP_WIFI_SSID) > 0 && s_retry)
            esp_timer_start_once(s_retry, 2000 * 1000);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_wifi_up = true;
        uint8_t ch; wifi_second_chan_t sec;
        esp_wifi_get_channel(&ch, &sec);
        s_channel = ch;
        ESP_LOGI(TAG, "associated, channel %u", ch);
    }
}

void radio_init(void)
{
    knob_load();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    const esp_timer_create_args_t ta = { .callback = retry_cb, .name = "wifi_retry" };
    ESP_ERROR_CHECK(esp_timer_create(&ta, &s_retry));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL, NULL));

    if (strlen(CONFIG_AMP_WIFI_SSID) > 0) {
        wifi_config_t wc = { 0 };
        strncpy((char *)wc.sta.ssid,     CONFIG_AMP_WIFI_SSID, sizeof wc.sta.ssid - 1);
        strncpy((char *)wc.sta.password, CONFIG_AMP_WIFI_PASS, sizeof wc.sta.password - 1);
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    }

    ESP_ERROR_CHECK(esp_wifi_start());

    /* No power save.  An unassociated or dozing station drops ESP-NOW
     * frames, and a volume key that needs pressing twice is worse than
     * the 70-odd milliamps this costs on an always-on rail. */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    if (strlen(CONFIG_AMP_WIFI_SSID) == 0) {
        /* Nothing to associate with: park on the configured channel so a
         * knob that also falls back can still find us. */
        ESP_ERROR_CHECK(esp_wifi_set_channel(s_channel, WIFI_SECOND_CHAN_NONE));
        ESP_LOGW(TAG, "no SSID configured, ESP-NOW only on channel %u", s_channel);
    }

    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb(on_recv));
#if AMP_ENCRYPT
    ESP_ERROR_CHECK(esp_now_set_pmk((const uint8_t *)CONFIG_AMP_ESPNOW_PMK));
#endif

    /* Broadcast peer so an unpaired knob can say hello. */
    add_peer(BCAST, false);
    if (s_knob_known) add_peer(s_knob, AMP_ENCRYPT);

    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    ESP_LOGI(TAG, "ESP-NOW up, our MAC %02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    if (s_knob_known)
        ESP_LOGI(TAG, "knob %02X:%02X:%02X:%02X:%02X:%02X from NVS",
                 s_knob[0], s_knob[1], s_knob[2], s_knob[3], s_knob[4], s_knob[5]);
}
