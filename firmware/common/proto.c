/* proto.c - frame codec shared by both UART links. */
#include "proto.h"

uint8_t proto_crc8(const uint8_t *d, size_t n)
{
    uint8_t c = 0x00;
    while (n--) {
        c ^= *d++;
        for (int i = 0; i < 8; i++)
            c = (c & 0x80) ? (uint8_t)((c << 1) ^ 0x31) : (uint8_t)(c << 1);
    }
    return c;
}

size_t proto_build(uint8_t *out, uint8_t type, const uint8_t *payload, uint8_t len)
{
    out[0] = PROTO_SOF;
    out[1] = len;
    out[2] = type;
    for (uint8_t i = 0; i < len; i++) out[3 + i] = payload[i];
    out[3 + len] = proto_crc8(&out[2], (size_t)len + 1);
    return (size_t)len + 4;
}

enum { S_SOF = 0, S_LEN, S_TYPE, S_DATA, S_CRC };

void proto_rx_init(proto_rx_t *r) { r->state = S_SOF; r->got = 0; }

bool proto_rx_byte(proto_rx_t *r, uint8_t b)
{
    switch (r->state) {
    case S_SOF:
        if (b == PROTO_SOF) r->state = S_LEN;
        break;
    case S_LEN:
        if (b > PROTO_MAX_PAYLOAD) { r->state = S_SOF; break; }
        r->len = b; r->got = 0; r->state = S_TYPE;
        break;
    case S_TYPE:
        r->type = b;
        r->state = r->len ? S_DATA : S_CRC;
        break;
    case S_DATA:
        r->buf[r->got++] = b;
        if (r->got >= r->len) r->state = S_CRC;
        break;
    case S_CRC: {
        uint8_t tmp[PROTO_MAX_PAYLOAD + 1];
        tmp[0] = r->type;
        for (uint8_t i = 0; i < r->len; i++) tmp[1 + i] = r->buf[i];
        r->state = S_SOF;
        return proto_crc8(tmp, (size_t)r->len + 1) == b;
    }
    default:
        r->state = S_SOF;
        break;
    }
    return false;
}

static const char *fn_names[FN_COUNT] = {
    "-", "Vol +", "Vol -", "Mute", "Power", "Input", "EQ bypass",
    "Prev", "Next", "Play/Pause", "Menu", "OK", "Back", "Up", "Down",
    "Preset 1", "Preset 2", "Preset 3"
};
const char *fn_name(fn_id_t f)
{
    return (f < FN_COUNT) ? fn_names[f] : "?";
}
