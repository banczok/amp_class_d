/* link.c - ESP-NOW to the amplifier's C3.
 *
 * Two things make this worth doing over ESP-NOW rather than plain WiFi:
 * latency and energy.  Associating costs about a second and a couple of
 * hundred millijoules; an ESP-NOW frame costs a few milliseconds and
 * needs no association at all.  On a battery knob that wakes, does one
 * thing and sleeps again, that is the difference between a device that
 * feels instant and one that does not.
 *
 * The C3 is discovered by broadcast on the first wake and its MAC kept
 * in NVS after that, so a normal wake is one unicast frame.
 */
#include "link.h"
#include "board_knob.h"

#include "esp_now.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "link";

static const uint8_t BCAST[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

static uint8_t      s_amp_mac[6];
static bool         s_amp_known = false;
static int64_t      s_last_rx_ms = 0;
static amp_state_t  s_amp;
static nowplaying_t s_np;
static volatile uint32_t s_rev = 0;

/* Detent coalescing: a knob spun fast produces a burst, and one frame
 * per click is both slow and pointless. */
static int      s_pending = 0;
static int64_t  s_pending_since = 0;

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

uint32_t link_revision(void)  { return s_rev; }
amp_state_t  link_amp(void)        { return s_amp; }
nowplaying_t link_nowplaying(void) { return s_np; }
bool link_amp_reachable(void)
{
    return s_last_rx_ms != 0 && (now_ms() - s_last_rx_ms) < T_LINK_STALE_MS;
}

bool link_ever_heard(void) { return s_last_rx_ms != 0; }

float link_volume_db(void)
{
    /* PGA2320: 0.5 dB per step, code 192 is exactly 0 dB. */
    return 0.5f * (float)((int)s_amp.vol_code - 192);
}

/* ---------------------------------------------------------------- nvs */

static void mac_load(void)
{
    nvs_handle_t h;
    if (nvs_open("knob", NVS_READONLY, &h) != ESP_OK) return;
    size_t n = sizeof s_amp_mac;
    if (nvs_get_blob(h, "amp", s_amp_mac, &n) == ESP_OK && n == sizeof s_amp_mac)
        s_amp_known = true;
    nvs_close(h);
}

static void mac_store(void)
{
    nvs_handle_t h;
    if (nvs_open("knob", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, "amp", s_amp_mac, sizeof s_amp_mac);
    nvs_commit(h);
    nvs_close(h);
}

/* -------------------------------------------------------------- peers */

static void add_peer(const uint8_t *mac)
{
    if (esp_now_is_peer_exist(mac)) return;
    esp_now_peer_info_t p = { 0 };
    memcpy(p.peer_addr, mac, 6);
    p.channel = 0;                 /* current channel; follows the AP */
    p.ifidx   = WIFI_IF_STA;
    p.encrypt = false;
    esp_err_t e = esp_now_add_peer(&p);
    if (e != ESP_OK) ESP_LOGW(TAG, "add_peer: %s", esp_err_to_name(e));
}

static void tx(const uint8_t *mac, uint8_t type, const uint8_t *p, uint8_t n)
{
    uint8_t f[PROTO_MAX_PAYLOAD + 4];
    size_t k = proto_build(f, type, p, n);
    esp_now_send(mac, f, k);
}

static void send(uint8_t type, const uint8_t *p, uint8_t n)
{
    /* Unicast once we know who we are talking to, broadcast until then.
     * Broadcast still reaches the C3 - it just also reaches everything
     * else, which is why it is only the discovery path. */
    tx(s_amp_known ? s_amp_mac : BCAST, type, p, n);
}

/* --------------------------------------------------------------- recv */

static void take_str(const uint8_t **p, const uint8_t *end, char *out, size_t max)
{
    size_t i = 0;
    while (*p < end && **p && i < max - 1) out[i++] = (char)*(*p)++;
    out[i] = 0;
    if (*p < end) (*p)++;                       /* step over the NUL */
}

static void parse_nowplaying(const uint8_t *p, uint8_t n)
{
    if (n < 2) return;
    const uint8_t *end = p + n;
    nowplaying_t np = { 0 };
    np.playing = (p[0] & NP_FLAG_PLAYING) != 0;
    np.art_id  = p[1];
    p += 2;
    take_str(&p, end, np.artist, sizeof np.artist);
    take_str(&p, end, np.title,  sizeof np.title);
    take_str(&p, end, np.album,  sizeof np.album);
    np.valid = np.title[0] || np.artist[0];
    s_np = np;
    s_rev++;
}

static void on_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    proto_rx_t rx;
    proto_rx_init(&rx);

    for (int i = 0; i < len; i++) {
        if (!proto_rx_byte(&rx, data[i])) continue;

        s_last_rx_ms = now_ms();

        /* Whoever answers is the amplifier.  There is only one, and it
         * is the only thing that speaks this protocol. */
        if (!s_amp_known || memcmp(s_amp_mac, info->src_addr, 6) != 0) {
            memcpy(s_amp_mac, info->src_addr, 6);
            s_amp_known = true;
            add_peer(s_amp_mac);
            mac_store();
            ESP_LOGI(TAG, "amp at %02X:%02X:%02X:%02X:%02X:%02X",
                     s_amp_mac[0], s_amp_mac[1], s_amp_mac[2],
                     s_amp_mac[3], s_amp_mac[4], s_amp_mac[5]);
        }

        switch (rx.type) {
        case MSG_ESP_STATE:
            if (rx.len >= 6) {
                s_amp.power    = rx.buf[0];
                s_amp.vol_code = rx.buf[1];
                s_amp.muted    = rx.buf[2];
                s_amp.input    = rx.buf[3];
                s_amp.eq_path  = rx.buf[4];
                s_amp.max_vol  = rx.buf[5];
                s_amp.valid    = true;
                s_rev++;
            }
            break;

        case MSG_ESP_NOW:
            parse_nowplaying(rx.buf, rx.len);
            break;

        default:
            break;
        }
    }
}

/* --------------------------------------------------------------- send */

void link_hello(void)
{
    send(MSG_ESP_HELLO, NULL, 0);
}

void link_send_key(fn_id_t fn, bool repeat)
{
    uint8_t p[3] = { (uint8_t)fn, repeat ? 1u : 0u, 0 };
    send(MSG_ESP_REMOTE, p, 3);
}

static void flush_detents(void)
{
    if (!s_pending) return;
    int d = s_pending;
    s_pending = 0;
    while (d) {
        int chunk = d > 127 ? 127 : (d < -127 ? -127 : d);
        uint8_t p[3] = { (uint8_t)(chunk > 0 ? FN_VOL_UP : FN_VOL_DOWN),
                         0, (uint8_t)(int8_t)chunk };
        send(MSG_ESP_REMOTE, p, 3);
        d -= chunk;
    }
}

void link_send_volume(int detents)
{
    s_pending += detents;
    if (!s_pending_since) s_pending_since = now_ms();

    /* Send at once if the burst has run long enough, otherwise let the
     * task below pick it up - either way the user sees the local
     * overlay move immediately, so nothing feels delayed. */
    if (now_ms() - s_pending_since >= T_KNOB_COALESCE_MS) {
        flush_detents();
        s_pending_since = 0;
    }
}

static void tick_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(20));
        if (s_pending && now_ms() - s_pending_since >= T_KNOB_COALESCE_MS) {
            flush_detents();
            s_pending_since = 0;
        }
    }
}

/* --------------------------------------------------------------- init */

void link_init(void)
{
    memset(&s_amp, 0, sizeof s_amp);
    memset(&s_np,  0, sizeof s_np);
    mac_load();

    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb(on_recv));

    add_peer(BCAST);
    if (s_amp_known) add_peer(s_amp_mac);

    xTaskCreate(tick_task, "knob_tx", 2560, NULL, 6, NULL);
}
