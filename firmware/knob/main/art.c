/* art.c - fetch and decode the album cover.
 *
 * 480x480 RGB565 is 460,800 bytes.  There is 8 MB of octal PSRAM on this
 * board, so holding one full-screen canvas is not the problem; holding
 * two while decoding would be, which is why the decode writes straight
 * into the live canvas and the UI hides the image until it completes.
 *
 * The Pi serves a pre-scaled 480x480 baseline JPEG.  Doing the resample
 * on the Pi rather than here is worth saying out loud: an arbitrary
 * 1400x1400 cover would need a second full buffer plus a scaler, for a
 * result no better than what a machine with a real CPU can produce
 * before it ever hits the air.
 *
 * PLACEHOLDER: Raspberry Pi 4 - the two endpoints below have to exist
 * on the Pi.  See README.
 */
#include "art.h"
#include "net.h"
#include "board_knob.h"

#include "esp_http_client.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_jpeg_dec.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char *TAG = "art";

#define CANVAS_BYTES (LCD_H_RES * LCD_V_RES * 2)
#define JPEG_MAX     (192 * 1024)
#define NET_WAIT_MS  8000

static uint8_t     *s_canvas;
static lv_img_dsc_t s_dsc;
static volatile art_state_t s_state = ART_NONE;
static volatile uint8_t     s_loaded_id;
static volatile uint8_t     s_want_id;
static TaskHandle_t s_task;

art_state_t art_state(void)     { return s_state; }
uint8_t     art_loaded_id(void) { return s_loaded_id; }
const lv_img_dsc_t *art_image(void) { return &s_dsc; }

/* ------------------------------------------------------------- fetch */

static int http_get(const char *url, uint8_t *buf, int max)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 6000,
        .disable_auto_redirect = false,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return -1;

    int got = -1;
    if (esp_http_client_open(c, 0) == ESP_OK) {
        int len = esp_http_client_fetch_headers(c);
        if (len <= max) {
            got = esp_http_client_read(c, (char *)buf, max);
            if (esp_http_client_get_status_code(c) != 200) got = -1;
        } else {
            ESP_LOGW(TAG, "cover is %d bytes, buffer is %d", len, max);
        }
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return got;
}

/* ------------------------------------------------------------ decode */

static bool decode_into_canvas(uint8_t *jpg, int len)
{
    jpeg_dec_config_t cfg = DEFAULT_JPEG_DEC_CONFIG();
    cfg.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;

    jpeg_dec_handle_t dec = NULL;
    if (jpeg_dec_open(&cfg, &dec) != JPEG_ERR_OK) return false;

    jpeg_dec_io_t io = { 0 };
    jpeg_dec_header_info_t hdr = { 0 };
    io.inbuf = jpg;
    io.inbuf_len = len;

    bool ok = false;
    if (jpeg_dec_parse_header(dec, &io, &hdr) == JPEG_ERR_OK) {
        if (hdr.width == LCD_H_RES && hdr.height == LCD_V_RES) {
            io.outbuf = s_canvas;
            ok = jpeg_dec_process(dec, &io) == JPEG_ERR_OK;
        } else {
            ESP_LOGW(TAG, "cover is %dx%d, expected %dx%d - the Pi should "
                          "scale it", (int)hdr.width, (int)hdr.height,
                     LCD_H_RES, LCD_V_RES);
        }
    }
    jpeg_dec_close(dec);
    return ok;
}

/* -------------------------------------------------------------- task */

static void fetch_task(void *arg)
{
    (void)arg;
    char url[192];
    uint8_t *jpg = NULL;

    for (;;) {
        /* Wait to be poked by art_request(). */
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        uint8_t want = s_want_id;
        if (want == 0 || want == s_loaded_id) { s_state = want ? ART_READY : ART_NONE; continue; }

        s_state = ART_LOADING;

        net_connect();
        int waited = 0;
        while (!net_is_up() && waited < NET_WAIT_MS) {
            vTaskDelay(pdMS_TO_TICKS(100));
            waited += 100;
        }
        if (!net_is_up()) {
            ESP_LOGW(TAG, "no network, cover skipped");
            s_state = ART_FAILED;
            continue;
        }

        if (!jpg) jpg = heap_caps_malloc(JPEG_MAX, MALLOC_CAP_SPIRAM);
        if (!jpg) { s_state = ART_FAILED; continue; }

        snprintf(url, sizeof url, "%s/cover.jpg?id=%u", CONFIG_KNOB_PI_BASE_URL, want);
        int n = http_get(url, jpg, JPEG_MAX);
        if (n <= 0) {
            ESP_LOGW(TAG, "cover fetch failed");
            s_state = ART_FAILED;
            continue;
        }

        if (decode_into_canvas(jpg, n)) {
            s_loaded_id = want;
            s_state = ART_READY;
            ESP_LOGI(TAG, "cover %u loaded, %d bytes of JPEG", want, n);
        } else {
            s_state = ART_FAILED;
        }
    }
}

/* -------------------------------------------------------------- init */

void art_init(void)
{
    s_canvas = heap_caps_malloc(CANVAS_BYTES, MALLOC_CAP_SPIRAM);
    if (!s_canvas) {
        ESP_LOGE(TAG, "no PSRAM for the cover canvas");
        return;
    }
    memset(s_canvas, 0, CANVAS_BYTES);

    s_dsc.header.always_zero = 0;
    s_dsc.header.w  = LCD_H_RES;
    s_dsc.header.h  = LCD_V_RES;
    s_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
    s_dsc.data_size = CANVAS_BYTES;
    s_dsc.data      = s_canvas;

    xTaskCreate(fetch_task, "art", 5120, NULL, 4, &s_task);
}

void art_request(uint8_t art_id)
{
    if (!s_canvas || !s_task) return;
    if (art_id == s_loaded_id && art_id != 0) { s_state = ART_READY; return; }
    s_want_id = art_id;
    xTaskNotifyGive(s_task);
}
