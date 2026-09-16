/* state.c - cached amp state and now-playing blob. */
#include "state.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include <string.h>

static amp_state_t    s_amp;
static uint8_t        s_np[NOWPLAYING_MAX];
static int            s_np_len;
static SemaphoreHandle_t s_lock;
static int64_t        s_amp_at_us;

void state_init(void)
{
    memset(&s_amp, 0, sizeof s_amp);
    s_np_len = 0;
    s_lock = xSemaphoreCreateMutex();
}

void state_set_amp(const uint8_t *p, int n)
{
    if (n < 6) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_amp.power    = p[0];
    s_amp.vol_code = p[1];
    s_amp.muted    = p[2];
    s_amp.input    = p[3];
    s_amp.eq_path  = p[4];
    s_amp.max_vol  = p[5];
    s_amp.valid    = true;
    s_amp_at_us    = esp_timer_get_time();
    xSemaphoreGive(s_lock);
}

int64_t state_amp_age_ms(void)
{
    if (!s_amp_at_us) return INT64_MAX;
    return (esp_timer_get_time() - s_amp_at_us) / 1000;
}

amp_state_t state_amp(void)
{
    amp_state_t c;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    c = s_amp;
    xSemaphoreGive(s_lock);
    return c;
}

void state_set_nowplaying(const uint8_t *p, int n)
{
    if (n < 0) return;
    if (n > NOWPLAYING_MAX) n = NOWPLAYING_MAX;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy(s_np, p, (size_t)n);
    s_np_len = n;
    xSemaphoreGive(s_lock);
}

int state_nowplaying(uint8_t *out, int max)
{
    int n;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    n = s_np_len < max ? s_np_len : max;
    memcpy(out, s_np, (size_t)n);
    xSemaphoreGive(s_lock);
    return n;
}
