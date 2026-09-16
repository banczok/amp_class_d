/* irrx.c - multi-protocol IR receiver with a learning-friendly output.
 *
 * The receiver on J11 is a TSOP-class demodulator: the output idles HIGH
 * and pulls LOW while a 38 kHz burst is present.  We timestamp both
 * edges in the GPIO interrupt and decode in the main loop once a long
 * idle gap closes the frame.
 *
 * Rather than record raw timings, the decoders normalise to a
 * {protocol, address, command} triple.  That is what learning mode
 * stores, so a held button - which sends a payload-free repeat frame on
 * NEC, or the same code with an unchanged toggle bit on RC5 - maps back
 * onto the same learned key instead of creating a second entry.
 *
 * Covered: NEC and NEC-extended, RC5 and RC5X, Sony SIRC 12/15/20.
 * Adding another family means one more decode function and an enum slot.
 */
#include "irrx.h"
#include "board.h"
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/sync.h"

#define IR_MAX_EDGES 200
#define IR_GAP_US    12000u

typedef struct { uint16_t us; uint8_t level; } edge_t;

static volatile edge_t   s_edge[IR_MAX_EDGES];
static volatile uint16_t s_n = 0;
static volatile uint32_t s_last_us = 0;
static volatile bool     s_frame_ready = false;

static ir_key_t s_last_key;
static uint32_t s_last_key_ms = 0;
static uint8_t  s_last_toggle = 0xFF;

static void ir_isr(uint gpio, uint32_t events)
{
    (void)events;
    if (gpio != PIN_IR) return;
    uint32_t now = time_us_32();
    uint32_t dt  = now - s_last_us;
    s_last_us = now;

    if (dt > IR_GAP_US) {
        if (s_n > 8) { s_frame_ready = true; return; }
        s_n = 0;
    }
    if (s_frame_ready) return;
    if (s_n < IR_MAX_EDGES) {
        s_edge[s_n].us = (uint16_t)(dt > 65535 ? 65535 : dt);
        /* The interval that just ended had the inverse of the new level:
         * a mark is a LOW on this receiver. */
        s_edge[s_n].level = (uint8_t)(gpio_get(PIN_IR) ? 1 : 0);
        s_n++;
    }
}

void irrx_init(void)
{
    gpio_init(PIN_IR);
    gpio_set_dir(PIN_IR, GPIO_IN);
    gpio_disable_pulls(PIN_IR);
    gpio_set_irq_enabled_with_callback(PIN_IR,
        GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL, true, ir_isr);
    s_last_us = time_us_32();
}

static bool near_us(uint16_t v, uint16_t ref, int pct)
{
    int lo = (int)ref - (int)ref * pct / 100;
    int hi = (int)ref + (int)ref * pct / 100;
    return (int)v >= lo && (int)v <= hi;
}

static bool decode_nec(const edge_t *e, uint16_t n, ir_key_t *k, bool *rpt)
{
    if (n < 3) return false;
    if (!(e[0].level == 1 && near_us(e[0].us, 9000, 25))) return false;

    if (near_us(e[1].us, 2250, 25)) {
        *rpt = true;
        k->proto = IR_PROTO_NONE;
        return true;
    }
    if (!near_us(e[1].us, 4500, 25)) return false;
    if (n < 2 + 64) return false;

    uint32_t bits = 0;
    for (int i = 0; i < 32; i++) {
        const edge_t *m = &e[2 + i * 2];
        const edge_t *s = &e[3 + i * 2];
        if (m->level != 1 || !near_us(m->us, 560, 45)) return false;
        int bit;
        if      (near_us(s->us, 560,  50)) bit = 0;
        else if (near_us(s->us, 1690, 30)) bit = 1;
        else return false;
        bits |= ((uint32_t)bit) << i;
    }
    uint8_t a0 = (uint8_t)( bits        & 0xFF);
    uint8_t a1 = (uint8_t)((bits >> 8)  & 0xFF);
    uint8_t c0 = (uint8_t)((bits >> 16) & 0xFF);
    uint8_t c1 = (uint8_t)((bits >> 24) & 0xFF);
    /* NEC sends every byte followed by its complement. */
    if (c0 + c1 != 0xFF) return false;

    if (a0 + a1 == 0xFF) { k->proto = IR_PROTO_NEC; k->addr = a0; }
    else                    { k->proto = IR_PROTO_NEC_EXT; k->addr = (uint16_t)(a0 | (a1 << 8)); }
    k->cmd = c0;
    *rpt = false;
    return true;
}

static bool decode_rc5(const edge_t *e, uint16_t n, ir_key_t *k, bool *rpt)
{
    const int T = 889;
    if (n < 10) return false;

    uint8_t lvl[32];
    int h = 0;
    for (uint16_t i = 0; i < n && h < 30; i++) {
        int units;
        if      (near_us(e[i].us, (uint16_t)T,     35)) units = 1;
        else if (near_us(e[i].us, (uint16_t)(2*T), 25)) units = 2;
        else return false;
        while (units-- > 0 && h < 30) lvl[h++] = e[i].level;
    }
    if (h < 26) return false;

    uint16_t v = 0;
    int nb = 0;
    for (int i = 0; i + 1 < h && nb < 14; i += 2) {
        uint8_t a = lvl[i], b = lvl[i + 1];
        if      (a == 1 && b == 0) v = (uint16_t)((v << 1) | 1);
        else if (a == 0 && b == 1) v = (uint16_t)( v << 1);
        else return false;
        nb++;
    }
    if (nb < 14) return false;

    uint8_t s1 = (uint8_t)((v >> 13) & 1);
    uint8_t s2 = (uint8_t)((v >> 12) & 1);
    uint8_t tg = (uint8_t)((v >> 11) & 1);
    uint8_t ad = (uint8_t)((v >> 6)  & 0x1F);
    uint8_t cm = (uint8_t)( v        & 0x3F);
    if (!s1) return false;

    if (s2) { k->proto = IR_PROTO_RC5;  k->cmd = cm; }
    else    { k->proto = IR_PROTO_RC5X; k->cmd = (uint16_t)(cm | 0x40); }
    k->addr = ad;
    *rpt = (tg == s_last_toggle);
    s_last_toggle = tg;
    return true;
}

static bool decode_sirc(const edge_t *e, uint16_t n, ir_key_t *k, bool *rpt)
{
    if (n < 2 + 24) return false;
    if (!(e[0].level == 1 && near_us(e[0].us, 2400, 25))) return false;

    uint32_t bits = 0;
    int nb = 0;
    for (uint16_t i = 1; i + 1 < n && nb < 20; i += 2) {
        const edge_t *s = &e[i];
        const edge_t *m = &e[i + 1];
        if (!near_us(s->us, 600, 50)) break;
        if (m->level != 1) break;
        int bit;
        if      (near_us(m->us, 1200, 30)) bit = 1;
        else if (near_us(m->us, 600,  50)) bit = 0;
        else break;
        bits |= ((uint32_t)bit) << nb;
        nb++;
    }
    if      (nb == 12) { k->proto = IR_PROTO_SIRC12; k->cmd = bits & 0x7F; k->addr = (uint16_t)((bits >> 7) & 0x1F);   }
    else if (nb == 15) { k->proto = IR_PROTO_SIRC15; k->cmd = bits & 0x7F; k->addr = (uint16_t)((bits >> 7) & 0xFF);   }
    else if (nb == 20) { k->proto = IR_PROTO_SIRC20; k->cmd = bits & 0x7F; k->addr = (uint16_t)((bits >> 7) & 0x1FFF); }
    else return false;
    *rpt = false;
    return true;
}

bool irrx_poll(ir_key_t *out, bool *repeat)
{
    if (!s_frame_ready) {
        if (s_n > 8 && (time_us_32() - s_last_us) > IR_GAP_US) s_frame_ready = true;
        else return false;
    }

    uint32_t save = save_and_disable_interrupts();
    edge_t   buf[IR_MAX_EDGES];
    uint16_t n = s_n;
    if (n > IR_MAX_EDGES) n = IR_MAX_EDGES;
    for (uint16_t i = 0; i < n; i++) {
        buf[i].us    = s_edge[i].us;
        buf[i].level = s_edge[i].level;
    }
    s_n = 0;
    s_frame_ready = false;
    restore_interrupts(save);

    ir_key_t k;
    k.proto = IR_PROTO_NONE; k.addr = 0; k.cmd = 0;
    bool rpt = false;
    bool ok = decode_nec(buf, n, &k, &rpt)
           || decode_rc5(buf, n, &k, &rpt)
           || decode_sirc(buf, n, &k, &rpt);
    if (!ok) return false;

    uint32_t now = to_ms_since_boot(get_absolute_time());
    if (k.proto == IR_PROTO_NONE) {
        /* Bare NEC repeat frame carries no payload - reuse the previous
         * key, but only if it is recent enough to belong to it. */
        if (now - s_last_key_ms > 300) return false;
        k = s_last_key;
    }
    s_last_key    = k;
    s_last_key_ms = now;

    *out    = k;
    *repeat = rpt;
    return true;
}

static const char *pn[IR_PROTO_COUNT] = {
    "-", "NEC", "NEC-ext", "RC5", "RC5X", "SIRC12", "SIRC15", "SIRC20"
};
const char *ir_proto_name(uint8_t p)
{
    return (p < IR_PROTO_COUNT) ? pn[p] : "?";
}
