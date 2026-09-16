/* proto.h - framed byte protocol shared by both UART links.
 *
 *   0xA5 | LEN | TYPE | payload[LEN] | CRC8
 *
 * CRC8 is Dallas/Maxim (poly 0x31, init 0x00) over TYPE and payload.
 * LEN counts payload bytes only, 0..250.
 *
 * The same framing runs on UART0 (Pi, 921600) and UART1 (ESP32, 115200);
 * only the message set differs.  Keeping one codec means one place to
 * get the escaping and resync right.
 */
#ifndef PROTO_H
#define PROTO_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define PROTO_SOF        0xA5
#define PROTO_MAX_PAYLOAD 250

/* ---- Pico -> Pi ---------------------------------------------------- */
#define MSG_STATE        0x01  /* full audio + power state               */
#define MSG_METER        0x02  /* levels and spectrum, ~50 Hz            */
#define MSG_MENU         0x03  /* menu to render on the 8.8" panel       */
#define MSG_EVENT        0x04  /* user input happened                    */
#define MSG_LOG          0x05  /* ASCII debug line                       */
#define MSG_IRLEARN      0x06  /* learning-mode progress                 */
#define MSG_AUDIO        0x07  /* decimated analogue audio, see below    */

/* ---- Pi -> Pico ---------------------------------------------------- */
#define MSG_CMD          0x81  /* set one field, see CMD_* below         */
#define MSG_NOWPLAYING   0x82  /* track metadata, relayed to the ESP     */
#define MSG_PING         0x83
#define MSG_MENU_KEY     0x84  /* panel touch acting as encoder/back     */
#define MSG_HELLO        0x85  /* Pi announces it is up                  */

/* ---- Pico <-> ESP32-C3 --------------------------------------------- */
#define MSG_ESP_STATE    0x10  /* brief state for the knob display       */
#define MSG_ESP_NOW      0x11  /* now-playing passthrough                */
#define MSG_ESP_REMOTE   0x90  /* key event from the battery remote      */
                               /* payload: [fn_id][repeat] and, for a    */
                               /* rotary source, [int8 detents] - a knob */
                               /* spun fast must not cost one frame per  */
                               /* click.                                 */
#define MSG_ESP_HELLO    0x91  /* ESP booted / link check                */
#define MSG_ESP_WAKE     0x92  /* preamble byte train, see power.c       */

/* ---- CMD_* field ids (MSG_CMD payload: [id][int16 value LE]) -------- */
#define CMD_POWER        0x01  /* 0 = standby, 1 = on                    */
#define CMD_VOLUME       0x02  /* absolute PGA code 0..192               */
#define CMD_VOLUME_REL   0x03  /* signed steps                           */
#define CMD_MUTE         0x04  /* 0/1, 2 = toggle                        */
#define CMD_INPUT        0x05  /* IN_SEL_DIGITAL / IN_SEL_ANALOG         */
#define CMD_EQ_BYPASS    0x06  /* EQ_PATH_ACTIVE / EQ_PATH_BYPASS        */
#define CMD_BALANCE      0x07  /* signed half-dB, -24..+24               */
#define CMD_BASS_GAIN    0x08  /* signed dB, clamped                     */
#define CMD_MID_GAIN     0x09
#define CMD_TREBLE_GAIN  0x0A
#define CMD_BASS_F0      0x0B  /* index 0..3                             */
#define CMD_MID_F0       0x0C
#define CMD_TREBLE_F0    0x0D
#define CMD_BASS_Q       0x0E
#define CMD_MID_Q        0x0F
#define CMD_TREBLE_Q     0x10
#define CMD_LOUDNESS     0x11  /* 0..15 dB, 0 = off                      */
#define CMD_PRESET_LOAD  0x12
#define CMD_PRESET_SAVE  0x13
#define CMD_ENTER_MENU   0x14
#define CMD_IR_LEARN     0x15  /* payload value = function id to learn   */
#define CMD_AUDIO_STREAM 0x16  /* 0 = off, 1 = on, N>1 = N*100ms then off */
#define CMD_AUDIO_MODE   0x17  /* 0 = mono, 1 = stereo interleaved        */

/* ---- MSG_AUDIO payload --------------------------------------------- *
 *   [0]   sequence, wraps at 256 - the Pi uses it to spot dropped frames
 *   [1]   0 = mono, 1 = stereo with samples interleaved L,R,L,R...
 *   [2..] G.711 mu-law samples at AUDIO_RATE_HZ
 *
 * Why this exists: the Pi cannot hear the analogue input.  It has no
 * converter on it and the amplifier does not route audio that way, so
 * without this the analogue side has no title, no artist and no cover -
 * and the waveform visualisers have nothing to draw.
 *
 * Why mu-law and not PCM: 8 bits per sample instead of 16, for a
 * fingerprint match that is completely indifferent to the difference.
 * 24 kB/s of a 92 kB/s link, rather than 48.
 *
 * Why 24 kHz and not 16: 16 kHz is what a fingerprinter wants, but it
 * leaves a spectrum display with nothing above 8 kHz.  24 kHz is one
 * clean halving of the 48 kHz the ADC already runs at, gives 12 kHz of
 * spectrum, and the Pi can decimate again for the fingerprinter.
 *
 * Mono is 24 kB/s, 26% of the link.  Stereo is 48 kB/s, 52% - which the
 * link carries comfortably, control traffic being under 3 kB/s, but
 * there is no reason to pay it for a fingerprint that gets summed to
 * mono anyway.  Hence the two modes: the visualisers that genuinely
 * need both channels ask for stereo, everything else does not.
 */
#define AUDIO_RATE_HZ    24000
#define AUDIO_CHUNK       240   /* samples per frame = 10 ms             */

/* ---- MSG_NOWPLAYING / MSG_ESP_NOW payload -------------------------- *
 *   [0] flags   bit0 = playing (else paused/stopped)
 *   [1] art_id  bumped by the Pi whenever the cover changes, 0 = none
 *   [2..] "artist\0title\0album\0", UTF-8
 *
 * Text goes over the radio because it is tiny and has to be on screen
 * before WiFi has even associated.  The cover does not: the knob pulls
 * that over HTTP, keyed by art_id, once it is awake and has an address.
 */
#define NP_FLAG_PLAYING  0x01

/* ---- MSG_IRLEARN status byte --------------------------------------- */
#define IRLEARN_WAITING  0  /* armed, waiting for a key                  */
#define IRLEARN_OK       1  /* captured and stored                       */
#define IRLEARN_FULL     2  /* map is full, nothing stored               */
#define IRLEARN_TIMEOUT  3  /* nobody pressed anything                   */
#define IRLEARN_CLEARED  4  /* binding for this function removed         */
#define IRLEARN_UNKNOWN  5  /* key seen that is bound to nothing         */

/* ---- remote / IR function ids (also used as the learn targets) ------ */
typedef enum {
    FN_NONE = 0,
    FN_VOL_UP,      FN_VOL_DOWN,   FN_MUTE,
    FN_POWER,       FN_INPUT,      FN_EQ_BYPASS,
    FN_PREV,        FN_NEXT,       FN_PLAY_PAUSE,
    FN_MENU,        FN_OK,         FN_BACK,
    FN_UP,          FN_DOWN,
    FN_PRESET_1,    FN_PRESET_2,   FN_PRESET_3,
    FN_COUNT
} fn_id_t;

extern const char *fn_name(fn_id_t f);

/* ---- codec --------------------------------------------------------- */

uint8_t proto_crc8(const uint8_t *d, size_t n);

/* Build a frame into out[] (must hold len+4).  Returns bytes written. */
size_t  proto_build(uint8_t *out, uint8_t type, const uint8_t *payload, uint8_t len);

/* Incremental receiver.  Feed bytes; returns true once per complete
 * frame, leaving type/payload/len valid until the next call. */
typedef struct {
    uint8_t  state;
    uint8_t  len, got, type;
    uint8_t  buf[PROTO_MAX_PAYLOAD];
} proto_rx_t;

void proto_rx_init(proto_rx_t *r);
bool proto_rx_byte(proto_rx_t *r, uint8_t b);

#endif /* PROTO_H */
