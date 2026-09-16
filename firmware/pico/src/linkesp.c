/* linkesp.c - UART1 link to the on-board ESP32-C3.
 *
 * The C3 is the ESP-NOW endpoint for the battery knob remote.  It stays
 * powered from +5VA whether or not the amplifier is on, so it is also
 * the thing that can wake the Pico out of standby.
 *
 * Waking matters for the framing: when the Pico is asleep its UART is
 * clocked down, so the byte that wakes it is lost.  The C3 therefore
 * sends a short train of MSG_ESP_WAKE preamble bytes, waits, and only
 * then sends the real frame.  See power.c for the other half.
 */
#include "linkesp.h"
#include "board.h"
#include "audio.h"
#include "power.h"
#include "settings.h"
#include "linkpi.h"
#include "dispatch.h"
#include "pico/stdlib.h"
#include "hardware/uart.h"
#include "hardware/gpio.h"
#include <string.h>

static proto_rx_t s_rx;
static uint32_t   s_last_rx_ms = 0;

static void tx(uint8_t type, const uint8_t *p, uint8_t n)
{
    uint8_t f[PROTO_MAX_PAYLOAD + 4];
    size_t k = proto_build(f, type, p, n);
    uart_write_blocking(UART_ESP, f, k);
}

void linkesp_init(void)
{
    uart_init(UART_ESP, UART_ESP_BAUD);
    gpio_set_function(PIN_ESP_TX, GPIO_FUNC_UART);
    gpio_set_function(PIN_ESP_RX, GPIO_FUNC_UART);
    uart_set_hw_flow(UART_ESP, false, false);
    uart_set_format(UART_ESP, 8, 1, UART_PARITY_NONE);
    uart_set_fifo_enabled(UART_ESP, true);
    proto_rx_init(&s_rx);
}

void linkesp_send_state(void)
{
    const audio_state_t *a = audio_get();
    uint8_t p[6];
    p[0] = (uint8_t)power_state();
    p[1] = a->vol_code;
    p[2] = a->muted ? 1 : 0;
    p[3] = a->input;
    p[4] = a->eq_path;
    p[5] = settings()->max_vol;
    tx(MSG_ESP_STATE, p, 6);
}

void linkesp_forward_nowplaying(const uint8_t *p, uint8_t n)
{
    tx(MSG_ESP_NOW, p, n);
}

static void handle_remote(const uint8_t *p, uint8_t n)
{
    if (n < 2) return;

    /* A rotary source sends a signed detent count in the third byte.
     * Everything else is an ordinary key. */
    if (n >= 3 && (int8_t)p[2] != 0)
        dispatch_volume_delta((int8_t)p[2]);
    else
        dispatch_fn((fn_id_t)p[0], p[1] != 0);

    linkesp_send_state();
}

void linkesp_poll(void)
{
    while (uart_is_readable(UART_ESP)) {
        uint8_t b = uart_getc(UART_ESP);
        if (!proto_rx_byte(&s_rx, b)) continue;
        s_last_rx_ms = to_ms_since_boot(get_absolute_time());

        switch (s_rx.type) {
        case MSG_ESP_REMOTE: handle_remote(s_rx.buf, s_rx.len); break;
        case MSG_ESP_HELLO:  linkesp_send_state();              break;
        case MSG_ESP_WAKE:   /* preamble only - waking is the point */ break;
        default: break;
        }
    }
}

bool linkesp_alive(void)
{
    return (to_ms_since_boot(get_absolute_time()) - s_last_rx_ms) < 30000;
}

void linkesp_arm_wake(bool en)
{
    /* UART RX idles high; a start bit pulls it low.  In standby we hand
     * the pin back to the GPIO block so an edge can wake the core. */
    if (en) {
        gpio_set_function(PIN_ESP_RX, GPIO_FUNC_SIO);
        gpio_set_dir(PIN_ESP_RX, GPIO_IN);
        gpio_set_irq_enabled(PIN_ESP_RX, GPIO_IRQ_EDGE_FALL, true);
    } else {
        gpio_set_irq_enabled(PIN_ESP_RX, GPIO_IRQ_EDGE_FALL, false);
        gpio_set_function(PIN_ESP_RX, GPIO_FUNC_UART);
    }
}
