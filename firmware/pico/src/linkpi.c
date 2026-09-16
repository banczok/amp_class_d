/* linkpi.c - UART0 link to the Raspberry Pi, through the isolator.
 *
 * The Pi renders the 8.8" panel: the menu, the spectrum and the status
 * line all come from here.  What it does NOT do is own any state - the
 * Pico is authoritative for volume, input, EQ and power, and the Pi is a
 * display and a touch surface.  That is what makes the acceptance test
 * in the design doc pass: pull this cable and the encoder and IR still
 * work.
 *
 * 921600 baud.  The isolator is good for 100 Mbps so the wire is not the
 * limit; the meter frame is 40 bytes at ~47 Hz, which is nothing.
 */
#include "linkpi.h"
#include "board.h"
#include "audio.h"
#include "relays.h"
#include "settings.h"
#include "power.h"
#include "menu.h"
#include "linkesp.h"
#include "meter.h"
#include "pico/stdlib.h"
#include "hardware/uart.h"
#include "hardware/gpio.h"
#include <string.h>

static proto_rx_t s_rx;
static uint32_t   s_last_rx_ms = 0;
static uint8_t    s_audio_seq = 0;
static uint32_t   s_audio_until = 0;   /* 0 = until told otherwise */

static void tx(uint8_t type, const uint8_t *p, uint8_t n)
{
    uint8_t f[PROTO_MAX_PAYLOAD + 4];
    size_t k = proto_build(f, type, p, n);
    uart_write_blocking(UART_PI, f, k);
}

void linkpi_init(void)
{
    uart_init(UART_PI, UART_PI_BAUD);
    gpio_set_function(PIN_PI_TX, GPIO_FUNC_UART);
    gpio_set_function(PIN_PI_RX, GPIO_FUNC_UART);
    uart_set_hw_flow(UART_PI, false, false);
    uart_set_format(UART_PI, 8, 1, UART_PARITY_NONE);
    uart_set_fifo_enabled(UART_PI, true);
    proto_rx_init(&s_rx);
}

void linkpi_send_state(void)
{
    const audio_state_t *a = audio_get();
    uint8_t p[20];
    int i = 0;
    p[i++] = (uint8_t)power_state();
    p[i++] = a->vol_code;
    p[i++] = (uint8_t)a->balance;
    p[i++] = a->muted ? 1 : 0;
    p[i++] = a->input;
    p[i++] = a->eq_path;
    p[i++] = (uint8_t)a->tone_gain[0];
    p[i++] = (uint8_t)a->tone_gain[1];
    p[i++] = (uint8_t)a->tone_gain[2];
    p[i++] = a->tone_f0[0]; p[i++] = a->tone_f0[1]; p[i++] = a->tone_f0[2];
    p[i++] = a->tone_q[0];  p[i++] = a->tone_q[1];  p[i++] = a->tone_q[2];
    p[i++] = a->loudness;
    p[i++] = (uint8_t)audio_boost_headroom();   /* panel greys out above */
    p[i++] = settings()->max_vol;
    tx(MSG_STATE, p, (uint8_t)i);
}

void linkpi_send_meter(const meter_result_t *m)
{
    /* 4 + 64 = 68 bytes at ~47 Hz, about 3.2 kB/s.  Stereo bands cost 32
     * bytes a frame more than mono did, which is nothing next to the
     * audio stream and means the cheap always-on feed can drive every
     * visualiser on the panel without turning that stream on at all. */
    uint8_t p[4 + 2 * METER_BANDS];
    int i = 0;
    p[i++] = (uint8_t)(m->peak_l >> 3); p[i++] = (uint8_t)(m->peak_r >> 3);
    p[i++] = (uint8_t)(m->rms_l  >> 3); p[i++] = (uint8_t)(m->rms_r  >> 3);
    for (int b = 0; b < METER_BANDS; b++) p[i++] = m->band_l[b];
    for (int b = 0; b < METER_BANDS; b++) p[i++] = m->band_r[b];
    tx(MSG_METER, p, (uint8_t)i);
}

void linkpi_send_menu(void)
{
    uint8_t p[PROTO_MAX_PAYLOAD];
    uint8_t n = menu_serialise(p, sizeof p);
    if (n) tx(MSG_MENU, p, n);
}

void linkpi_send_event(uint8_t kind, uint8_t a, uint8_t b)
{
    uint8_t p[3] = { kind, a, b };
    tx(MSG_EVENT, p, 3);
}

void linkpi_log(const char *s)
{
    size_t n = strlen(s);
    if (n > PROTO_MAX_PAYLOAD) n = PROTO_MAX_PAYLOAD;
    tx(MSG_LOG, (const uint8_t *)s, (uint8_t)n);
}

void linkpi_send_irlearn(uint8_t fn, uint8_t status, uint8_t proto,
                         uint16_t addr, uint16_t cmd)
{
    uint8_t p[7] = { fn, status, proto,
                     (uint8_t)(addr & 0xFF), (uint8_t)(addr >> 8),
                     (uint8_t)(cmd  & 0xFF), (uint8_t)(cmd  >> 8) };
    tx(MSG_IRLEARN, p, 7);
}

static void handle_cmd(const uint8_t *p, uint8_t n)
{
    if (n < 3) return;
    uint8_t id = p[0];
    int16_t v  = (int16_t)((uint16_t)p[1] | ((uint16_t)p[2] << 8));

    switch (id) {
    case CMD_POWER:       power_request(v ? POWER_ON : POWER_STANDBY); break;
    case CMD_VOLUME:      audio_set_volume(v);                        break;
    case CMD_VOLUME_REL:  audio_volume_step(v);                       break;
    case CMD_MUTE:        (v == 2) ? audio_toggle_mute() : audio_set_mute(v != 0); break;
    case CMD_INPUT:       audio_set_input((uint8_t)v);                break;
    case CMD_EQ_BYPASS:   audio_set_eq_path((uint8_t)v);              break;
    case CMD_BALANCE:     audio_set_balance(v);                       break;
    case CMD_BASS_GAIN:   audio_set_tone(BD_BASS,   v);               break;
    case CMD_MID_GAIN:    audio_set_tone(BD_MID,    v);               break;
    case CMD_TREBLE_GAIN: audio_set_tone(BD_TREBLE, v);               break;
    case CMD_LOUDNESS:    audio_set_loudness((uint8_t)v, audio_get()->loudness_f0); break;
    case CMD_ENTER_MENU:  menu_open();                                break;
    case CMD_IR_LEARN:    menu_start_ir_learn((fn_id_t)v);            break;
    case CMD_AUDIO_MODE:
        meter_audio_stereo(v != 0);
        break;
    case CMD_AUDIO_STREAM:
        /* 0 off, 1 continuous, anything larger is a timed capture in
         * hundreds of milliseconds - which is what the Pi actually wants
         * for a fingerprint, and means a crashed Pi cannot leave 26% of
         * the link running forever. */
        meter_audio_enable(v != 0);
        s_audio_until = (v > 1)
            ? to_ms_since_boot(get_absolute_time()) + (uint32_t)v * 100u
            : 0;
        break;
    default: break;
    }
    linkpi_send_state();
}

void linkpi_poll(void)
{
    while (uart_is_readable(UART_PI)) {
        uint8_t b = uart_getc(UART_PI);
        if (!proto_rx_byte(&s_rx, b)) continue;
        s_last_rx_ms = to_ms_since_boot(get_absolute_time());

        switch (s_rx.type) {
        case MSG_CMD:      handle_cmd(s_rx.buf, s_rx.len); break;
        case MSG_MENU_KEY: menu_key(s_rx.len ? s_rx.buf[0] : 0);
                           linkpi_send_menu();             break;
        case MSG_PING:     linkpi_send_state();            break;
        case MSG_HELLO:    power_note_pi_up();
                           linkpi_send_state();            break;
        case MSG_NOWPLAYING:
            /* Straight through to the ESP for the knob display; the
             * Pico has no use for it. */
            linkesp_forward_nowplaying(s_rx.buf, s_rx.len);
            break;
        default: break;
        }
    }
}

void linkpi_pump_audio(void)
{
    if (!meter_audio_enabled()) return;

    if (s_audio_until && to_ms_since_boot(get_absolute_time()) > s_audio_until) {
        meter_audio_enable(false);
        s_audio_until = 0;
        return;
    }

    /* Bounded per pass.  Each frame is 244 bytes, about 2.6 ms of
     * blocking write at 921600, and the control loop still has to run. */
    uint8_t p[2 + AUDIO_CHUNK];
    for (int i = 0; i < 4; i++) {
        /* meter_audio_read() guarantees whole L,R pairs in stereo, so a
         * frame boundary can never slip the interleave. */
        uint16_t n = meter_audio_read(&p[2], AUDIO_CHUNK);
        if (!n) return;
        p[0] = s_audio_seq++;
        p[1] = meter_audio_is_stereo() ? 1 : 0;
        tx(MSG_AUDIO, p, (uint8_t)(2 + n));
    }
}

bool linkpi_pi_alive(void)
{
    return (to_ms_since_boot(get_absolute_time()) - s_last_rx_ms) < 5000;
}
