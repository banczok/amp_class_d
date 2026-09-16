/* picolink.c - UART0 link to the Pico.
 *
 * Same framing as everything else in this project (see common/proto.h),
 * 115200 8N1, no flow control.
 *
 * The one thing this file has to get right is waking a sleeping Pico.
 * In standby the Pico drops clk_sys to the 12 MHz crystal and hands its
 * UART RX pin to the GPIO block so an edge can wake it - which means the
 * byte that does the waking is consumed by the wake itself and never
 * reaches a UART.  So: if we have not heard from the Pico recently,
 * assume it is asleep, send a short train of MSG_ESP_WAKE frames, wait
 * for it to restore its clocks and re-init the UART, and only then send
 * the frame that matters.  Sending the preamble unnecessarily costs
 * nothing: the Pico ignores MSG_ESP_WAKE when it is already awake.
 */
#include "picolink.h"
#include "board.h"
#include "state.h"
#include "radio.h"
#include "proto.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "picolink";

#define RX_BUF_SIZE 512
#define TX_BUF_SIZE 512

static proto_rx_t       s_rx;
static volatile int64_t s_last_rx_us = 0;
static SemaphoreHandle_t s_tx_lock;

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

bool picolink_pico_awake(void)
{
    return (now_ms() - s_last_rx_us / 1000) < PICO_ASSUMED_ASLEEP_MS;
}

static void raw_send(uint8_t type, const uint8_t *p, uint8_t n)
{
    uint8_t f[PROTO_MAX_PAYLOAD + 4];
    size_t k = proto_build(f, type, p, n);
    uart_write_bytes(UART_PICO, (const char *)f, k);
}

void picolink_send(uint8_t type, const uint8_t *p, uint8_t n)
{
    xSemaphoreTake(s_tx_lock, portMAX_DELAY);

    if (!picolink_pico_awake() && type != MSG_ESP_WAKE) {
        for (int i = 0; i < WAKE_PREAMBLE_FRAMES; i++)
            raw_send(MSG_ESP_WAKE, NULL, 0);
        uart_wait_tx_done(UART_PICO, pdMS_TO_TICKS(100));
        vTaskDelay(pdMS_TO_TICKS(WAKE_SETTLE_MS));
    }

    raw_send(type, p, n);
    xSemaphoreGive(s_tx_lock);
}

static void handle_frame(uint8_t type, const uint8_t *p, uint8_t n)
{
    switch (type) {
    case MSG_ESP_STATE:
        state_set_amp(p, n);
        radio_push_state();          /* knob sees it on its next check-in */
        break;

    case MSG_ESP_NOW:
        /* Now-playing, originated by the Pi and passed straight through
         * the Pico.  Cache it so a knob waking up gets an answer without
         * a round trip. */
        state_set_nowplaying(p, n);
        radio_push_nowplaying();
        break;

    default:
        break;
    }
}

static void rx_task(void *arg)
{
    (void)arg;
    uint8_t buf[128];

    for (;;) {
        int n = uart_read_bytes(UART_PICO, buf, sizeof buf, pdMS_TO_TICKS(50));
        if (n <= 0) continue;
        s_last_rx_us = esp_timer_get_time();
        for (int i = 0; i < n; i++)
            if (proto_rx_byte(&s_rx, buf[i]))
                handle_frame(s_rx.type, s_rx.buf, s_rx.len);
    }
}

void picolink_init(void)
{
    const uart_config_t cfg = {
        .baud_rate  = UART_PICO_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(uart_driver_install(UART_PICO, RX_BUF_SIZE, TX_BUF_SIZE, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_PICO, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(UART_PICO, PIN_PICO_TX, PIN_PICO_RX,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    proto_rx_init(&s_rx);
    s_tx_lock = xSemaphoreCreateMutex();

    xTaskCreate(rx_task, "pico_rx", 3072, NULL, 10, NULL);
    ESP_LOGI(TAG, "UART%d up at %d baud", (int)UART_PICO, UART_PICO_BAUD);
}
